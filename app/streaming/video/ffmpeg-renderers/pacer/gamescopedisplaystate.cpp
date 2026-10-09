#include "gamescopedisplaystate.h"

#include <SDL.h>

#if defined(__linux__) && defined(HAS_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdio>

namespace {

// Wlroots gives each Xwayland the first free display number up to this
constexpr int MaxXwaylandDisplay = 32;

// Reads a CARDINAL from the root window, if it is set
bool rootCardinal(Display* display, Atom atom, uint32_t& value)
{
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    bool found = false;
    if (XGetWindowProperty(display, DefaultRootWindow(display), atom, 0, 1, False, XA_CARDINAL,
                           &type, &format, &count, &remaining, &data) == Success &&
            type == XA_CARDINAL && format == 32 && count == 1 && data != nullptr) {
        // Xlib returns 32-bit properties as longs
        value = uint32_t(*reinterpret_cast<unsigned long*>(data));
        found = true;
    }
    if (data != nullptr) {
        XFree(data);
    }
    return found;
}

struct XwaylandIdentity {
    bool gamescope = false;
    uint32_t pid = 0;
    uint32_t serverId = 0;
};

// Gamescope tags every Xwayland root with its own PID and the server's index
XwaylandIdentity identify(Display* display)
{
    XwaylandIdentity identity;
    const Atom pid = XInternAtom(display, "GAMESCOPE_PID", True);
    const Atom serverId = XInternAtom(display, "GAMESCOPE_XWAYLAND_SERVER_ID", True);
    identity.gamescope = pid != None && serverId != None &&
                         rootCardinal(display, pid, identity.pid) &&
                         rootCardinal(display, serverId, identity.serverId);
    return identity;
}

}

struct GamescopeDisplayState::Impl {
    Display* display = nullptr;
    Atom refreshRate = None;
    Atom vrrFeedback = None;
    Atom limiterFeedback = None;
    Atom allowTearing = None;

    // A missing property reads as zero, which is what Gamescope assumes too
    uint32_t cardinal(Atom atom)
    {
        uint32_t value = 0;
        rootCardinal(display, atom, value);
        return value;
    }
};

GamescopeDisplayState::GamescopeDisplayState() = default;

GamescopeDisplayState::~GamescopeDisplayState()
{
    if (m_Impl != nullptr) {
        if (m_Impl->display != nullptr) {
            XCloseDisplay(m_Impl->display);
        }
        delete m_Impl;
    }
}

bool GamescopeDisplayState::open()
{
    if (m_Impl != nullptr) {
        return m_Impl->display != nullptr;
    }
    m_Impl = new Impl();

    // Steam sets these properties, and Gamescope reports VRR, only on the
    // root window of Gamescope's first Xwayland, where Steam itself runs.
    // With more than one Xwayland (Steam's Game Mode runs two), games get
    // a later one as DISPLAY, whose root has none of them and would always
    // read as fixed refresh.
    Display* own = XOpenDisplay(nullptr);
    if (own == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: unable to open Gamescope's X display to read its presentation settings");
        return false;
    }
    const XwaylandIdentity ownIdentity = identify(own);
    if (!ownIdentity.gamescope || ownIdentity.serverId == 0) {
        // Gamescope's first Xwayland, or one too old to say which it is
        m_Impl->display = own;
    }
    else {
        // Look for the first Xwayland of the same Gamescope. Under Flatpak,
        // DISPLAY is renumbered but the others stay reachable through
        // their abstract sockets.
        for (int number = 0; number <= MaxXwaylandDisplay && m_Impl->display == nullptr; number++) {
            char name[16];
            snprintf(name, sizeof(name), ":%d", number);
            Display* candidate = XOpenDisplay(name);
            if (candidate == nullptr) {
                continue;
            }
            const XwaylandIdentity identity = identify(candidate);
            if (identity.gamescope && identity.pid == ownIdentity.pid && identity.serverId == 0) {
                m_Impl->display = candidate;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Timestamp pacing: reading Gamescope's presentation settings from Xwayland %s (DISPLAY %s is its server %u)",
                            name, DisplayString(own), ownIdentity.serverId);
            }
            else {
                XCloseDisplay(candidate);
            }
        }
        XCloseDisplay(own);
        if (m_Impl->display == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Timestamp pacing: unable to find Gamescope's first Xwayland to read its presentation settings");
            return false;
        }
    }
    m_Impl->refreshRate = XInternAtom(m_Impl->display, "GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK", False);
    m_Impl->vrrFeedback = XInternAtom(m_Impl->display, "GAMESCOPE_VRR_FEEDBACK", False);
    m_Impl->limiterFeedback = XInternAtom(m_Impl->display, "GAMESCOPE_LIMITER_FEEDBACK", False);
    m_Impl->allowTearing = XInternAtom(m_Impl->display, "GAMESCOPE_ALLOW_TEARING", False);
    return true;
}

GamescopeDisplayState::State GamescopeDisplayState::read()
{
    State state;
    if (m_Impl == nullptr || m_Impl->display == nullptr) {
        return state;
    }
    state.valid = true;
    state.refreshHz = m_Impl->cardinal(m_Impl->refreshRate);
    state.vrrInUse = m_Impl->cardinal(m_Impl->vrrFeedback) != 0;
    state.frameLimited = m_Impl->cardinal(m_Impl->limiterFeedback) != 0;
    state.tearingAllowed = m_Impl->cardinal(m_Impl->allowTearing) != 0;
    return state;
}

#else

struct GamescopeDisplayState::Impl {};

GamescopeDisplayState::GamescopeDisplayState() = default;

GamescopeDisplayState::~GamescopeDisplayState()
{
    delete m_Impl;
}

bool GamescopeDisplayState::open()
{
    return false;
}

GamescopeDisplayState::State GamescopeDisplayState::read()
{
    return State();
}

#endif

uint32_t GamescopeDisplayState::readRefreshHz()
{
    if (!runningUnderGamescope()) {
        return 0;
    }
    GamescopeDisplayState state;
    if (!state.open()) {
        return 0;
    }
    return state.read().refreshHz;
}
