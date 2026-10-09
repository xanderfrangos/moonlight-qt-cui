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

// A shorter wait for a frame's decode is the sync call itself, or driver
// contention after the decode finished (0.25-0.6 ms on a Deck)
constexpr uint64_t k_DecodeWaitThresholdUs = 1000;
// How much later than the learned decode delay a render may begin and still
// time a slower decode
constexpr uint64_t k_DecodeWindowSlackUs = 3000;
// Once a second, the learned decode delay falls 2% while the median wait of
// renders it held (begun no more than this window after it) is under the
// threshold. Replayed over six Deck and two Steam Machine captures
// (2026-10-09), renders begun after their decode finished had 1 s medians
// of 0.004-0.047 ms, while renders begun x ms before it had medians of
// about x.
constexpr uint64_t k_DecodeDecayWindowUs = 1000;
constexpr uint64_t k_DecodeDecayMedianUs = 150;

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
        m_RefreshPeriodUs = 1000000.0 / m_DisplayHz;
        m_Grid.configure(m_RefreshPeriodUs);
    }
}

TimestampPacer::~TimestampPacer()
{
    stop();
    // Whoever owns this has stopped the V-sync and render threads by now
    m_Trace.reset();
}

void TimestampPacer::trace(const TimestampTrace::Row& row)
{
    if (m_Trace != nullptr) {
        m_Trace->record(row);
    }
}

// The modes in which frames are shown at the display's refreshes
bool TimestampPacer::modeUsesGrid(DisplayMode mode)
{
    return mode == DisplayMode::Unknown || mode == DisplayMode::FixedRefresh || mode == DisplayMode::FrameLimited;
}

TimestampTrace::Row TimestampPacer::frameRow(TimestampTrace::Event event, const Entry& entry, uint64_t atUs)
{
    TimestampTrace::Row row;
    row.event = event;
    row.eventUs = atUs;
    row.frameNumber = entry.frameNumber;
    row.rtpValid = entry.rtpValid;
    row.rtpTimestamp = entry.rtpTimestamp;
    row.paced = entry.paced;
    row.targetUs = entry.targetUs;
    return row;
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
    // Before the pacing thread, and before the owner starts the V-sync and
    // render threads
    m_Trace = TimestampTrace::openIfRequested();
    if (m_Trace != nullptr) {
        TimestampTrace::Row row;
        row.event = TimestampTrace::Event::Session;
        row.eventUs = LiGetMicroseconds();
        row.sourcePeriodUs = uint64_t(m_SourcePeriodUs);
        row.renderLeadUs = m_RenderLeadUs;
        row.streamFps = m_StreamFps;
        row.displayHz = m_DisplayHz;
        row.smoothing = m_Options.smoothing;
        row.targetPerMille = m_Options.targetPerMille;
        row.minBufferMs = m_Options.minBufferMs;
        row.maxBufferMs = m_Options.maxBufferMs;
        row.vsyncMarginUs = m_Options.vsyncMarginUs;
        row.vblankGridAvailable = m_UseVblankGrid;
        row.presentFlipsImmediately = m_PresentFlipsImmediately;
        std::lock_guard<std::mutex> lock(m_Lock);
        row.compositorProbe = bool(m_DisplayModeProbe);
        row.measureRefresh = m_MeasureRefresh;
        row.gridPeriodUs = uint64_t(m_Grid.nominalPeriodUs());
        m_Trace->record(row);
    }

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
        .arg(m_DisplayModeProbe ? QStringLiteral(", following the compositor's presentation mode") :
             m_MeasureRefresh ? QStringLiteral(", measuring whether the display refreshes adaptively") : QString());
}

