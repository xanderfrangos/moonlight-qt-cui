#pragma once

#include <cstdint>

// How Gamescope is presenting the focused app right now, from the root window
// properties Steam sets through its quick access menu and Gamescope reports
// back. Each can change mid-stream:
//   GAMESCOPE_FPS_LIMIT       Steam's frame limit; nonzero forces FIFO
//   GAMESCOPE_VRR_FEEDBACK    Gamescope reports adaptive sync in use
//   GAMESCOPE_ALLOW_TEARING   Steam's Allow Tearing (Gamescope V-Sync off)
// Linux with X11 only; elsewhere, and outside Gamescope, it never opens.
class GamescopeDisplayState
{
public:
    struct State {
        bool valid = false;
        uint32_t fpsLimit = 0;
        bool vrrInUse = false;
        bool tearingAllowed = false;
    };

    GamescopeDisplayState();
    ~GamescopeDisplayState();
    GamescopeDisplayState(const GamescopeDisplayState&) = delete;
    GamescopeDisplayState& operator=(const GamescopeDisplayState&) = delete;

    // Whether this process runs under Gamescope at all
    static bool runningUnderGamescope();

    // Opens a private connection to Gamescope's X server. Reads happen on
    // one thread only after this returns.
    bool open();

    State read();

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};
