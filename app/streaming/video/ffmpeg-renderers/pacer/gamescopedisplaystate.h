#pragma once

#include <QtGlobal>

#include <cstdint>

// How Gamescope is presenting the focused app right now, from the root window
// properties Steam sets through its quick access menu and Gamescope reports
// back. Each can change mid-stream:
//   GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK  the output's refresh rate (Hz)
//   GAMESCOPE_VRR_FEEDBACK    Gamescope reports adaptive sync in use
//   GAMESCOPE_LIMITER_FEEDBACK  Gamescope's frame limit is engaged, which
//                             forces FIFO presentation
//   GAMESCOPE_ALLOW_TEARING   Steam's Allow Tearing (Gamescope V-Sync off)
// GAMESCOPE_FPS_LIMIT is not read: Gamescope resets its active limit
// internally without changing the property, which then reads a stale value
// (60 on an unlimited 120 Hz stream, 90 on a docked Deck; 2026-10-08 captures).
// The limiter feedback is rewritten whenever the limit engages or releases;
// on a Deck capture it matched presents blocking at the refresh rate. It
// does not say what the limit is.
// They live on the root of Gamescope's first Xwayland, which need not be
// DISPLAY. Linux with X11 only; elsewhere, and outside Gamescope, it never
// opens.
class GamescopeDisplayState
{
public:
    struct State {
        bool valid = false;
        // Zero when Gamescope does not report it
        uint32_t refreshHz = 0;
        bool vrrInUse = false;
        bool frameLimited = false;
        bool tearingAllowed = false;
    };

    GamescopeDisplayState();
    ~GamescopeDisplayState();
    GamescopeDisplayState(const GamescopeDisplayState&) = delete;
    GamescopeDisplayState& operator=(const GamescopeDisplayState&) = delete;

    // Whether this process runs under Gamescope at all, on any build. Header
    // only, so settings code can ask without linking the X11 probe.
    static bool runningUnderGamescope()
    {
        return !qEnvironmentVariableIsEmpty("GAMESCOPE_WAYLAND_DISPLAY");
    }

    // Opens a private connection to Gamescope's first X server. Reads happen on
    // one thread only after this returns.
    bool open();

    State read();

    // Gamescope's output refresh rate right now, or zero when it can't be
    // read. Opens and closes its own connection.
    static uint32_t readRefreshHz();

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};