void TimestampPacer::submit(PacedFrame&& paced)
{
    // A frame is ready once its GPU decode has finished, which with VAAPI is
    // some time after the decoder returned it
    const uint64_t decodeDelayUs = m_DecodeDelayUs.load(std::memory_order_relaxed);
    const uint64_t outputReadyUs = paced.decodeCompleteUs() != 0 ? paced.decodeCompleteUs()
                                                                 : paced.decoderOutputUs();
    const uint64_t readyUs = paced.decoderOutputUs() != 0
        ? std::max(outputReadyUs, paced.decoderOutputUs() + decodeDelayUs) : outputReadyUs;
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
    entry.readyUs = readyUs;
    entry.targetUs = decision.paced ? std::min(decision.targetUs, readyUs + k_MaximumHoldUs) : 0;
    entry.frameNumber = paced.frameNumber();
    entry.rtpValid = paced.timestampValid();
    entry.rtpTimestamp = paced.rtpTimestamp();

    Entry evicted;
    bool late = false;
    bool warnQueueFull = false;
    uint64_t renderLeadUs = 0;
    uint32_t queueDepth = 0;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_Stopping) {
            av_frame_free(&entry.frame);
            return;
        }

        m_SourcePeriodUs = m_Policy.sourcePeriodUs();
        m_Limit.noteArrival();
        // Ready too late to be handed over before its target
        late = entry.paced && entry.targetUs < readyUs + m_RenderLeadUs;
        renderLeadUs = m_RenderLeadUs;

        if (m_Queue.size() >= MaxQueuedFrames) {
            evicted = m_Queue.front();
            m_Queue.pop_front();
            if (!m_WarnedQueueFull) {
                m_WarnedQueueFull = true;
                warnQueueFull = true;
            }
        }
        m_Queue.push_back(entry);
        queueDepth = uint32_t(m_Queue.size());
    }
    m_Wake.notify_one();

    if (m_Trace != nullptr) {
        const uint64_t nowUs = LiGetMicroseconds();
        TimestampTrace::Row row = frameRow(TimestampTrace::Event::Scheduled, entry, nowUs);
        row.hostLatencyUs = paced.hostLatencyUs();
        row.receiveUs = paced.receiveUs();
        row.reassembledUs = paced.reassembledUs();
        row.decodeSubmitUs = paced.decodeSubmitUs();
        row.decoderOutputUs = paced.decoderOutputUs();
        row.decodeCompleteUs = paced.decodeCompleteUs();
        row.decoderQueueUs = paced.decoderQueueUs();
        row.readyUs = readyUs;
        row.decodeDelayUs = decodeDelayUs;
        row.admitted = decision.admitted;
        row.repeat = decision.repeat;
        row.restarted = decision.restarted;
        row.reanchored = decision.reanchored;
        row.late = late;
        row.policyTargetUs = decision.targetUs;
        row.bufferUs = decision.bufferUs;
        row.latenessUs = decision.latenessUs;
        row.correctionUs = int64_t(std::llround(decision.correctionUs));
        row.hostJerkUs = int64_t(std::llround(decision.hostJerkUs));
        row.hostJerkValid = decision.hostJerkValid;
        row.sourcePeriodUs = uint64_t(m_Policy.sourcePeriodUs());
        row.renderLeadUs = renderLeadUs;
        row.queueDepth = queueDepth;
        m_Trace->record(row);
        if (evicted.frame != nullptr) {
            TimestampTrace::Row evictedRow = frameRow(TimestampTrace::Event::Evicted, evicted, nowUs);
            evictedRow.queueDepth = queueDepth;
            m_Trace->record(evictedRow);
        }
    }

    if (evicted.frame != nullptr) {
        m_Callbacks.drop(evicted.frame, true);
    }
    if (warnQueueFull) {
        // Logging may start its own worker; don't hold the pacing lock or
        // retain the evicted decoder surface while doing so.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: queue full; the buffer is limited to %zu waiting frames",
                    MaxQueuedFrames);
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

void TimestampPacer::measureRefreshMode()
{
    std::lock_guard<std::mutex> lock(m_Lock);
    m_MeasureRefresh = m_DisplayHz > 0;
    m_RefreshClass.configure(1000000.0 / std::max(1, m_DisplayHz));
}

void TimestampPacer::onVsync(uint64_t atUs)
{
    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::Vsync;
    row.eventUs = atUs;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Grid.observe(atUs);
        if (m_MeasureRefresh) {
            m_RefreshClass.observe(atUs);
        }
        row.gridPeriodUs = uint64_t(m_Grid.periodUs());
    }
    trace(row);
}

