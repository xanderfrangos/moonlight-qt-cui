#pragma once

#include "timestamppacingpolicy.h"
#include "timestamptrace.h"
#include "vrr/vrrtargetwaiter.h"
#include "vrr/vrrtypes.h"

#include <QString>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class PacerTelemetry;

// Holds decoded frames until the time their smoothed host timestamp calls
// for, then hands them to the renderer through the fixed pacer's render
// queue, so every renderer works unchanged with V-Sync on or off.
//
// With V-Sync and a V-sync source, frames are placed on the measured V-blank
// grid: each is handed over a little before the first V-blank at or after its
// target, and when several are due for the same V-blank only the newest is
// shown. A renderer whose present flips at once rather than at the next
// V-blank (exclusive fullscreen) is handed the frame at the V-blank instead,
// as fixed pacing does. Without a grid, each frame is handed over its
// rendering time ahead of its target.
//
// The grid can come from a V-sync source or from the renderer's reports of
// when frames were actually displayed. A compositor that can change how it
// presents mid-stream (Gamescope) supplies a probe; the grid is only used
// while it presents at a fixed refresh rate.
class TimestampPacer
{
public:
    // How the compositor presents frames, when it can say
    enum class DisplayMode : uint8_t {
        // No probe, or it can't tell: assume a fixed refresh rate
        Unknown,
        // The newest frame is shown at each refresh
        FixedRefresh,
        // Adaptive sync: a frame is shown when it arrives
        Adaptive,
        // Tearing allowed: a frame flips the moment it arrives
        Tearing,
        // A compositor frame limiter forces FIFO presentation: each queued
        // frame is shown at its own refresh. Uses the V-blank grid, which
        // releases at most one frame per refresh, so surplus frames are
        // dropped here rather than queued behind a blocking present.
        FrameLimited,
    };
    // What the probe reads from the compositor
    struct CompositorState {
        DisplayMode mode = DisplayMode::Unknown;
        // The compositor's output refresh rate, zero when it doesn't say.
        // It sets the V-blank grid's period in place of the renderer's
        // reported refresh cycle.
        int refreshHz = 0;
    };
    using DisplayModeProbe = std::function<CompositorState()>;
    static const char* displayModeName(DisplayMode mode);
    static bool modeUsesGrid(DisplayMode mode);

    // Frames waiting for their target. Kept to the fixed pacing queue's bound
    // so the decoder's surface pool still covers every frame held here.
    static constexpr size_t MaxQueuedFrames = 3;

    struct Callbacks {
        // Hands a frame to the renderer
        std::function<void(AVFrame*)> release;
        // Discards a frame replaced by a newer one before it was shown.
        // queueFull is set when it was evicted from a full queue instead.
        std::function<void(AVFrame*, bool queueFull)> drop;
    };

    TimestampPacer(const TimestampPacingOptions& options, int streamFps, int displayHz,
                   bool useVblankGrid, bool presentFlipsImmediately,
                   PacerTelemetry* telemetry, Callbacks callbacks);
    ~TimestampPacer();

    // Optional. Polled on the pacing thread, at most every 250 ms.
    void setDisplayModeProbe(DisplayModeProbe probe);

    bool start();

    // Joins the pacing thread and frees the frames still waiting. No callback
    // runs after this returns. The trace stays open until destruction, for
    // the V-sync and render threads that stop after this.
    void stop();

    void submit(PacedFrame&& frame);

    // Judge from V-sync wakeups whether the display refreshes at a fixed rate
    // or only as frames arrive (VRR), and follow it as the compositor probe
    // would: the grid is only used at a fixed rate. Only for a source that
    // wakes at every hardware V-blank whether or not anything is presented
    // (Windows). Call before start(); the probe takes precedence.
    void measureRefreshMode();

    // A V-sync source woke at this time
    void onVsync(uint64_t atUs);

