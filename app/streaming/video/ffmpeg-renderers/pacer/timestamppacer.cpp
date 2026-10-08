#include "timestamppacer.h"
#include "pacertelemetry.h"
#include "../../videothreadpriority.h"
#include "streaming/video/pacinglog.h"

#include <Limelight.h>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace {

// Long waits use the condition variable, so a new frame or a stop can cut
// them short. The last stretch is handed to the precise waiter.
constexpr uint64_t k_CoarseWakeUs = 2000;

// Frames whose release falls this close together are handed over together
constexpr uint64_t k_ReleaseSlackUs = 200;

// A target this far past readiness means the timeline is wrong, not that the
// frame should wait that long
constexpr uint64_t k_MaximumHoldUs = 100000;

}

TimestampPacer::TimestampPacer(const TimestampPacingOptions& options, int streamFps, int displayHz,
                               bool useVblankGrid, bool presentFlipsImmediately,
                               PacerTelemetry* telemetry, Callbacks callbacks) :
    m_Options(options.resolved()),
    m_StreamFps(streamFps),
    m_DisplayHz(displayHz),
    m_UseVblankGrid(useVblankGrid && displayHz > 0),
    m_PresentFlipsImmediately(presentFlipsImmediately),
    m_Telemetry(telemetry),
    m_Callbacks(std::move(callbacks)),
    m_Waiter(VrrTargetWaiterHooks{ [] { return LiGetMicroseconds(); }, {}, {} })
{
    m_Policy.configure(m_Options, streamFps);
    m_SourcePeriodUs = m_Policy.sourcePeriodUs();
    if (m_DisplayHz > 0) {
        m_Grid.configure(1000000.0 / m_DisplayHz);
    }
}

TimestampPacer::~TimestampPacer()
{
    stop();
}

const char* TimestampPacer::displayModeName(DisplayMode mode)
{
    switch (mode) {
    case DisplayMode::Unknown: return "unknown";
    case DisplayMode::FixedRefresh: return "fixed refresh";
    case DisplayMode::Adaptive: return "VRR";
    case DisplayMode::Tearing: return "tearing allowed";
    case DisplayMode::FrameLimited: return "frame limited (FIFO)";
    }
    return "unknown";
}

void TimestampPacer::setDisplayModeProbe(DisplayModeProbe probe)
{
    std::lock_guard<std::mutex> lock(m_Lock);
    m_DisplayModeProbe = std::move(probe);
}

bool TimestampPacer::start()
{
    try {
        m_Thread = std::thread(&TimestampPacer::run, this);
    }
    catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Timestamp pacer thread creation failed: %s", e.what());
        return false;
    }
    m_Telemetry->beginTimestampSession();
    return true;
}

void TimestampPacer::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Stopping = true;
    }
    m_Wake.notify_all();
    if (m_Thread.joinable()) {
        m_Thread.join();
    }

    std::lock_guard<std::mutex> lock(m_Lock);
    for (Entry& entry : m_Queue) {
        av_frame_free(&entry.frame);
    }
    m_Queue.clear();
}

QString TimestampPacer::describe() const
{
    static const char* const k_Smoothing[] = { "off", "light", "standard" };
    return QStringLiteral("smoothing %1, target %2%, buffer %3-%4 ms, %5%6")
        .arg(k_Smoothing[m_Options.smoothing])
        .arg(m_Options.targetPerMille / 10.0, 0, 'f', 1)
        .arg(m_Options.minBufferMs)
        .arg(m_Options.maxBufferMs)
        .arg(!m_UseVblankGrid ? QStringLiteral("no V-blank grid") :
             m_PresentFlipsImmediately ? QStringLiteral("V-blank grid at %1 Hz, handed over at the V-blank").arg(m_DisplayHz) :
             QStringLiteral("V-blank grid at %1 Hz, %2 ms margin")
                 .arg(m_DisplayHz)
                 .arg(m_Options.vsyncMarginUs / 1000.0, 0, 'f', 2))
        .arg(m_DisplayModeProbe ? QStringLiteral(", following the compositor's presentation mode") : QString());
}

