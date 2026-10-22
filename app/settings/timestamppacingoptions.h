#pragma once

#include <algorithm>
#include <cstdint>

// Options for the timestamp pacer, which presents fixed-refresh and
// non-V-Sync streams on a smoothed version of the host's RTP timeline. It and
// VRR Pacing Mode are mutually exclusive; see docs/timestamp-pacing-proposal.md.
struct TimestampPacingOptions {
    // Smoothing strength of the host timeline. Light is the default: on
    // every capture evaluated it matched or beat Standard, and it follows a
    // game whose frame rate wanders. A stronger level was removed after it
    // skipped far more frames on a Steam Deck capture whose rate kept
    // changing; see docs/timestamp-pacing-findings.md.
    enum Smoothing {
        // Follow the host's timestamps exactly
        SmoothingOff = 0,
        SmoothingLight = 1,
        SmoothingStandard = 2,
    };

    bool enabled = false;
    int smoothing = SmoothingLight;
    // Share of frames the buffer aims to have ready by their target, per mille
    int targetPerMille = 990;
    int minBufferMs = 2;
    int maxBufferMs = 16;
    // With V-Sync, how long before the chosen V-blank a frame is handed to
    // the renderer, on top of the learned rendering time
    int vsyncMarginUs = 2000;
    // Under Gamescope, which composites before each refresh, the margin
    // starts here instead unless a test key sets it
    static constexpr int GamescopeVsyncMarginUs = 4500;

    // Under a frame limit (FIFO), a frame whose refresh is already taken is
    // shown at the next one instead of being replaced by the frame behind
    // it, which then waits a refresh too. This is how long such a refresh of
    // delay may be held before a frame is dropped to give it back; it also
    // drains by itself wherever the plan leaves a refresh empty. Off follows
    // the timestamps exactly and replaces as before.
    //
    // Replayed (tsreplay) on two Deck captures (2026-10-22, 90 FPS at 90 Hz,
    // frame limit), replacing cost 3.5 and 33 jerks over 2 ms per 1000
    // frames at Light, 3.3 and 30 at Standard, against 1.5 and 17 for
    // Moonlight's own pacer. Light's 1 s hold gave 2.4 and 23 for 0.7 and
    // 4.3 ms more mean delay; Standard's 2 s hold gave 2.1 and 18 for 1.2 and
    // 3.9 ms. Without a limit the delay lingered for up to 81 s. Light keeps
    // the shorter hold for lower latency.
    uint64_t fifoHoldLimitUs() const
    {
        switch (smoothing) {
        case SmoothingLight: return 1000000;
        case SmoothingStandard: return 2000000;
        default: return 0;
        }
    }

    TimestampPacingOptions resolved() const
    {
        TimestampPacingOptions out = *this;
        out.smoothing = std::max<int>(SmoothingOff, std::min<int>(SmoothingStandard, smoothing));
        out.targetPerMille = std::max(900, std::min(999, targetPerMille));
        out.minBufferMs = std::max(0, std::min(20, minBufferMs));
        out.maxBufferMs = std::max(std::max(1, out.minBufferMs), std::min(50, maxBufferMs));
        out.vsyncMarginUs = ((std::max(250, std::min(8000, vsyncMarginUs)) + 125) / 250) * 250;
        return out;
    }
};