void TimestampPacer::onDisplayEvent(uint64_t displayUs, uint64_t refreshPeriodUs)
{
    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::DisplayEvent;
    row.eventUs = LiGetMicroseconds();
    row.displayUs = displayUs;
    row.refreshPeriodUs = refreshPeriodUs;
    std::lock_guard<std::mutex> lock(m_Lock);
    row.displayMode = uint8_t(m_DisplayMode);
    // Recorded on every return, still under the lock: recording never waits
    const TraceOnReturn record { this, row };

    // Display times off a fixed refresh grid would only mislead it
    if (!modeUsesGrid(m_DisplayMode)) {
        return;
    }
    // The renderer's refresh period beats the nominal display mode, which
    // under Gamescope need not match the panel. A refresh rate the
    // compositor reports itself beats both: Gamescope's WSI layer starts
    // each swapchain at 60 Hz and Gamescope resends the real cycle only when
    // it changes, so a recreated swapchain keeps 60 Hz (2026-10-08 capture).
    if (m_CompositorRefreshHz == 0 && refreshPeriodUs != 0 &&
            std::fabs(double(refreshPeriodUs) - m_RefreshPeriodUs) > m_RefreshPeriodUs * 0.01) {
        m_RefreshPeriodUs = double(refreshPeriodUs);
        m_Grid.configure(m_Limit.refreshes() * m_RefreshPeriodUs);
    }
    if (m_DisplayMode == DisplayMode::FrameLimited) {
        noteLimitedDisplayLocked(displayUs, row.eventUs);
    }
    m_Grid.observe(displayUs);

    // Match the display time to the release planned for the nearest V-blank
    // at or before it
    const double periodUs = m_Grid.periodUs();
    row.gridPeriodUs = uint64_t(periodUs);
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
    row.matched = true;
    row.vblankUs = match->vblankUs;
    row.releaseUs = match->releaseUs;

    // Gamescope reports only frames it showed. A release planned for an
    // earlier V-blank that was never reported missed its latch and was
    // replaced by the next frame: that V-blank repeated a frame, and this
    // one dropped one. The lag check below can't see it, since the frame
    // shown here was on time. On a Deck capture (2026-10-09, 90 FPS at
    // 90 Hz) a 30 s stretch lost about a fifth of its frames this way, with
    // presents returning 5 ms before their V-blank and no miss detected.
    // Releases from before a longer gap in reports (an overlay, say) are
    // just forgotten. Gamescope also leaves out a few reports of frames it
    // did show (0-2 a second there), so the margin only widens once three
    // go missing within a second; that stretch lost 6-18 a second.
    // FIFO (the frame limit) never replaces a frame: an unreported one there
    // is only a gap in the reports. On a Deck capture (2026-10-09) treating
    // those as replaced widened the margin to 6 ms, handing frames over up
    // to 17 ms before their V-blank, where they waited in Gamescope's queue.
    uint32_t replaced = 0;
    const bool canReplace = m_DisplayMode != DisplayMode::FrameLimited;
    for (Release& release : m_Releases) {
        if (release.vblankUs == 0 || &release == match || release.vblankUs >= match->vblankUs) {
            continue;
        }
        if (canReplace && double(match->vblankUs - release.vblankUs) <= periodUs * 3.5) {
            replaced++;
        }
        release.vblankUs = 0;
    }
    if (replaced != 0) {
        row.superseded = replaced;
        if (displayUs - m_ReplacedWindowUs > 1000000) {
            m_ReplacedWindowUs = displayUs;
            m_ReplacedInWindow = 0;
        }
        m_ReplacedInWindow += replaced;
        const bool widen = m_ReplacedInWindow >= 3;
        if (widen) {
            m_ExtraMarginUs = std::min<uint64_t>(6000, m_ExtraMarginUs + 500);
            m_MarginChangedUs = displayUs;
            m_ReplacedWindowUs = displayUs;
            m_ReplacedInWindow = 0;
        }
        // Each is a missed V-blank in the overlay, widening or not
        for (uint32_t i = 0; i < replaced; i++) {
            m_Telemetry->recordTimestampVblankResult(true, m_ExtraMarginUs);
        }
        row.missed = true;
    }
    row.missed = noteDisplayLagLocked(displayUs, match->vblankUs, periodUs);
    match->vblankUs = 0;
}