void TimestampPacer::submit(PacedFrame&& paced)
{
    const uint64_t readyUs = paced.decodeCompleteUs() != 0 ? paced.decodeCompleteUs()
                                                           : paced.decoderOutputUs();
    const TimestampPacing::Policy::Decision decision =
        m_Policy.schedule(paced.timestampValid(), paced.frameNumber(), paced.rtpTimestamp(),
                          readyUs, paced.hostLatencyUs(), paced.decoderQueueUs());

    if (PacingLog::active()) {
        PacingLog::Record record;
        record.event = PacingLog::Event::Scheduled;
        record.eventUs = LiGetMicroseconds();
        record.frameNumber = paced.frameNumber();
        record.rtpValid = paced.timestampValid();
        record.rtpTimestamp = paced.rtpTimestamp();
        record.decoderOutputUs = paced.decoderOutputUs();
        record.targetUs = decision.targetUs;
        record.bufferUs = decision.bufferUs;
        record.latenessUs = decision.latenessUs;
        record.repeat = decision.repeat;
        PacingLog::record(record);
    }

    Entry entry;
    entry.frame = paced.release();
    entry.paced = decision.paced;
    entry.targetUs = decision.paced ? std::min(decision.targetUs, readyUs + k_MaximumHoldUs) : 0;

    AVFrame* evicted = nullptr;
    bool late = false;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_Stopping) {
            av_frame_free(&entry.frame);
            return;
        }

        m_SourcePeriodUs = m_Policy.sourcePeriodUs();
        // Ready too late to be handed over before its target
        late = entry.paced && entry.targetUs < readyUs + m_RenderLeadUs;

        if (m_Queue.size() >= MaxQueuedFrames) {
            evicted = m_Queue.front().frame;
            m_Queue.pop_front();
            if (!m_WarnedQueueFull) {
                m_WarnedQueueFull = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Timestamp pacing: queue full; the buffer is limited to %zu waiting frames",
                            MaxQueuedFrames);
            }
        }
        m_Queue.push_back(entry);
    }
    m_Wake.notify_one();

    if (evicted != nullptr) {
        m_Callbacks.drop(evicted, true);
    }

    TimestampScheduleSample sample;
    sample.paced = decision.paced;
    sample.admitted = decision.admitted;
    sample.late = late;
    sample.latenessUs = decision.latenessUs;
    sample.bufferUs = decision.bufferUs;
    sample.sourcePeriodUs = uint64_t(m_Policy.sourcePeriodUs());
    sample.correctionUs = int64_t(std::llround(decision.correctionUs));
    sample.reanchored = decision.reanchored;
    sample.hostJerkUs = int64_t(std::llround(decision.hostJerkUs));
    sample.hostJerkValid = decision.hostJerkValid;
    m_Telemetry->recordTimestampSchedule(sample);
}

void TimestampPacer::onVsync(uint64_t atUs)
{
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Grid.observe(atUs);
}

void TimestampPacer::onDisplayEvent(uint64_t displayUs, uint64_t refreshPeriodUs)
{
    std::lock_guard<std::mutex> lock(m_Lock);
    // Display times off a fixed refresh grid would only mislead it
    if (m_DisplayMode != DisplayMode::Unknown && m_DisplayMode != DisplayMode::FixedRefresh) {
        return;
    }
    // The compositor's own refresh period beats the nominal display mode,
    // which under Gamescope need not match the panel
    if (refreshPeriodUs != 0 &&
            std::fabs(double(refreshPeriodUs) - m_Grid.nominalPeriodUs()) > m_Grid.nominalPeriodUs() * 0.01) {
        m_Grid.configure(double(refreshPeriodUs));
    }
    m_Grid.observe(displayUs);

    // Match the display time to the release planned for the nearest V-blank
    // at or before it. Shown a refresh or more after that V-blank, it missed.
    const double periodUs = m_Grid.periodUs();
    Release* match = nullptr;
    for (Release& release : m_Releases) {
        if (release.vblankUs == 0 || release.releaseUs > displayUs ||
                double(release.vblankUs) > displayUs + periodUs / 2 ||
                double(release.vblankUs) < displayUs - periodUs * 1.5) {
            continue;
        }
        if (match == nullptr || release.vblankUs > match->vblankUs) {
            match = &release;
        }
    }
    if (match == nullptr) {
        return;
    }
    const bool missed = double(displayUs) - double(match->vblankUs) > periodUs / 2;
    match->vblankUs = 0;

    if (missed) {
        m_ExtraMarginUs = std::min<uint64_t>(6000, m_ExtraMarginUs + 500);
        m_MarginChangedUs = displayUs;
    }
    else if (m_ExtraMarginUs != 0 && displayUs - m_MarginChangedUs > 5000000) {
        m_ExtraMarginUs -= std::min<uint64_t>(250, m_ExtraMarginUs);
        m_MarginChangedUs = displayUs;
    }
    m_Telemetry->recordTimestampVblankResult(missed, m_ExtraMarginUs);
}

