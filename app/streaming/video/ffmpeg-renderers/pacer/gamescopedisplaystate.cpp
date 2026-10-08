#include "gamescopedisplaystate.h"

#include <SDL.h>

#if defined(__linux__) && defined(HAS_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>

struct GamescopeDisplayState::Impl {
    Display* display = nullptr;
    Atom fpsLimit = None;
    Atom vrrFeedback = None;
    Atom allowTearing = None;

    // A missing property reads as zero, which is what Gamescope assumes too
    uint32_t cardinal(Atom atom)
    {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        uint32_t value = 0;
        if (XGetWindowProperty(display, DefaultRootWindow(display), atom, 0, 1, False, XA_CARDINAL,
                               &type, &format, &count, &remaining, &data) == Success &&
                type == XA_CARDINAL && format == 32 && count == 1 && data != nullptr) {
            // Xlib returns 32-bit properties as longs
            value = uint32_t(*reinterpret_cast<unsigned long*>(data));
        }
        if (data != nullptr) {
            XFree(data);
        }
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

bool GamescopeDisplayState::runningUnderGamescope()
{
    const char* wayland = SDL_getenv("GAMESCOPE_WAYLAND_DISPLAY");
    return wayland != nullptr && wayland[0] != '\0';
}

bool GamescopeDisplayState::open()
{
    if (m_Impl != nullptr) {
        return m_Impl->display != nullptr;
    }
    m_Impl = new Impl();

    // Apps under Gamescope get its Xwayland as DISPLAY, whose root window
    // carries these properties even for a Wayland client
    m_Impl->display = XOpenDisplay(nullptr);
    if (m_Impl->display == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Timestamp pacing: unable to open Gamescope's X display to read its presentation settings");
        return false;
    }
    m_Impl->fpsLimit = XInternAtom(m_Impl->display, "GAMESCOPE_FPS_LIMIT", False);
    m_Impl->vrrFeedback = XInternAtom(m_Impl->display, "GAMESCOPE_VRR_FEEDBACK", False);
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
    state.fpsLimit = m_Impl->cardinal(m_Impl->fpsLimit);
    state.vrrInUse = m_Impl->cardinal(m_Impl->vrrFeedback) != 0;
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

bool GamescopeDisplayState::runningUnderGamescope()
{
    return false;
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