void TimestampPacer::onFrameDisplayed(uint64_t presentStartUs, uint64_t displayUs, uint64_t refreshPeriodUs)
{
    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::FrameDisplayed;
    row.eventUs = LiGetMicroseconds();
    row.presentStartUs = presentStartUs;
    row.displayUs = displayUs;
    row.refreshPeriodUs = refreshPeriodUs;
    std::lock_guard<std::mutex> lock(m_Lock);
    row.displayMode = uint8_t(m_DisplayMode);
    row.gridPeriodUs = uint64_t(m_Grid.periodUs());
    const TraceOnReturn record { this, row };

    if (!modeUsesGrid(m_DisplayMode)) {
        return;
    }

    // The frame is the one released last before its present began
    Release* match = nullptr;
    for (Release& release : m_Releases) {
        if (release.vblankUs == 0 || release.releaseUs > presentStartUs ||
                presentStartUs - release.releaseUs > 100000) {
            continue;
        }
        if (match == nullptr || release.releaseUs > match->releaseUs) {
            match = &release;
        }
    }
    if (match == nullptr) {
        return;
    }
    row.matched = true;
    row.vblankUs = match->vblankUs;
    row.releaseUs = match->releaseUs;
    row.missed = noteDisplayLagLocked(displayUs, match->vblankUs,
                                      refreshPeriodUs != 0 ? double(refreshPeriodUs) : m_Grid.periodUs());
    match->vblankUs = 0;
}

TimestampPacer::TraceOnReturn::~TraceOnReturn()
{
    row.extraMarginUs = pacer->m_ExtraMarginUs;
    pacer->trace(row);
}

bool TimestampPacer::noteDisplayLagLocked(uint64_t displayUs, uint64_t vblankUs, double periodUs)
{
    const bool missed = m_Misses.observe(double(displayUs) - double(vblankUs), periodUs);

    if (missed) {
        m_ExtraMarginUs = std::min<uint64_t>(6000, m_ExtraMarginUs + 500);
        m_MarginChangedUs = displayUs;
    }
    else if (m_ExtraMarginUs != 0 && displayUs - m_MarginChangedUs > 5000000) {
        m_ExtraMarginUs -= std::min<uint64_t>(250, m_ExtraMarginUs);
        m_MarginChangedUs = displayUs;
    }
    m_Telemetry->recordTimestampVblankResult(missed, m_ExtraMarginUs);
    return missed;
}

void TimestampPacer::refreshDisplayMode(std::unique_lock<std::mutex>& lock, uint64_t nowUs)
{
    if ((!m_DisplayModeProbe && !m_MeasureRefresh) ||
            (m_DisplayModeCheckedUs != 0 && nowUs - m_DisplayModeCheckedUs < 250000)) {
        return;
    }
    m_DisplayModeCheckedUs = nowUs;

    DisplayMode mode;
    if (m_DisplayModeProbe) {
        // The probe may talk to the compositor, so never hold the lock over it
        const DisplayModeProbe probe = m_DisplayModeProbe;
        lock.unlock();
        const CompositorState state = probe();
        lock.lock();
        mode = state.mode;
        applyCompositorRefreshLocked(state.refreshHz, nowUs);
    }
    else {
        switch (m_RefreshClass.result()) {
        case TimestampPacing::RefreshClassifier::Result::Fixed: mode = DisplayMode::FixedRefresh; break;
        case TimestampPacing::RefreshClassifier::Result::Adaptive: mode = DisplayMode::Adaptive; break;
        default: mode = DisplayMode::Unknown; break;
        }
    }

    if (mode == m_DisplayMode) {
        return;
    }
    const bool gridBefore = modeUsesGrid(m_DisplayMode);
    const bool gridAfter = modeUsesGrid(mode);
    if (m_DisplayModeProbe) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: compositor presentation is now %s%s",
                    displayModeName(mode),
                    gridAfter ? " (frames placed on the V-blank grid)" : " (frames presented at their targets)");
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: display measured as %s: %.0f%% of %d Hz refreshes in the last second%s",
                    mode == DisplayMode::Adaptive ? "VRR (refreshing as frames arrive)" : "fixed refresh",
                    m_RefreshClass.lastShare() * 100, m_DisplayHz,
                    gridAfter ? " (frames placed on the V-blank grid)" : " (frames presented at their targets)");
    }
    if (mode == DisplayMode::FrameLimited) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: Steam's frame limit is on, so Gamescope presents in FIFO. Frames are placed on the V-blank grid, at most one per refresh; a limit below the refresh rate is measured from display times and the grid follows it.");
    }
    m_DisplayMode = mode;
    // Display times from another mode are not V-blank times. Start the grid
    // and the lock over, unless both modes use the grid, which then still
    // holds.
    if (gridBefore != gridAfter || m_DisplayModeProbe) {
        resetGridLocked();
    }
    m_Telemetry->recordTimestampDisplayMode(uint8_t(mode), !m_DisplayModeProbe);

    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::DisplayMode;
    row.eventUs = nowUs;
    row.displayMode = uint8_t(mode);
    row.displayModeMeasured = !m_DisplayModeProbe;
    row.refreshSharePerMille = m_DisplayModeProbe ? 0 : uint32_t(std::lround(m_RefreshClass.lastShare() * 1000));
    row.gridPeriodUs = uint64_t(m_Grid.periodUs());
    row.limitRefreshes = m_Limit.refreshes();
    trace(row);
}

