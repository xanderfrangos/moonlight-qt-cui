#pragma once

#include <algorithm>

// Options for the timestamp pacer, which presents fixed-refresh and
// non-V-Sync streams on a smoothed version of the host's RTP timeline. It and
// VRR Pacing Mode are mutually exclusive; see docs/timestamp-pacing-proposal.md.
struct TimestampPacingOptions {
    // Smoothing strength of the host timeline
    enum Smoothing {
        // Follow the host's timestamps exactly
        SmoothingOff = 0,
        SmoothingLight = 1,
        SmoothingStandard = 2,
        SmoothingStrong = 3,
    };

    bool enabled = false;
    int smoothing = SmoothingStandard;
    // Share of frames the buffer aims to have ready by their target, per mille
    int targetPerMille = 990;
    int minBufferMs = 2;
    int maxBufferMs = 16;
    // With V-Sync, how long before the chosen V-blank a frame is handed to
    // the renderer, on top of the learned rendering time
    int vsyncMarginUs = 2000;

    TimestampPacingOptions resolved() const
    {
        TimestampPacingOptions out = *this;
        out.smoothing = std::max<int>(SmoothingOff, std::min<int>(SmoothingStrong, smoothing));
        out.targetPerMille = std::max(900, std::min(999, targetPerMille));
        out.minBufferMs = std::max(0, std::min(20, minBufferMs));
        out.maxBufferMs = std::max(std::max(1, out.minBufferMs), std::min(50, maxBufferMs));
        out.vsyncMarginUs = ((std::max(250, std::min(8000, vsyncMarginUs)) + 125) / 250) * 250;
        return out;
    }
};
