#pragma once

#include <QString>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    // Plotted history of the metrics the debug overlay reports as running
    // averages. Drawn before the menu background so the menu dims it too.
    OverlayDebugGraphs,
    // Dims everything behind it, so it must be drawn after the overlays it
    // covers and before the menu it sits behind.
    OverlayMenuBackground,
    OverlayStatusUpdate,
    OverlayMax
};

enum OverlayAnchor {
    OverlayAnchorTopLeft,
    OverlayAnchorTopRight,
    OverlayAnchorBottomLeft,
    OverlayAnchorCenter,
    // Stretches the overlay over the whole viewport, so a solid fill can be
    // uploaded as a tiny texture instead of a screen-sized one.
    OverlayAnchorFill,
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;

    struct UpdateTiming {
        OverlayType type;
        uint64_t revision;
        int64_t queueNs, rasterNs, dispatchNs;
    };
    void recordOverlayTiming(UpdateTiming timing) {
        std::lock_guard<std::mutex> lock(m_TimingLock);
        m_Timing[timing.type] = timing;
        m_HaveTiming[timing.type] = true;
    }
    bool takeOverlayTiming(UpdateTiming& timing) {
        std::lock_guard<std::mutex> lock(m_TimingLock);
        for (int i = 0; i < OverlayMax; ++i) if (m_HaveTiming[i]) {
            timing = m_Timing[i]; m_HaveTiming[i] = false; return true;
        }
        return false;
    }
private:
    std::mutex m_TimingLock;
    UpdateTiming m_Timing[OverlayMax] = {};
    bool m_HaveTiming[OverlayMax] = {};
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    std::string getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    // Publishes a pre-rendered overlay in place of the built-in text rasterizer.
    // Ownership of the surface transfers to the overlay manager, which retains it
    // so the overlay survives renderer recreation. Passing nullptr returns the
    // overlay to text rendering. Surfaces must be ARGB8888.
    void setOverlaySurface(OverlayType type, SDL_Surface* surface);

    // Restyles an overlay. This takes effect on the next updateOverlayText()
    // or setOverlayState() call for that overlay. A background with zero alpha
    // draws the text directly over the video with no box behind it.
    void setOverlayStyle(OverlayType type, OverlayAnchor anchor, SDL_Color color, SDL_Color background);

    // Safe to call from render threads without blocking overlay producers
    OverlayAnchor getOverlayAnchor(OverlayType type);

    // Places an overlay of the given size within a viewport and returns the size
    // it should be drawn at, which differs from the surface size for anchors that
    // scale. Renderers whose coordinate space puts the origin in the lower-left
    // corner (OpenGL, D3D11 NDC) pass originAtBottom.
    static void getOverlayRect(OverlayAnchor anchor,
                               int overlayWidth, int overlayHeight,
                               int viewportWidth, int viewportHeight,
                               bool originAtBottom,
                               int& x, int& y, int& w, int& h);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void run();
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);
    static SDL_Surface* AddBackground(SDL_Surface* textSurface, SDL_Color background, int padding);

    struct {
        bool enabled = false;
        bool dirty = false;
        uint64_t revision = 0;
        std::chrono::steady_clock::time_point queued;
        int fontSize = 0;
        SDL_Color color = {};
        SDL_Color background = {};
        char text[1024] = {};

        // Read by renderers without m_StateLock held
        SDL_atomic_t anchor = {};

        // Pre-rendered overlay from setOverlaySurface(), retained so it can be
        // republished when the renderer changes. Guarded by m_StateLock.
        SDL_Surface* paintedSurface = nullptr;

        TTF_Font* font = nullptr; // Owned exclusively by the overlay worker.
        SDL_Surface* surface = nullptr; // Atomic ownership transfer to renderer.
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    std::mutex m_StateLock;
    std::condition_variable m_WorkReady;
    // Only renderer attachment and callbacks take this lock. Producers never
    // wait for rasterization, texture upload or renderer destruction.
    std::mutex m_RendererLock;
    bool m_HaveRenderer = false, m_Stopping = false, m_TtfInitialized = false;
    std::thread m_Worker;
};

}