void TimestampPacer::noteLateLocked(const Entry& shown, uint64_t assignedUs)
{
    // Under Gamescope, a frame handed over less than its lead before its
    // V-blank misses the latch, and in Mailbox the next frame replaces it: a
    // V-blank repeats and a frame is lost. On a Deck capture (2026-10-09,
    // 90 FPS at 90 Hz) a stretch where frames were ready only 5.5 ms before
    // their V-blank (10.7 ms normally) lost a quarter of them, and a wider
    // margin couldn't help, since a frame can't be handed over before it's
    // ready. Moving each late frame to its next V-blank instead (the next
    // capture) made the frames after it collide with it, since readiness
    // jitters around the limit: the pacer then dropped up to 15 a second.
    // So once half of the last 64 frames are late (normal stretches reached
    // 18, the bad one 64), all frames move a refresh later, a uniform shift
    // that costs a refresh of latency and drops nothing, until at most 2
    // would be late without it. The lead includes the margin that replaced
    // frames widen, so frames ready in time that Gamescope still missed
    // also lead here. The frame limit's FIFO already shows a late frame a
    // refresh later.
    if (!m_DisplayModeProbe || m_DisplayMode == DisplayMode::FrameLimited || shown.readyUs == 0) {
        return;
    }
    const bool late = shown.readyUs + leadUsLocked(true) > assignedUs;
    if (m_LateFilled == m_LateFlags.size()) {
        m_LateCount -= m_LateFlags[m_LateIndex];
    }
    else {
        m_LateFilled++;
    }
    m_LateFlags[m_LateIndex] = late ? 1 : 0;
    m_LateCount += late ? 1 : 0;
    m_LateIndex = (m_LateIndex + 1) % m_LateFlags.size();
    if (m_LateFilled < m_LateFlags.size()) {
        return;
    }

    if (!m_LateShift && m_LateCount >= m_LateFlags.size() / 2) {
        m_LateShift = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: %zu of the last %zu frames were ready too late for their V-blank; planning frames a refresh later",
                    m_LateCount, m_LateFlags.size());
    }
    else if (m_LateShift && m_LateCount <= 2) {
        m_LateShift = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: frames are ready in time for their V-blank again; no longer planning them a refresh later");
    }
}

void TimestampPacer::resetGridLocked()
{
    m_LateShift = false;
    m_LateFlags = {};
    m_LateIndex = 0;
    m_LateCount = 0;
    m_LateFilled = 0;
    m_Limit.reset();
    m_Grid.configure(m_RefreshPeriodUs);
    m_PhaseLock.reset();
    m_Releases = {};
    m_LastVblankUs = 0;
    m_Misses.reset();
}

void TimestampPacer::noteLimitedDisplayLocked(uint64_t displayUs, uint64_t nowUs)
{
    const TimestampPacing::LimitMeter::Update update =
        m_Limit.observe(displayUs, nowUs, m_RefreshPeriodUs, m_SourcePeriodUs);
    if (update.measured) {
        const uint32_t refreshes = m_Limit.refreshes();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: frames are shown every %u refresh%s (%.1f FPS) under the frame limit; V-blank grid period %.3f ms",
                    refreshes, refreshes == 1 ? "" : "es", 1000000.0 / (refreshes * m_RefreshPeriodUs),
                    refreshes * m_RefreshPeriodUs / 1000.0);
    }
    if (update.changed) {
        applyLimitLocked(nowUs);
    }
}

