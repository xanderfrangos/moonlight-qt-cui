#pragma once

#include <QString>

struct SDL_Window;

// Diagnostics for what the display really is under Gamescope, where SDL and
// Qt see only the Xwayland output Gamescope created at startup. Each source
// is read independently so the report shows where they disagree:
//   - SDL and Qt display modes (what Moonlight uses today)
//   - every Xwayland server's root properties and RandR state, including
//     Gamescope's first Xwayland, where Steam and Gamescope keep the live
//     refresh rate and VRR state
//   - Gamescope's own Wayland socket: wl_output and gamescope_control's
//     active_display_info (connector, VRR capability, valid refresh rates)
//   - KMS through the DRM card nodes: connector modes and properties, the
//     active CRTC mode and its VRR_ENABLED property, the EDID
//   - sysfs connector state, Gamescope's patched EDID, its command line
//   - (active probe) Vulkan: displays, surface present modes, and the
//     intervals Gamescope actually presents frames at in each present mode
// The startup and stream reports, and the stream's samples, are written only
// while VRR tracing is on (MOONLIGHT_VRR_TRACE, which Settings' diagnostic
// capture sets), beside the trace, and summarized in the log. The explicit
// probe-display action writes under ~/moonlight-display-probe (or the app
// data directory when home is not writable). Linux only; elsewhere these do
// nothing.
namespace DisplayProbe
{
    // Writes a passive report beside the VRR trace, which needs no window of
    // its own. Returns the report path, or an empty string if nothing was
    // written, including when tracing is off.
    QString writePassiveReport(const char* reason, SDL_Window* window = nullptr);

    // Runs the passive report, then presents test frames in a fullscreen
    // Vulkan window and records their display times. For the "probe-display"
    // command line action. Returns a process exit code.
    int runActiveProbe();

    // While VRR tracing is on, samples Gamescope's live state as a stream
    // runs, appending a line to the stream's report whenever it changes.
    class StreamSampler
    {
    public:
        StreamSampler();
        ~StreamSampler();
        StreamSampler(const StreamSampler&) = delete;
        StreamSampler& operator=(const StreamSampler&) = delete;

        void start(SDL_Window* window);
        void stop();

    private:
        struct Impl;
        Impl* m_Impl = nullptr;
    };
}
