#include "pacer.h"
#include "fixedvsynctrace.h"
#include "path.h"
#include <QCryptographicHash>
#include <QVarLengthArray>
#include "vrrpacingworker.h"
#include "../ivrrframepresenter.h"
#include "streaming/streamutils.h"
#include "streaming/vrrratepolicy.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>

#include <utility>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3
static_assert(PACER_MAX_OUTSTANDING_FRAMES == MAX_QUEUED_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

Pacer::Pacer(IFFmpegRenderer* renderer) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_Shutdown(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_RendererAttributes(0),
    m_VsyncMode(StreamingPreferences::VSM_DEFAULT),
    m_Mailbox(false),
    m_Smooth(false),
    m_SkipsSinceTick(0),
    m_PrevTickRepeated(false),
    m_SmoothRepeat(false),
    m_RepeatRequested(false)
{

}

Pacer::~Pacer()
{
    shutdown();
}

void Pacer::shutdown()
{
    if (m_Shutdown) {
        return;
    }
    m_Shutdown = true;

    if (m_VrrWorker != nullptr) {
        // The VRR worker owns the renderer context and releases it from its
        // own thread after cancelling any prepared frame.
        m_VrrWorker.reset();
        return;
    }

    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);

    // Producers have stopped; flush the diagnostics file
    m_Trace.reset();
}

PacerTelemetrySnapshot Pacer::telemetrySnapshot() const
{
    return m_Telemetry.snapshot();
}

PacerTelemetryCounters Pacer::telemetryCounters() const
{
    return m_Telemetry.counters();
}

Vrr13::ReadinessWindow::Snapshot Pacer::vrrReadiness(bool* active) const
{
    return m_Telemetry.vrrReadiness(active);
}

PacerFrametimeStats Pacer::takeFrametimeStats()
{
    return m_Telemetry.takeFrametimeStats();
}

uint32_t Pacer::queueDepth()
{
    if (m_VrrWorker != nullptr) {
        return (uint32_t)m_VrrWorker->queueDepth();
    }

    // This lock is only ever held across an individual enqueue or dequeue, so
    // sampling it ten times a second costs nothing measurable.
    QMutexLocker lock(&m_FrameQueueLock);
    return (uint32_t)(m_PacingQueue.count() + m_RenderQueue.count());
}

void Pacer::renderOnMainThread()
{
    if (m_VrrWorker != nullptr) {
        return;
    }

    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty() && !me->m_RepeatRequested) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        if (me->m_RenderQueue.isEmpty()) {
            // Smooth: present the last frame again. It is only freed by the
            // next renderFrame() on this thread, so it is still valid.
            me->m_RepeatRequested = false;
            me->m_FrameQueueLock.unlock();
            if (me->m_DeferredFreeFrame != nullptr) {
                me->m_VsyncRenderer->renderFrame(me->m_DeferredFreeFrame);
            }
            continue;
        }
        // A new frame replaces any repeat that was still waiting
        me->m_RepeatRequested = false;

        AVFrame* frame = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    if (m_Mailbox) {
        replaceQueuedFramesForMailbox(m_RenderQueue);
    }
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    const uint64_t tickUs = LiGetMicroseconds();
    if (m_Smooth) {
        handleSmoothVsync(tickUs);
        return;
    }

    m_FrameQueueLock.lock();
    const int queueBefore = m_PacingQueue.count();
    uint32_t skipped = 0;

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark. Mailbox never tolerates a backlog: only
    // the newest frame is eligible at each V-sync.
    if (!m_Mailbox && m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead. These drops are reported with
    // this refresh.
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();
        skipped++;

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS) ||
                m_Stopping || m_PacingQueue.isEmpty()) {
            // Wait timed out - unlock and bail. The previous frame stays up.
            m_FrameQueueLock.unlock();
            recordVsyncTick(tickUs, false, skipped, queueBefore, 0, nullptr, false);
            return;
        }
    }

    // Place the first frame on the render queue
    AVFrame* frame = m_PacingQueue.dequeue();
    const int queueAfter = m_PacingQueue.count();
    const ShownFrameInfo shownInfo(frame);
    enqueueFrameForRenderingAndUnlock(frame);

    recordVsyncTick(tickUs, true, skipped, queueBefore, queueAfter, &shownInfo, false);
}