void TimestampPacer::applyLimitLocked(uint64_t nowUs)
{
    m_Grid.configure(m_Limit.refreshes() * m_RefreshPeriodUs);
    m_PhaseLock.reset();
    m_Releases = {};
    m_LastVblankUs = 0;
    m_Misses.reset();

    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::DisplayMode;
    row.eventUs = nowUs;
    row.displayMode = uint8_t(m_DisplayMode);
    row.gridPeriodUs = uint64_t(m_Grid.periodUs());
    row.limitRefreshes = m_Limit.refreshes();
    trace(row);
}

void TimestampPacer::applyCompositorRefreshLocked(int refreshHz, uint64_t nowUs)
{
    if (refreshHz <= 0 || refreshHz == m_CompositorRefreshHz) {
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Timestamp pacing: compositor reports a %d Hz output; V-blank grid period %.3f ms (was %.3f ms)",
                refreshHz, 1000.0 / refreshHz, m_Grid.nominalPeriodUs() / 1000.0);
    m_CompositorRefreshHz = refreshHz;
    // V-blanks of the old period don't line up with the new ones, and a
    // frame limit is measured again against the new refresh
    m_RefreshPeriodUs = 1000000.0 / refreshHz;
    resetGridLocked();

    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::DisplayMode;
    row.eventUs = nowUs;
    row.displayMode = uint8_t(m_DisplayMode);
    row.gridPeriodUs = uint64_t(m_Grid.periodUs());
    row.limitRefreshes = m_Limit.refreshes();
    trace(row);
}

bool TimestampPacer::gridUsableLocked(uint64_t nowUs) const
{
    return m_UseVblankGrid &&
           modeUsesGrid(m_DisplayMode) &&
           m_Grid.valid(nowUs);
}