void TimestampPacer::refreshDisplayMode(std::unique_lock<std::mutex>& lock, uint64_t nowUs)
{
    if (!m_DisplayModeProbe || (m_DisplayModeCheckedUs != 0 && nowUs - m_DisplayModeCheckedUs < 250000)) {
        return;
    }
    m_DisplayModeCheckedUs = nowUs;

    // The probe may talk to the compositor, so never hold the lock over it
    const DisplayModeProbe probe = m_DisplayModeProbe;
    lock.unlock();
    const DisplayMode mode = probe();
    lock.lock();

    if (mode == m_DisplayMode) {
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Timestamp pacing: compositor presentation is now %s%s",
                displayModeName(mode),
                mode == DisplayMode::FixedRefresh || mode == DisplayMode::Unknown ?
                    " (frames placed on the V-blank grid)" : " (frames presented at their targets)");
    if (mode == DisplayMode::FrameLimited) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: Steam's frame limit forces FIFO presentation, which queues frames instead of replacing them. Turn the frame limit off for timestamp pacing.");
    }
    m_DisplayMode = mode;
    // Display times from another mode are not V-blank times. Start the grid
    // and the lock over.
    m_Grid.configure(m_Grid.nominalPeriodUs());
    m_PhaseLock.reset();
    m_Releases = {};
    m_Telemetry->recordTimestampDisplayMode(uint8_t(mode));
}

bool TimestampPacer::gridUsableLocked(uint64_t nowUs) const
{
    return m_UseVblankGrid &&
           (m_DisplayMode == DisplayMode::Unknown || m_DisplayMode == DisplayMode::FixedRefresh) &&
           m_Grid.valid(nowUs);
}

void TimestampPacer::notePresented(uint64_t presentUs)
{
    uint64_t sampleUs, leadUs, plannedPresentUs;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_LastReleaseUs == 0 || presentUs < m_LastReleaseUs || presentUs - m_LastReleaseUs > 50000) {
            return;
        }

        // Rise quickly to a slow frame and fall back slowly, so the lead
        // covers most frames rather than the average one
        sampleUs = presentUs - m_LastReleaseUs;
        if (sampleUs > m_RenderLeadUs) {
            m_RenderLeadUs += (sampleUs - m_RenderLeadUs) / 4;
        }
        else {
            m_RenderLeadUs -= (m_RenderLeadUs - sampleUs) / 100;
        }
        m_RenderLeadUs = std::max<uint64_t>(200, std::min<uint64_t>(8000, m_RenderLeadUs));
        leadUs = m_RenderLeadUs;
        plannedPresentUs = m_PlannedPresentUs;
        m_LastReleaseUs = 0;
        m_PlannedPresentUs = 0;
    }

    m_Telemetry->recordTimestampPresent(plannedPresentUs != 0,
                                        int64_t(presentUs) - int64_t(plannedPresentUs),
                                        sampleUs, leadUs);
}

size_t TimestampPacer::queueDepth()
{
    std::lock_guard<std::mutex> lock(m_Lock);
    return m_Queue.size();
}

uint64_t TimestampPacer::leadUsLocked(bool vblankGrid) const
{
    if (!vblankGrid) {
        return m_RenderLeadUs;
    }
    // A present that flips at once must come just after the V-blank, so
    // the tear lands where it can't be seen
    return m_PresentFlipsImmediately ? 0 : m_RenderLeadUs + uint64_t(m_Options.vsyncMarginUs) + m_ExtraMarginUs;
}

uint64_t TimestampPacer::releaseTimeLocked(const Entry& entry, bool vblankGrid) const
{
    // Repeats and frames without a timestamp go as soon as everything ahead
    // of them has
    if (!entry.paced) {
        return 0;
    }

    const uint64_t leadUs = leadUsLocked(vblankGrid);
    const uint64_t showUs = vblankGrid ? m_PhaseLock.assign(entry.targetUs, m_Grid) : entry.targetUs;
    return showUs > leadUs ? showUs - leadUs : 0;
}

void TimestampPacer::run()
{
    const VideoThreadPriority priority("TimestampPacer", VideoThreadPriority::Role::Deadline);

    std::unique_lock<std::mutex> lock(m_Lock);
    while (!m_Stopping) {
        refreshDisplayMode(lock, LiGetMicroseconds());
        if (m_Stopping) {
            break;
        }
        if (m_Queue.empty()) {
            // Wake at least as often as the display mode is polled
            m_Wake.wait_for(lock, std::chrono::milliseconds(250));
            continue;
        }

        const uint64_t nowUs = LiGetMicroseconds();
        const bool vblankGrid = gridUsableLocked(nowUs);
        const uint64_t releaseUs = releaseTimeLocked(m_Queue.front(), vblankGrid);

        if (releaseUs > nowUs + k_CoarseWakeUs) {
            m_Wake.wait_for(lock, std::chrono::microseconds(releaseUs - nowUs - k_CoarseWakeUs));
            continue;
        }
        if (releaseUs > nowUs) {
            lock.unlock();
            const VrrTargetWaitResult wait = m_Waiter.waitUntil(releaseUs, m_WakeLeadUs);
            if (wait.schedulerDelayValid) {
                learnWakeLead(wait.schedulerDelayUs);
            }
            lock.lock();
            if (m_Stopping || m_Queue.empty()) {
                continue;
            }
        }

        releaseDueLocked(lock, vblankGrid);
    }
}