    // The renderer reports a frame was displayed at this time, with the
    // display's refresh period when known (zero otherwise). Builds the
    // V-blank grid and checks for missed V-blanks.
    void onDisplayEvent(uint64_t displayUs, uint64_t refreshPeriodUs);

    // The renderer reports when the frame whose present began at
    // presentStartUs was displayed. Only checks for missed V-blanks, for
    // renderers whose grid comes from a V-sync source (Windows).
    void onFrameDisplayed(uint64_t presentStartUs, uint64_t displayUs, uint64_t refreshPeriodUs);

    // The renderer finished presenting the most recently released frame.
    // renderStartUs is when its render began, decoderOutputUs when the
    // decoder returned it, and decodeWaitUs how long the render waited for
    // its GPU decode to finish (zero when the renderer doesn't say). The RTP
    // timestamp identifies it in the trace.
    void notePresented(uint64_t renderStartUs, uint64_t presentUs, uint64_t decoderOutputUs,
                       uint64_t decodeWaitUs, bool rtpValid, uint32_t rtpTimestamp);

    size_t queueDepth();

    // How long after decoder output frames are treated as decoded
    uint64_t decodeDelayUs() const { return m_DecodeDelayUs.load(std::memory_order_relaxed); }

    // How much earlier than usual frames are handed over, after missed V-blanks
    uint64_t extraMarginUs();

    // Whether frames can be placed on a V-blank grid at all
    bool usesVblankGrid() const { return m_UseVblankGrid; }

    QString describe() const;

private:
    struct Entry {
        AVFrame* frame = nullptr;
        uint64_t targetUs = 0;
        // When its decode is expected to be finished
        uint64_t readyUs = 0;
        bool paced = false;
        // For the trace
        int32_t frameNumber = -1;
        bool rtpValid = false;
        uint32_t rtpTimestamp = 0;
    };

    void run();
    void refreshDisplayMode(std::unique_lock<std::mutex>& lock, uint64_t nowUs);
    // Takes the grid's period from the compositor's reported refresh rate
    void applyCompositorRefreshLocked(int refreshHz, uint64_t nowUs);
    bool gridUsableLocked(uint64_t nowUs) const;
    uint64_t leadUsLocked(bool vblankGrid) const;
    uint64_t releaseTimeLocked(const Entry& entry, bool vblankGrid) const;
    uint64_t vblankForLocked(const Entry& entry) const;
    void releaseDueLocked(std::unique_lock<std::mutex>& lock, bool vblankGrid);
    void learnWakeLead(uint64_t schedulerDelayUs);
    // A miss widens the margin only when releasing earlier could have helped
    bool noteDisplayLagLocked(uint64_t displayUs, uint64_t vblankUs, double periodUs, bool decodeBound = false);
    // Under a frame limit, spaces the grid at the interval frames are shown at
    void noteLimitedDisplayLocked(uint64_t displayUs, uint64_t nowUs);
    void applyLimitLocked(uint64_t nowUs);
    // Puts the grid back at one V-blank per refresh
    void resetGridLocked();
    // Shifts every frame a refresh later while most are ready too late
    void noteLateLocked(const Entry& shown, uint64_t assignedUs);
    void trace(const TimestampTrace::Row& row);
    // Records a display report's row on every return, under the lock
    struct TraceOnReturn {
        TimestampPacer* pacer;
        TimestampTrace::Row& row;
        ~TraceOnReturn();
    };
    static TimestampTrace::Row frameRow(TimestampTrace::Event event, const Entry& entry, uint64_t atUs);

    const TimestampPacingOptions m_Options;
    const int m_StreamFps;
    const int m_DisplayHz;
    const bool m_UseVblankGrid;
    const bool m_PresentFlipsImmediately;
    PacerTelemetry* const m_Telemetry;
    const Callbacks m_Callbacks;

    // Used only on the submitting (decoder) thread
    TimestampPacing::Policy m_Policy;

