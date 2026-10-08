#pragma once

#include "timestamppacingpolicy.h"
#include "vrr/vrrtargetwaiter.h"
#include "vrr/vrrtypes.h"

#include <QString>

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
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
        // A compositor frame limiter forces FIFO presentation
        FrameLimited,
    };
    using DisplayModeProbe = std::function<DisplayMode()>;
    static const char* displayModeName(DisplayMode mode);

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
    // runs after this returns.
    void stop();

    void submit(PacedFrame&& frame);

    // A V-sync source woke at this time
    void onVsync(uint64_t atUs);

    // The renderer reports a frame was displayed at this time, with the
    // display's refresh period when known (zero otherwise)
    void onDisplayEvent(uint64_t displayUs, uint64_t refreshPeriodUs);

    // The renderer finished presenting the most recently released frame
    void notePresented(uint64_t presentUs);

    size_t queueDepth();

    // Whether frames can be placed on a V-blank grid at all
    bool usesVblankGrid() const { return m_UseVblankGrid; }

    QString describe() const;

private:
    struct Entry {
        AVFrame* frame = nullptr;
        uint64_t targetUs = 0;
        bool paced = false;
    };

    void run();
    void refreshDisplayMode(std::unique_lock<std::mutex>& lock, uint64_t nowUs);
    bool gridUsableLocked(uint64_t nowUs) const;
    uint64_t leadUsLocked(bool vblankGrid) const;
    uint64_t releaseTimeLocked(const Entry& entry, bool vblankGrid) const;
    void releaseDueLocked(std::unique_lock<std::mutex>& lock, bool vblankGrid);

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
    // Release-to-present time of recent frames, biased toward the slow ones
    uint64_t m_RenderLeadUs = 1000;
    uint64_t m_LastReleaseUs = 0;
    // When the most recently released frame's present should return
    uint64_t m_PlannedPresentUs = 0;
    bool m_Stopping = false;
    bool m_WarnedQueueFull = false;

    // With display events, which V-blank each released frame was meant for,
    // so a frame shown a refresh late can be caught. Each miss widens the
    // submit margin; a clean stretch narrows it again.
    struct Release {
        uint64_t releaseUs = 0;
        uint64_t vblankUs = 0;
    };
    std::array<Release, 16> m_Releases {};
    size_t m_NextRelease = 0;
    uint64_t m_ExtraMarginUs = 0;
    uint64_t m_MarginChangedUs = 0;

    DisplayModeProbe m_DisplayModeProbe;
    DisplayMode m_DisplayMode = DisplayMode::Unknown;
    uint64_t m_DisplayModeCheckedUs = 0;

    VrrTargetWaiter m_Waiter;
    std::thread m_Thread;
};