void TimestampPacer::learnWakeLead(uint64_t schedulerDelayUs)
{
    m_SchedulerDelays[m_NextSchedulerDelay] = schedulerDelayUs;
    m_NextSchedulerDelay = (m_NextSchedulerDelay + 1) % m_SchedulerDelays.size();
    m_SchedulerDelayCount = std::min(m_SchedulerDelayCount + 1, m_SchedulerDelays.size());

    // The 95th percentile of recent oversleeps, as VRR Pacing Mode learns it
    std::array<uint64_t, SchedulerSamples> sorted {};
    std::copy_n(m_SchedulerDelays.begin(), m_SchedulerDelayCount, sorted.begin());
    const size_t rank = (95 * (m_SchedulerDelayCount - 1) + 50) / 100;
    std::nth_element(sorted.begin(), sorted.begin() + rank, sorted.begin() + m_SchedulerDelayCount);
    m_WakeLeadUs = std::min(VrrTargetWaiter::kMaximumAdditionalWakeLeadUs, sorted[rank]);
}

void TimestampPacer::releaseDueLocked(std::unique_lock<std::mutex>& lock, bool vblankGrid)
{
    const uint64_t nowUs = LiGetMicroseconds();

    // The front frame is due. Later frames due at the same time join it: on
    // the V-blank grid, those assigned to the same V-blank (or an earlier
    // one), and otherwise those whose release has also come. Only the newest
    // of them is shown, as a mailbox swapchain would.
    const Entry& front = m_Queue.front();
    const uint64_t frontVblankUs = vblankGrid && front.paced ? m_PhaseLock.assign(front.targetUs, m_Grid) : 0;
    size_t dueCount = 1;
    for (size_t i = 1; i < m_Queue.size(); i++) {
        const Entry& entry = m_Queue[i];
        bool due;
        if (!entry.paced) {
            due = true;
        }
        else if (vblankGrid && frontVblankUs != 0) {
            due = m_PhaseLock.assign(entry.targetUs, m_Grid) <= frontVblankUs;
        }
        else {
            due = releaseTimeLocked(entry, vblankGrid) <= nowUs + k_ReleaseSlackUs;
        }
        if (!due) {
            break;
        }
        dueCount = i + 1;
    }

    std::array<AVFrame*, MaxQueuedFrames> superseded {};
    size_t supersededCount = 0;
    for (size_t i = 0; i + 1 < dueCount; i++) {
        superseded[supersededCount++] = m_Queue.front().frame;
        m_Queue.pop_front();
    }
    const Entry shown = m_Queue.front();
    m_Queue.pop_front();

    // When the present should return: the planned release plus the expected
    // release-to-present time. Unpaced frames have no plan.
    const uint64_t plannedReleaseUs = releaseTimeLocked(shown, vblankGrid);
    m_PlannedPresentUs = shown.paced && plannedReleaseUs != 0 ? plannedReleaseUs + m_RenderLeadUs : 0;

    bool vblankWaitValid = false;
    int64_t vblankWaitUs = 0;
    if (vblankGrid && shown.paced) {
        const uint64_t vblankUs = m_PhaseLock.assign(shown.targetUs, m_Grid);
        vblankWaitValid = true;
        vblankWaitUs = int64_t(vblankUs) - int64_t(shown.targetUs);
        m_PhaseLock.update(shown.targetUs, vblankUs, m_Grid.periodUs(), m_SourcePeriodUs);
        m_Releases[m_NextRelease] = { nowUs, vblankUs };
        m_NextRelease = (m_NextRelease + 1) % m_Releases.size();
    }
    m_LastReleaseUs = nowUs;
    const int64_t trimUs = int64_t(m_PhaseLock.trimUs());

    lock.unlock();
    for (size_t i = 0; i < supersededCount; i++) {
        m_Callbacks.drop(superseded[i], false);
    }
    m_Callbacks.release(shown.frame);
    m_Telemetry->recordTimestampRelease(trimUs, vblankGrid, vblankWaitValid, vblankWaitUs);
    lock.lock();
}