void TimestampPacer::notePresented(uint64_t renderStartUs, uint64_t presentUs, uint64_t decoderOutputUs,
                                   uint64_t decodeWaitUs, bool rtpValid, uint32_t rtpTimestamp)
{
    TimestampTrace::Row row;
    row.event = TimestampTrace::Event::Presented;
    row.eventUs = presentUs;
    row.rtpValid = rtpValid;
    row.rtpTimestamp = rtpTimestamp;
    row.renderStartUs = renderStartUs;
    row.presentUs = presentUs;
    row.decoderOutputUs = decoderOutputUs;
    row.decodeWaitUs = decodeWaitUs;

    // Learn when decodes finish, relative to decoder output. A render that
    // waited for its decode shows when it finished; one that didn't shows
    // the decode was done by the time it began. Rise quickly to a slow
    // decode and fall back slowly, as the render lead does. A renderer that
    // never reports a wait leaves this at zero.
    //
    // A wait only times the decode when the render began near when decodes
    // finish. Driver contention also stretches the sync to a fraction of a
    // millisecond long after the decode is done: on a Deck capture
    // (2026-10-09, frame limit) such waits of 0.25-1.3 ms, on frames rendered
    // 10-20 ms after output, raised the delay to its cap. Holding frames for
    // that delay then kept every render that late, so nothing lowered it, and
    // frames sat 2-3 deep in the queue. Real decode waits there were about
    // 3 ms. The cap also keeps the delay under one and a half frames.
    if (decoderOutputUs != 0 && renderStartUs >= decoderOutputUs && renderStartUs - decoderOutputUs < 100000) {
        const uint64_t startedAfterUs = renderStartUs - decoderOutputUs;
        uint64_t delayUs = m_DecodeDelayUs.load(std::memory_order_relaxed);
        if (decodeWaitUs >= k_DecodeWaitThresholdUs) {
            if (startedAfterUs <= delayUs + k_DecodeWindowSlackUs) {
                const uint64_t doneUs = startedAfterUs + decodeWaitUs;
                delayUs = doneUs > delayUs ? delayUs + (doneUs - delayUs) / 4 : delayUs - (delayUs - doneUs) / 32;
            }
        }
        else if (startedAfterUs + decodeWaitUs < delayUs) {
            delayUs -= (delayUs - startedAfterUs - decodeWaitUs) / 32;
        }

        // Frames are held until the delay, so renders rarely begin before it
        // and an estimate that is too high may never be lowered above. Short
        // waits can't say by how much on their own, but their median can:
        // it stays near zero while the delay is too high and rises to the
        // shortfall once it is too low, so the decay stops about the
        // threshold below a typical decode. Renders held for other reasons
        // (FIFO back-pressure) begin later and don't count.
        if (startedAfterUs <= delayUs + k_DecodeDecayWindowUs && m_DecodeWaitCount < m_DecodeWaits.size()) {
            m_DecodeWaits[m_DecodeWaitCount++] = uint32_t(std::min<uint64_t>(decodeWaitUs, UINT32_MAX));
        }
        if (m_DecodeWaitWindowUs == 0 || presentUs < m_DecodeWaitWindowUs) {
            m_DecodeWaitWindowUs = presentUs;
        }
        else if (presentUs - m_DecodeWaitWindowUs >= 1000000) {
            const size_t minSamples = std::max<size_t>(8, size_t(std::max(m_StreamFps, 0)) / 4);
            if (m_DecodeWaitCount >= minSamples) {
                const auto begin = m_DecodeWaits.begin();
                std::nth_element(begin, begin + m_DecodeWaitCount / 2, begin + m_DecodeWaitCount);
                if (m_DecodeWaits[m_DecodeWaitCount / 2] < k_DecodeDecayMedianUs) {
                    delayUs -= delayUs / 50;
                }
            }
            m_DecodeWaitCount = 0;
            m_DecodeWaitWindowUs = presentUs;
        }
        const uint64_t maxDelayUs = m_StreamFps > 0 ? std::min<uint64_t>(20000, 1500000 / uint64_t(m_StreamFps)) : 20000;
        m_DecodeDelayUs.store(std::min(delayUs, maxDelayUs), std::memory_order_relaxed);
    }
    row.decodeDelayUs = m_DecodeDelayUs.load(std::memory_order_relaxed);

    uint64_t sampleUs, leadUs, plannedPresentUs;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        row.releaseUs = m_LastReleaseUs;
        row.plannedPresentUs = m_PlannedPresentUs;
        row.renderLeadUs = m_RenderLeadUs;
        if (m_LastReleaseUs == 0 || presentUs < m_LastReleaseUs || presentUs - m_LastReleaseUs > 50000) {
            // No release to time it from
            trace(row);
            return;
        }

        // Rise quickly to a slow frame and fall back slowly, so the lead
        // covers most frames rather than the average one. Waiting for the
        // decode is not rendering: readiness accounts for it.
        //
        // Under Gamescope, the render thread can also still be waiting for a
        // swapchain image when a frame is released, because FIFO (the frame
        // limit) holds the frames handed over earlier. That wait is not
        // rendering either, and counting it feeds itself: a longer lead hands
        // frames over earlier, they wait in FIFO longer, and the next image
        // comes later. On a Deck capture (2026-10-09, frame limit at 90 Hz)
        // occasional 11 ms samples held the lead at 5.6 ms against 1.5 ms of
        // rendering, and frames presented 9 ms before their V-blank. So the
        // lead there is timed from when the render began. Elsewhere that wait
        // (DXGI's frame latency) is part of handing a frame over.
        const uint64_t startUs = m_DisplayModeProbe ? std::max(m_LastReleaseUs, renderStartUs) : m_LastReleaseUs;
        sampleUs = presentUs > startUs ? presentUs - startUs : 0;
        sampleUs -= std::min(sampleUs, decodeWaitUs);
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
    trace(row);

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
    const uint64_t showUs = vblankGrid ? vblankForLocked(entry) : entry.targetUs;
    return showUs > leadUs ? showUs - leadUs : 0;
}