Pacer::ShownFrameInfo::ShownFrameInfo(const AVFrame* frame)
    : rtp(frame->pts == AV_NOPTS_VALUE ? -1 : frame->pts),
      arrivalUs(frame->pkt_dts > 0 ? static_cast<uint64_t>(frame->pkt_dts) : 0),
      slotUs(frame->best_effort_timestamp > 0 ? static_cast<uint64_t>(frame->best_effort_timestamp) : 0)
{
}

void Pacer::handleSmoothVsync(uint64_t tickUs)
{
    m_FrameQueueLock.lock();

    // The previous refresh's frame never arrived in time: it was a repeat
    if (m_PendingSmoothTick.showUs != 0) {
        const PendingSmoothTick pending = m_PendingSmoothTick;
        m_PendingSmoothTick = PendingSmoothTick();
        resolveSmoothRepeatLocked(pending.tickUs, pending.queueBefore, false);
    }

    m_Smoother.observeVsync(tickUs);
    const uint64_t periodUs = m_Smoother.displayPeriodUs();
    // This decision is shown on the next refresh. Frames must reach the
    // renderer TIMER_SLACK_MS before it, like the default path.
    const uint64_t showUs = tickUs + periodUs;
    const uint64_t renderLeadUs = SDL_min((uint64_t)TIMER_SLACK_MS * 1000, periodUs / 2);
    const int queueBefore = m_PacingQueue.count();

    int chosen = findSmoothFrameLocked(showUs);

    // A target far in the future means the timeline jumped (for example
    // the host clock reset). Start over rather than stall the stream.
    if (chosen < 0 && !m_PacingQueue.isEmpty() &&
            static_cast<uint64_t>(m_PacingQueue.head()->best_effort_timestamp) > showUs + 250000) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Smooth V-Sync: frame scheduled too far ahead; resetting timeline");
        m_Smoother.reset();
        chosen = m_PacingQueue.count() - 1;
    }

    if (chosen >= 0) {
        sendSmoothFrameAndUnlock(chosen, tickUs, showUs, queueBefore);
        return;
    }

    if (m_Stopping) {
        recordVsyncTick(tickUs, false, 0, queueBefore, queueBefore, nullptr, false);
    }
    else if (!m_PacingQueue.isEmpty()) {
        // The next frame is already here and belongs to a later refresh
        resolveSmoothRepeatLocked(tickUs, queueBefore, true);
    }
    else {
        // submitFrame() sends the frame if it arrives before the deadline
        m_PendingSmoothTick.tickUs = tickUs;
        m_PendingSmoothTick.showUs = showUs;
        m_PendingSmoothTick.deadlineUs = showUs - renderLeadUs;
        m_PendingSmoothTick.queueBefore = queueBefore;
    }
    m_FrameQueueLock.unlock();
}

void Pacer::resolveSmoothRepeatLocked(uint64_t tickUs, int queueBefore, bool present)
{
    recordVsyncTick(tickUs, false, 0, queueBefore, m_PacingQueue.count(), nullptr, false);
    if (present && m_SmoothRepeat) {
        m_RepeatRequested = true;
        m_RenderQueueNotEmpty.wakeOne();
    }
}

int Pacer::findSmoothFrameLocked(uint64_t showUs) const
{
    // Newest frame whose assigned refresh is this one or earlier. A frame
    // without a usable target (0) is always eligible.
    const uint64_t halfPeriodUs = m_Smoother.displayPeriodUs() / 2;
    int chosen = -1;
    for (int i = 0; i < m_PacingQueue.count(); i++) {
        const uint64_t target = static_cast<uint64_t>(m_PacingQueue.at(i)->best_effort_timestamp);
        if (target <= showUs + halfPeriodUs) {
            chosen = i;
        }
    }
    return chosen;
}

