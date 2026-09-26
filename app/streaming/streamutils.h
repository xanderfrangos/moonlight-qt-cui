#pragma once

#include "SDL_compat.h"

class QScreen;

class StreamUtils
{
public:
    static
    Uint32 getPlatformWindowFlags();

    static
    SDL_Window* createTestWindow();

    static
    void scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst);

    static
    void screenSpaceToNormalizedDeviceCoords(SDL_FRect* rect, int viewportWidth, int viewportHeight);

    static
    void screenSpaceToNormalizedDeviceCoords(SDL_Rect* src, SDL_FRect* dst, int viewportWidth, int viewportHeight);

    static
    bool getNativeDesktopMode(int displayIndex, SDL_DisplayMode* mode, SDL_Rect* safeArea);

    // The SDL display showing a Qt screen, which is where a stream started
    // from that screen opens. Returns 0 if no display matches.
    static
    int getDisplayIndexForScreen(QScreen* screen);

    // The resolution and refresh rate a display is currently outputting, used
    // by the Native resolution and frame rate options. A value is 0 if unknown.
    static
    void getDisplayOutputMode(int displayIndex, int& width, int& height, int& refreshHz);

    static
    int getDisplayRefreshRate(SDL_Window* window);

    // Unlike getDisplayRefreshRate(), this does not guess 60 Hz.  It is used
    // to decide whether a session may enter the opt-in VRR path, where an
    // invented refresh rate would produce an invalid pacing configuration.
    static
    bool tryGetDisplayRefreshRate(SDL_Window* window, int& outHz);

    static
    bool hasFastAes();

    static
    int getDrmFdForWindow(SDL_Window* window, bool* needsClose);

    static
    int getDrmFd(bool preferRenderNode);

    static
    void enterAsyncLoggingMode();

    static
    void exitAsyncLoggingMode();
};