    std::mutex m_Lock;
    std::condition_variable m_Wake;
    std::deque<Entry> m_Queue;
    TimestampPacing::VblankGrid m_Grid;
    TimestampPacing::PhaseLock m_PhaseLock;
    double m_SourcePeriodUs = 0;
    // Release-to-present time of recent frames, biased toward the slow ones,
    // without the time spent waiting for their decode
    uint64_t m_RenderLeadUs = 1000;
    // How long after the decoder returns a frame its GPU decode is finished,
    // biased toward the slow ones. VAAPI returns frames before decoding
    // them; on a Deck (2026-10-08) the render then waited about 6 ms.
    // Frames are treated as ready this long after decoder output. Read by
    // the submitting thread.
    std::atomic<uint64_t> m_DecodeDelayUs { 0 };
    // Render thread only. Decode waits of renders the delay held, over the
    // current second, from which the delay decays.
    std::array<uint32_t, 512> m_DecodeWaits {};
    size_t m_DecodeWaitCount = 0;
    uint64_t m_DecodeWaitWindowUs = 0;
    uint64_t m_LastReleaseUs = 0;
    // The V-blank the last grid release was planned for
    uint64_t m_LastVblankUs = 0;
    // When the most recently released frame's present should return
    uint64_t m_PlannedPresentUs = 0;
    bool m_Stopping = false;
    bool m_WarnedQueueFull = false;

    // With display reports (Gamescope's display times, or D3D11's frame
    // statistics), which V-blank each released frame was meant for, so a
    // frame shown a refresh late can be caught. Each miss widens the submit
    // margin; a clean stretch narrows it again.
    struct Release {
        uint64_t releaseUs = 0;
        uint64_t vblankUs = 0;
        // Its render waited for its decode, so it presented when the decode
        // allowed, however early it was handed over
        bool decodeBound = false;
    };
    std::array<Release, 16> m_Releases {};
    size_t m_NextRelease = 0;
    uint64_t m_ExtraMarginUs = 0;
    uint64_t m_MarginChangedUs = 0;
    // Frames Gamescope replaced (never reported shown) within the last second
    uint64_t m_ReplacedWindowUs = 0;
    uint32_t m_ReplacedInWindow = 0;
    TimestampPacing::MissDetector m_Misses;
    TimestampPacing::RefreshClassifier m_RefreshClass;
    bool m_MeasureRefresh = false;

    DisplayModeProbe m_DisplayModeProbe;
    DisplayMode m_DisplayMode = DisplayMode::Unknown;
    int m_CompositorRefreshHz = 0;
    uint64_t m_DisplayModeCheckedUs = 0;
    // The display's refresh period, and under a frame limit how many
    // refreshes apart frames are shown, measured from display reports
    double m_RefreshPeriodUs = 16667;
    TimestampPacing::LimitMeter m_Limit;
    // Whether each of the last grid releases was ready too late for its own
    // V-blank, and whether frames are planned a refresh later for it
    std::array<uint8_t, 64> m_LateFlags {};
    size_t m_LateIndex = 0;
    size_t m_LateCount = 0;
    size_t m_LateFilled = 0;
    bool m_LateShift = false;

    VrrTargetWaiter m_Waiter;
    // Pacing thread only. How far the precise waiter's sleeps have recently
    // overrun, so it wakes that much earlier and spins the rest.
    static constexpr size_t SchedulerSamples = 19;
    std::array<uint64_t, SchedulerSamples> m_SchedulerDelays {};
    size_t m_SchedulerDelayCount = 0;
    size_t m_NextSchedulerDelay = 0;
    uint64_t m_WakeLeadUs = 0;
    // The precise waiter's last oversleep before a release, for the trace
    uint64_t m_LastSchedulerDelayUs = 0;
    bool m_LastSchedulerDelayValid = false;
    std::thread m_Thread;

    // Null unless tracing. Set before any thread that records starts, and
    // destroyed after they have all stopped.
    std::unique_ptr<TimestampTrace> m_Trace;
};