void Pacer::sendSmoothFrameAndUnlock(int chosen, uint64_t tickUs, uint64_t showUs,
                                     int queueBefore)
{
    // Older eligible frames missed their refresh; the newest one wins
    QVarLengthArray<AVFrame*, MAX_QUEUED_FRAMES> skippedFrames;
    for (int i = 0; i < chosen; i++) {
        skippedFrames.append(m_PacingQueue.dequeue());
    }
    AVFrame* frame = m_PacingQueue.dequeue();

    const ShownFrameInfo shownInfo(frame);
    const bool late = shownInfo.slotUs != 0 &&
            shownInfo.slotUs + m_Smoother.displayPeriodUs() / 2 < showUs;
    recordVsyncTick(tickUs, true, (uint32_t)skippedFrames.size(),
                    queueBefore, m_PacingQueue.count(), &shownInfo, late);

    enqueueFrameForRenderingAndUnlock(frame);

    for (AVFrame* skippedFrame : skippedFrames) {
        av_frame_free(&skippedFrame);
    }
}

void Pacer::recordVsyncTick(uint64_t tickUs, bool sent, uint32_t skipped,
                            int queueBefore, int queueAfter, const ShownFrameInfo* shown,
                            bool late)
{
    skipped += m_SkipsSinceTick.exchange(0);

    // The visible hitch: a refresh with nothing new, then a refresh that had
    // to discard a frame because two were ready.
    const bool missedSlot = m_PrevTickRepeated && sent && skipped != 0;
    m_PrevTickRepeated = !sent;

    int64_t offsetUs = 0;
    int64_t bufferUs = 0;
    uint64_t periodUs = 0;
    if (m_Smooth) {
        offsetUs = m_Smoother.offsetUs();
        bufferUs = m_Smoother.bufferUs();
        periodUs = m_Smoother.displayPeriodUs();
    }

    m_Telemetry.recordFixedVsyncTick(sent, skipped, missedSlot, late,
                                     m_Smooth, offsetUs, bufferUs);

    if (m_Trace != nullptr) {
        FixedVsyncTrace::Row row;
        row.event = "tick";
        row.timeUs = tickUs;
        if (shown != nullptr) {
            row.rtp = shown->rtp;
            row.arrivalUs = shown->arrivalUs;
            // Only Smooth stores a refresh in best_effort_timestamp
            row.slotUs = m_Smooth ? shown->slotUs : 0;
        }
        row.a = sent;
        row.b = skipped;
        row.c = queueBefore;
        row.d = queueAfter;
        row.e = missedSlot;
        row.f = late;
        row.offsetUs = offsetUs;
        row.displayPeriodUs = periodUs;
        m_Trace->record(row);
    }
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps,
                       bool enablePacing, bool enableVsync,
                       bool enableVrr, int vrrDisplayRefreshHz,
                       bool smoothVrrFrameTiming, const QString& calibrationKey,
                       int vrrLatencyMode, int vsyncMode)
{
    m_MaxVideoFps = maxVideoFps;
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    // VRR has its own queue and stale-frame policy. The session never asks
    // for both, but keep the fixed V-Sync modes out of the VRR path (and its
    // fallback) even if a caller does.
    m_VsyncMode = enableVsync && !enableVrr ? vsyncMode : StreamingPreferences::VSM_DEFAULT;
    m_Mailbox = m_VsyncMode == StreamingPreferences::VSM_MAILBOX;

    // VRR is deliberately a third pacing mode. It is selected once, before
    // any legacy V-sync source or render thread can be created, and every
    // rejection continues through the original fixed path below.
    if (enableVrr) {
        VrrSessionConfig config;
        // The production queue policy is shared across native backends.
        config.readinessHitchFeedback = false;
        config.latencyMode = vrrLatencyMode >= 0 && vrrLatencyMode <= 2 ? vrrLatencyMode : 1;
        config.streamRateHz = maxVideoFps;
        config.displayRefreshHz = vrrDisplayRefreshHz;
        config.smoothFrameTiming = smoothVrrFrameTiming;
        VrrFallbackReason fallbackReason = VrrFallbackReason::NoFallback;
        if (!calibrationKey.isEmpty()) {
            const QString display = QString::fromUtf8(SDL_GetDisplayName(SDL_GetWindowDisplayIndex(window)));
            auto context = calibrationKey + QString("|%1|%2|%3|%4")
                .arg(display).arg(maxVideoFps).arg(vrrDisplayRefreshHz)
                .arg(smoothVrrFrameTiming);
#ifdef Q_OS_LINUX
            // Do not seed the shared policy with retired Linux hitch-policy history.
            context += QStringLiteral("|shared-readiness-policy-v18");
#endif
            if (config.latencyMode != 0) {
                context += QStringLiteral("|latency-mode=%1").arg(config.latencyMode);
            }
            // Preserve the historical V2 calibration identity now that its
            // queue policy is unconditional rather than a live preference.
            context += QStringLiteral("|mean-miss-queue-v2");
            if (smoothVrrFrameTiming) {
                // The saved flag previously selected timestamp-following
                // playout too. Do not cross-seed its readiness calibration.
                const auto policy = vrrTimingParametersForSession(config);
                context += QStringLiteral("|frame-smoothing=%1-%2-%3|cadence=%4-%5")
                    .arg(policy.playoutSmoothingGainPerMille)
                    .arg(policy.playoutSmoothingPeriodAlphaPerMille)
                    .arg(policy.playoutSmoothingMaxLagUs)
                    .arg(policy.playoutSmoothingWindowedCadence)
                    .arg(policy.playoutSmoothingRecoveryUs);
                context += QStringLiteral("|catchup=%1").arg(policy.playoutCatchupPerMille);
                if (policy.playoutSmoothingReserveMaxUs != 0 ||
                        policy.playoutSmoothingPeriodFeedbackPerMillion != 0) {
                    context += QStringLiteral("|smoothing-reserve=%1-%2-%3-%4|period-feedback=%5")
                        .arg(policy.playoutSmoothingReserveMaxUs)
                        .arg(policy.playoutSmoothingReserveToleranceUs)
                        .arg(policy.playoutSmoothingReservePercentilePerMille)
                        .arg(policy.playoutSmoothingReserveReleaseUsPerSecond)
                        .arg(policy.playoutSmoothingPeriodFeedbackPerMillion);
                }
            }
            config.calibrationKey = QCryptographicHash::hash(context.toUtf8(), QCryptographicHash::Sha256).toHex().toStdString();
            config.calibrationPath = Path::getCacheFileInfo("vrr13-calibration.json").absoluteFilePath().toStdString();
        }
        // The retired extra-queue flag remains only for historical replay;
        // the timing profile controls delay and stale-frame replacement.
        config.allowAdditionalQueuedFrame = false;

        if (!enableVsync) {
            fallbackReason = VrrFallbackReason::IneffectiveVsync;
        }
        else if (config.displayRefreshHz <= 0) {
            fallbackReason = VrrFallbackReason::InvalidRefresh;
        }
        else if (!VrrRatePolicy::hasAdaptiveHeadroom(config.streamRateHz,
                                                     config.displayRefreshHz)) {
            fallbackReason = VrrFallbackReason::InsufficientHeadroom;
            IVrrFramePresenter* presenter =
                m_VsyncRenderer->getVrrFramePresenter();
            if (presenter != nullptr &&
                    presenter->checkSupport() == VrrFallbackReason::NoFallback &&
                    !presenter->restoreFixedPresentation(fallbackReason)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "VRR pacing lacks adaptive-refresh headroom and the presenter cannot restore fixed presentation");
                return false;
            }
        }
        else {
            IVrrFramePresenter* presenter =
                m_VsyncRenderer->getVrrFramePresenter();
            if (presenter == nullptr) {
                fallbackReason = VrrFallbackReason::UnsupportedRenderer;
            }
            else {
                fallbackReason = presenter->checkSupport();
                if (fallbackReason == VrrFallbackReason::NoFallback) {
                    m_VrrWorker = std::make_unique<VrrPacingWorker>(
                        presenter, config, &m_Telemetry);
                    if (m_VrrWorker->start()) {
                        m_DisplayFps = config.displayRefreshHz;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "VRR pacing: target %d Hz with %d FPS stream (adaptive timestamp playout, frame timing %s, timing profile %s)",
                                    m_DisplayFps, m_MaxVideoFps,
                                    config.smoothFrameTiming ? "smoothed" : "follows host timestamps",
                                    config.latencyMode == 2 ? "low latency" :
                                    config.latencyMode == 1 ? "balanced target" : "smooth");
                        return true;
                    }

                    fallbackReason = VrrFallbackReason::InitializationFailed;
                    if (!presenter->restoreFixedPresentation(fallbackReason)) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                     "VRR pacing worker failed to start and the presenter cannot restore fixed presentation");
                        m_VrrWorker.reset();
                        return false;
                    }

                    m_VrrWorker.reset();
                }
            }
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR pacing unavailable: %s; falling back to fixed V-sync pacing",
                    vrrFallbackReasonName(fallbackReason));

        // VRR requires V-sync at the session boundary, so its rejection still
        // has a valid fixed-pacing fallback even if the user did not select
        // the older frame-pacing checkbox.
        enablePacing = enablePacing || enableVsync;
    }

    // The VRR success path uses its strict session refresh snapshot and
    // returned above. Keep the legacy fallback query out of that path so it
    // cannot invent a 60 Hz value or produce an unrelated warning.
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);

    if (m_Mailbox) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "V-Sync mode: Mailbox (newest decoded frame replaces any frame still waiting)");
    }

    // Smooth needs the refresh clock only the Frame pacing path has
    if (m_VsyncMode == StreamingPreferences::VSM_SMOOTH) {
        enablePacing = true;
    }

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncMode == StreamingPreferences::VSM_SMOOTH) {
        if (m_VsyncSource != nullptr) {
            m_Smooth = true;
            m_Smoother.configure(m_MaxVideoFps, m_DisplayFps,
                                 (int64_t)TIMER_SLACK_MS * 1000);
            // Repeats run on the render thread, which only exists when the
            // renderer supports it
            m_SmoothRepeat = (m_RendererAttributes & RENDERER_ATTRIBUTE_REPEAT_FRAME) &&
                    m_VsyncRenderer->isRenderThreadSupported();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "V-Sync mode: Smooth (frames scheduled onto refreshes from host timestamps; %s)",
                        m_SmoothRepeat ? "empty refreshes re-present the last frame" :
                                         "empty refreshes present nothing");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Smooth V-Sync needs a V-sync source, which this platform lacks; using Default");
            m_VsyncMode = StreamingPreferences::VSM_DEFAULT;
        }
    }

    // Refresh accounting and the diagnostics trace need the V-sync thread
    if (m_VsyncSource != nullptr) {
        m_Telemetry.beginFixedVsync(m_VsyncMode);
        m_Trace = FixedVsyncTrace::create(m_VsyncMode, m_MaxVideoFps, m_DisplayFps);
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    if (m_VrrWorker != nullptr) {
        m_VrrWorker->notifyWindowChanged(info);
        return;
    }

    // Legacy pacing has no additional window-state work. The VRR worker
    // overrides this path to discard stale work on suspension/minimize.
}