// The V-blank a frame is planned for. Under a frame limit Gamescope presents
// in FIFO, which shows every queued frame at a refresh of its own. A frame
// whose V-blank an earlier release already took waits for the next one, and
// a newer frame due by then replaces it, so FIFO never holds a backlog. On a
// Deck capture (2026-10-08, 90 FPS at 60 Hz), releasing them all queued
// frames and blocked presents for up to 14 ms.
uint64_t TimestampPacer::vblankForLocked(const Entry& entry) const
{
    uint64_t vblankUs = m_PhaseLock.assign(entry.targetUs, m_Grid);

    // While most frames can't be ready their lead before their V-blank,
    // every frame is planned a refresh later (see noteLateLocked())
    if (m_LateShift) {
        vblankUs = m_Grid.atOrAfter(double(vblankUs) + m_Grid.periodUs() / 2);
    }

    if (m_DisplayMode != DisplayMode::FrameLimited || m_LastVblankUs == 0 ||
            double(vblankUs) > double(m_LastVblankUs) + m_Grid.periodUs() / 2) {
        return vblankUs;
    }
    return m_Grid.atOrAfter(double(m_LastVblankUs) + m_Grid.periodUs() / 2);
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
            m_LastSchedulerDelayUs = wait.schedulerDelayUs;
            m_LastSchedulerDelayValid = wait.schedulerDelayValid;
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
    const uint64_t frontVblankUs = vblankGrid && front.paced ? vblankForLocked(front) : 0;
    size_t dueCount = 1;
    for (size_t i = 1; i < m_Queue.size(); i++) {
        const Entry& entry = m_Queue[i];
        bool due;
        if (!entry.paced) {
            due = true;
        }
        else if (vblankGrid && frontVblankUs != 0) {
            due = vblankForLocked(entry) <= frontVblankUs;
        }
        else {
            due = releaseTimeLocked(entry, vblankGrid) <= nowUs + k_ReleaseSlackUs;
        }
        if (!due) {
            break;
        }
        dueCount = i + 1;
    }

    std::array<Entry, MaxQueuedFrames> superseded {};
    size_t supersededCount = 0;
    for (size_t i = 0; i + 1 < dueCount; i++) {
        superseded[supersededCount++] = m_Queue.front();
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
        // The phase lock learns from the V-blank the target falls on, not
        // one a frame limit postponed it to
        const uint64_t assignedUs = m_PhaseLock.assign(shown.targetUs, m_Grid);
        const uint64_t vblankUs = vblankForLocked(shown);
        vblankWaitValid = true;
        vblankWaitUs = int64_t(vblankUs) - int64_t(shown.targetUs);
        m_PhaseLock.update(shown.targetUs, assignedUs, m_Grid.periodUs(), m_SourcePeriodUs);
        noteLateLocked(shown, assignedUs);
        m_Releases[m_NextRelease] = { nowUs, vblankUs };
        m_NextRelease = (m_NextRelease + 1) % m_Releases.size();
        m_LastVblankUs = vblankUs;
    }
    m_LastReleaseUs = nowUs;
    const int64_t trimUs = int64_t(m_PhaseLock.trimUs());

    if (m_Trace != nullptr) {
        TimestampTrace::Row row = frameRow(TimestampTrace::Event::Released, shown, nowUs);
        row.plannedReleaseUs = plannedReleaseUs;
        row.vblankGrid = vblankGrid;
        row.vblankUs = vblankWaitValid ? uint64_t(int64_t(shown.targetUs) + vblankWaitUs) : 0;
        row.trimUs = trimUs;
        row.renderLeadUs = m_RenderLeadUs;
        row.releaseLeadUs = leadUsLocked(vblankGrid);
        row.extraMarginUs = m_ExtraMarginUs;
        row.wakeLeadUs = m_WakeLeadUs;
        row.schedulerDelayUs = m_LastSchedulerDelayUs;
        row.schedulerDelayValid = m_LastSchedulerDelayValid;
        row.superseded = uint32_t(supersededCount);
        row.queueDepth = uint32_t(m_Queue.size());
        row.plannedPresentUs = m_PlannedPresentUs;
        row.sourcePeriodUs = uint64_t(m_SourcePeriodUs);
        row.gridPeriodUs = vblankGrid ? uint64_t(m_Grid.periodUs()) : 0;
        row.displayMode = uint8_t(m_DisplayMode);
        row.limitRefreshes = m_Limit.refreshes();
        row.lateShift = m_LateShift;
        for (size_t i = 0; i < supersededCount; i++) {
            TimestampTrace::Row supersededRow = frameRow(TimestampTrace::Event::Superseded, superseded[i], nowUs);
            supersededRow.queueDepth = row.queueDepth;
            m_Trace->record(supersededRow);
        }
        m_Trace->record(row);
    }
    m_LastSchedulerDelayUs = 0;
    m_LastSchedulerDelayValid = false;

    lock.unlock();
    for (size_t i = 0; i < supersededCount; i++) {
        m_Callbacks.drop(superseded[i].frame, false);
    }
    m_Callbacks.release(shown.frame);
    m_Telemetry->recordTimestampRelease(trimUs, vblankGrid, vblankWaitValid, vblankWaitUs);
    lock.lock();
}