void Pacer::renderFrame(AVFrame* frame)
{
    const uint64_t decoderOutputUs =
        static_cast<uint64_t>(frame->pkt_dts);
    const uint64_t beforeRender = LiGetMicroseconds();
    // Render it
    m_VsyncRenderer->renderFrame(frame);
    const uint64_t afterRender = LiGetMicroseconds();

    m_Telemetry.recordLegacyFrame(
        afterRender >= decoderOutputUs ?
            afterRender - decoderOutputUs : 0,
        afterRender >= beforeRender ? afterRender - beforeRender : 0,
        afterRender);

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_Mailbox) {
        // Enqueue already replaces waiting frames. Keep the newest one if a
        // frame arrived while this one was rendering.
        frameDropTarget = 1;
    }
    else if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_Telemetry.recordLegacyDrop();
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
        if (m_VsyncSource != nullptr) {
            // Reported, and counted as a drop, with the next refresh
            m_SkipsSinceTick.fetch_add(1);
        }
    }
}

void Pacer::replaceQueuedFramesForMailbox(QQueue<AVFrame*>& queue)
{
    while (!queue.isEmpty()) {
        AVFrame* frame = queue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        if (m_VsyncSource != nullptr) {
            // Reported, and counted as a drop, with the next refresh
            m_SkipsSinceTick.fetch_add(1);
        }
        else {
            m_Telemetry.recordLegacyDrop();
        }
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        if (m_Mailbox) {
            replaceQueuedFramesForMailbox(m_PacingQueue);
        }
        if (m_Smooth) {
            admitSmoothFrameLocked(frame);
        }
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        if (m_Smooth && !m_Stopping && m_PendingSmoothTick.showUs != 0) {
            // The last refresh is still waiting for its frame
            const PendingSmoothTick pending = m_PendingSmoothTick;
            const int chosen = findSmoothFrameLocked(pending.showUs);
            if (LiGetMicroseconds() + 500 < pending.deadlineUs) {
                m_PendingSmoothTick = PendingSmoothTick();
                if (chosen >= 0) {
                    sendSmoothFrameAndUnlock(chosen, pending.tickUs, pending.showUs,
                                             pending.queueBefore);
                    return;
                }
                // It belongs to a later refresh: the pending one is a repeat
                resolveSmoothRepeatLocked(pending.tickUs, pending.queueBefore, true);
            }
        }
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}

void Pacer::submitFrame(PacedFrame&& frame)
{
    if (m_VrrWorker != nullptr) {
        m_VrrWorker->submit(std::move(frame));
        return;
    }

    submitFrame(frame.release());
}

bool Pacer::isVrrActive() const
{
    return m_VrrWorker != nullptr;
}

int Pacer::fixedVsyncMode() const
{
    return m_VrrWorker != nullptr ? -1 : m_VsyncMode;
}

void Pacer::admitSmoothFrameLocked(AVFrame* frame)
{
    // best_effort_timestamp is unused by the renderers. Smooth mode keeps
    // the frame's assigned refresh there; 0 means "show when possible".
    frame->best_effort_timestamp = 0;
    if (frame->pts == AV_NOPTS_VALUE || frame->pkt_dts <= 0) {
        return;
    }

    const FixedVsyncSmoother::Decision d =
        m_Smoother.admit(static_cast<uint32_t>(frame->pts),
                         static_cast<uint64_t>(frame->pkt_dts));
    frame->best_effort_timestamp = static_cast<int64_t>(d.slotUs != 0 ? d.slotUs : d.dueUs);

    if (m_Trace != nullptr) {
        FixedVsyncTrace::Row row;
        row.event = "admit";
        row.timeUs = LiGetMicroseconds();
        row.rtp = frame->pts;
        row.arrivalUs = static_cast<uint64_t>(frame->pkt_dts);
        row.dueUs = d.dueUs;
        row.slotUs = d.slotUs;
        row.a = d.hostUs;
        row.b = d.smoothedHostUs;
        row.c = d.transitUs;
        row.d = d.desiredOffsetUs;
        row.e = d.resynced;
        row.f = d.rephased;
        row.offsetUs = d.offsetUs;
        row.displayPeriodUs = m_Smoother.displayPeriodUs();
        m_Trace->record(row);
    }
}
