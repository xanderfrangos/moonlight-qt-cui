#include "overlaymanager.h"
#include "path.h"
#include <exception>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>

using namespace Overlay;

namespace {
// CPU rasterization runs on the overlay worker. All backends receive the same
// composed surface through their current overlay upload path; no GPU API work
// or text rendering happens on the frame path.
// Draws the lanes above the optional stats text surface, which it consumes.
SDL_Surface* composeTimingGraph(TTF_Font* font, SDL_Surface* text, const TimingGraphSnapshot& points, double scale)
{
    using Layout = TimingGraphLayout;
    const size_t first = points.size() > size_t(Layout::Frames) ? points.size() - Layout::Frames : 0;
    const size_t count = points.size() - first;
    if (count < 2) return text;
    const auto S = [scale](double value) { return int(std::lround(value * scale)); };

    std::array<uint64_t, Layout::Frames> planned{};
    size_t plannedCount = 0;
    for (size_t i = first; i < points.size(); ++i) {
        uint64_t interval;
        if (timingGraphInterval(points, i, TimingGraphLane::Target, interval)) planned[plannedCount++] = interval;
    }
    uint64_t nominal = 8333;
    if (plannedCount) {
        auto middle = planned.begin() + plannedCount / 2;
        std::nth_element(planned.begin(), middle, planned.begin() + plannedCount);
        nominal = *middle; // Axis placement only; plotted intervals stay raw.
    }
    const double minimum = std::max(0.0, double(nominal) - Layout::RadiusUs);
    const double maximum = double(nominal) + Layout::RadiusUs;

    const int textWidth = text ? text->w : 0, textHeight = text ? text->h : 0;
    const int width = std::max(textWidth, S(Layout::Width)), height = S(Layout::Height);
    auto surface = SDL_CreateRGBSurfaceWithFormat(0, width, textHeight + height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return text;
    SDL_FillRect(surface, nullptr, 0);
    if (text) {
        SDL_Rect textPosition{0, height, text->w, text->h};
        SDL_BlitSurface(text, nullptr, surface, &textPosition);
        SDL_FreeSurface(text);
    }
    SDL_Rect background{0, 0, S(Layout::Width), height};
    SDL_FillRect(surface, &background, SDL_MapRGBA(surface->format, 8, 12, 18, 235));

    const SDL_Color planColor{190, 198, 210, 255}, submitColor{65, 215, 250, 255};
    const SDL_Color displayColor{245, 110, 220, 255}, labelColor{225, 230, 235, 255};
    const SDL_Color gridColor{45, 53, 65, 255}, referenceColor{105, 113, 125, 255}, clipColor{255, 95, 65, 255};
    const auto label = [&](const char* value, int x, int y, SDL_Color c) {
        auto words = TTF_RenderUTF8_Blended(font, value, c);
        if (words) { SDL_Rect dest{x, y, words->w, words->h}; SDL_BlitSurface(words, nullptr, surface, &dest); SDL_FreeSurface(words); }
    };
    const auto line = [&](int x0, int y0, int x1, int y1, SDL_Color c, int thickness = 1) {
        const Uint32 pixel = SDL_MapRGBA(surface->format, c.r, c.g, c.b, c.a);
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            for (int stroke = 0; stroke < thickness; ++stroke) {
                const int y = y0 + stroke;
                if (x0 >= 0 && x0 < surface->w && y >= 0 && y < height)
                    reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels) + y * surface->pitch)[x0] = pixel;
            }
            if (x0 == x1 && y0 == y1) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    };

    char caption[160];
    std::snprintf(caption, sizeof(caption), "Frametimes: %zu frames, reference %.2f ms", count, nominal / 1000.0);
    label(caption, S(10), S(4), labelColor);
    label("ms", S(14), S(28), labelColor);

    const int left = S(Layout::Left), right = S(Layout::Width - Layout::RightMargin);
    const int stroke = std::max(1, S(1.5));
    const auto xAt = [&](size_t i) {
        return left + int((i - first + Layout::Frames - count) * size_t(right - left) / (Layout::Frames - 1));
    };
    const TimingGraphLane lanes[Layout::Lanes] = {TimingGraphLane::Target, TimingGraphLane::Submit, TimingGraphLane::Display};
    const SDL_Color colors[Layout::Lanes] = {planColor, submitColor, displayColor};
    const char* names[Layout::Lanes] = {"Planned cadence", "Client submissions", "Display events"};
    for (int lane = 0; lane < Layout::Lanes; ++lane) {
        const int top = S(Layout::plotTop(lane)), bottom = S(Layout::plotBottom(lane));
        const auto yAt = [&](double v) {
            return int(std::lround(bottom - (std::clamp(v, minimum, maximum) - minimum) / (maximum - minimum) * (bottom - top)));
        };
        size_t valid = 0;
        double latest = 0, peak = 0;
        for (size_t i = first; i < points.size(); ++i) {
            uint64_t interval;
            if (!timingGraphInterval(points, i, lanes[lane], interval)) continue;
            latest = double(interval); peak = std::max(peak, latest); ++valid;
        }
        if (!valid) std::snprintf(caption, sizeof(caption), "%s: unavailable", names[lane]);
        else if (lanes[lane] == TimingGraphLane::Display)
            std::snprintf(caption, sizeof(caption), "%s: %.2f ms, %zu matched, peak %.2f", names[lane], latest / 1000.0, valid, peak / 1000.0);
        else std::snprintf(caption, sizeof(caption), "%s: %.2f ms, peak %.2f", names[lane], latest / 1000.0, peak / 1000.0);
        label(caption, left, S(Layout::titleTop(lane)), colors[lane]);
        for (int tick = 0; tick <= 2; ++tick) {
            const double v = minimum + (maximum - minimum) * tick / 2;
            const int y = yAt(v);
            line(left, y, right, y, gridColor);
            char number[32]; std::snprintf(number, sizeof(number), "%.1f", v / 1000.0);
            label(number, S(8), y - S(8), labelColor);
        }
        const int referenceY = yAt(double(nominal));
        for (int x = left; x < right; x += S(8)) line(x, referenceY, std::min(x + S(3), right), referenceY, referenceColor);

        bool havePrevious = false;
        int previousX = 0, previousY = 0;
        for (size_t i = first; i < points.size(); ++i) {
            uint64_t interval;
            if (!timingGraphInterval(points, i, lanes[lane], interval)) { havePrevious = false; continue; }
            const double v = double(interval);
            const bool flat = std::abs(v - double(nominal)) <= Layout::FlatUs;
            const int x = xAt(i), y = yAt(flat ? double(nominal) : v);
            if (havePrevious) line(previousX, previousY, x, y, colors[lane], stroke);
            else line(x, y, x + 1, y, colors[lane], stroke);
            if (v < minimum || v > maximum) {
                const int inward = v > maximum ? S(4) : -S(4);
                line(x - S(3), y, x, y + inward, clipColor);
                line(x, y + inward, x + S(3), y, clipColor);
            }
            previousX = x; previousY = y; havePrevious = true;
        }
    }
    label("Flat within 1 ms. Red: clipped. Gaps: no OS feedback.", left, height - S(26), labelColor);
    return surface;
}

}

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayDebugGraphs].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebugGraphs].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    m_Overlays[OverlayType::OverlayMenuBackground].color = {0xFF, 0xFF, 0xFF, 0xFF};
    m_Overlays[OverlayType::OverlayMenuBackground].fontSize = 20;

    SDL_AtomicSet(&m_Overlays[OverlayType::OverlayDebug].anchor, OverlayAnchorTopLeft);
    SDL_AtomicSet(&m_Overlays[OverlayType::OverlayDebugGraphs].anchor, OverlayAnchorTopRight);
    SDL_AtomicSet(&m_Overlays[OverlayType::OverlayMenuBackground].anchor, OverlayAnchorFill);
    SDL_AtomicSet(&m_Overlays[OverlayType::OverlayStatusUpdate].anchor, OverlayAnchorBottomLeft);

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
    m_TtfInitialized = true;
    try { m_Worker = std::thread(&OverlayManager::run, this); }
    catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Overlay worker creation failed: %s", e.what());
    }
}

OverlayManager::~OverlayManager()
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        m_Stopping = true;
    }
    m_WorkReady.notify_one();
    if (m_Worker.joinable()) m_Worker.join();
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].paintedSurface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].paintedSurface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    if (m_TtfInitialized) TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    // Deliberately lock-free. See enabledForReaders.
    return SDL_AtomicGet(&m_Overlays[type].enabledForReaders) != 0;
}

bool OverlayManager::isStatsEnabled()
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_StatsEnabled;
}

bool OverlayManager::isTimingGraphEnabled()
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_TimingGraphEnabled;
}

void OverlayManager::setTimingGraphState(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        if (m_TimingGraphEnabled == enabled) return;
        m_TimingGraphEnabled = enabled;
        if (!enabled) m_TimingGraph.reset();
        auto& overlay = m_Overlays[OverlayDebug];
        overlay.enabled = m_StatsEnabled || m_TimingGraphEnabled;
        SDL_AtomicSet(&overlay.enabledForReaders, overlay.enabled);
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

void OverlayManager::setStatusMessage(StatusSource source, const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        m_StatusMessages[static_cast<int>(source)] = text;

        // While a pre-rendered surface (the gamepad menu) owns the overlay, the
        // messages wait. They're published when the surface is removed.
        if (m_StatusOverlayPainted || !publishStatusMessagesLocked()) {
            return;
        }
    }
    m_WorkReady.notify_one();
}

bool OverlayManager::publishStatusMessagesLocked()
{
    std::string combined = m_StatusMessages[static_cast<int>(StatusSource::Mouse)];
    if (combined.empty()) {
        combined = m_StatusMessages[static_cast<int>(StatusSource::Network)];
        const auto& client = m_StatusMessages[static_cast<int>(StatusSource::ClientPacing)];
        if (!combined.empty() && !client.empty()) combined += "\n\n";
        combined += client;
    }

    auto& overlay = m_Overlays[OverlayStatusUpdate];
    const bool enabled = !combined.empty();
    if (combined == overlay.text && overlay.enabled == enabled &&
            (SDL_AtomicGet(&overlay.enabledForReaders) != 0) == enabled) {
        return false;
    }

    SDL_utf8strlcpy(overlay.text, combined.c_str(), sizeof(overlay.text));
    overlay.enabled = enabled;
    // Renderers keep drawing their last texture while this says the overlay
    // is enabled, so it must follow 'enabled'
    SDL_AtomicSet(&overlay.enabledForReaders, enabled);
    ++overlay.revision;
    overlay.dirty = true;
    overlay.queued = std::chrono::steady_clock::now();
    return true;
}

std::string OverlayManager::getOverlayText(OverlayType type)
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text, TimingGraphSnapshot graph)
{
    const auto snapshot = graph.empty() ? nullptr : std::make_shared<const TimingGraphSnapshot>(std::move(graph));
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        SDL_FreeSurface(overlay.paintedSurface);
        overlay.paintedSurface = nullptr;
        if (type == OverlayDebug && m_TimingGraphEnabled) m_TimingGraph = snapshot;
        SDL_utf8strlcpy(overlay.text, text, sizeof(overlay.text));
        queueContentChangeLocked(type);
    }
    m_WorkReady.notify_one();
}

void OverlayManager::queueContentChangeLocked(OverlayType type)
{
    auto& overlay = m_Overlays[type];

    // A disabled overlay shows nothing whatever its content, so there is
    // nothing to redraw. Leave its revision alone too: bumping it would make
    // the worker discard an in-flight hide, and nothing would queue it again.
    if (!overlay.enabled) {
        return;
    }

    ++overlay.revision;
    overlay.dirty = true;
    overlay.queued = std::chrono::steady_clock::now();
}

void OverlayManager::setOverlayAnchor(OverlayType type, OverlayAnchor anchor)
{
    SDL_AtomicSet(&m_Overlays[type].anchor, anchor);

    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        queueContentChangeLocked(type);
    }
    m_WorkReady.notify_one();
}

void OverlayManager::updateTimingGraph(TimingGraphSnapshot graph)
{
    if (graph.empty()) return;
    auto snapshot = std::make_shared<const TimingGraphSnapshot>(std::move(graph));
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[OverlayDebug];
        if (!m_TimingGraphEnabled) return;
        m_TimingGraph = std::move(snapshot);
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

int OverlayManager::getOverlayWidth(OverlayType type)
{
    return SDL_AtomicGet(&m_Overlays[type].publishedWidth);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    std::lock_guard<std::mutex> lock(m_StateLock);
    return m_Overlays[type].fontSize;
}

void OverlayManager::setOutputSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    const int fontSize = std::max(width, height) >= 3840 && std::min(width, height) >= 2160 ? 26 : 20;
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& stats = m_Overlays[OverlayDebug];
        if (stats.fontSize == fontSize) return;
        stats.fontSize = fontSize;
        ++stats.revision;
        stats.dirty = true;
        stats.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlaySurface(OverlayType type, SDL_Surface* surface, bool retain)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        SDL_FreeSurface(overlay.paintedSurface);
        overlay.paintedSurface = surface;
        overlay.paintedRetained = retain;
        queueContentChangeLocked(type);

        // The status messages share this overlay. Hand it back to them once
        // the surface that took it over is gone, turning the overlay off if
        // there are none.
        if (type == OverlayStatusUpdate) {
            const bool wasPainted = m_StatusOverlayPainted;
            m_StatusOverlayPainted = surface != nullptr;
            if (wasPainted && !m_StatusOverlayPainted) {
                publishStatusMessagesLocked();
            }
        }
    }
    m_WorkReady.notify_one();
}

void OverlayManager::setOverlayStyle(OverlayType type, OverlayAnchor anchor, SDL_Color color, SDL_Color background)
{
    // The anchor is published outside the lock because renderers read it on
    // their own threads when they pick up a new surface.
    SDL_AtomicSet(&m_Overlays[type].anchor, anchor);

    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        overlay.color = color;
        overlay.background = background;
        queueContentChangeLocked(type);
    }
    m_WorkReady.notify_one();
}

OverlayAnchor OverlayManager::getOverlayAnchor(OverlayType type)
{
    return (OverlayAnchor)SDL_AtomicGet(&m_Overlays[type].anchor);
}

void OverlayManager::getOverlayRect(OverlayAnchor anchor,
                                    int overlayWidth, int overlayHeight,
                                    int viewportWidth, int viewportHeight,
                                    bool originAtBottom, int& x, int& y, int& w, int& h)
{
    w = overlayWidth;
    h = overlayHeight;

    switch (anchor) {
    case OverlayAnchorFill:
        x = 0;
        y = 0;
        w = viewportWidth;
        h = viewportHeight;
        return;
    case OverlayAnchorCenter:
        x = (viewportWidth - overlayWidth) / 2;
        y = (viewportHeight - overlayHeight) / 2;
        break;
    case OverlayAnchorTopRight:
        x = viewportWidth - overlayWidth;
        y = originAtBottom ? viewportHeight - overlayHeight : 0;
        break;
    case OverlayAnchorBottomLeft:
        x = 0;
        y = originAtBottom ? 0 : viewportHeight - overlayHeight;
        break;
    case OverlayAnchorTopLeft:
    default:
        x = 0;
        y = originAtBottom ? viewportHeight - overlayHeight : 0;
        break;
    }

    // Keep the overlay on screen if it doesn't fit in the viewport
    x = SDL_max(0, x);
    y = SDL_max(0, y);
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(m_StateLock);
        auto& overlay = m_Overlays[type];
        if (type == OverlayDebug) {
            if (m_StatsEnabled == enabled) return;
            m_StatsEnabled = enabled;
            overlay.enabled = m_StatsEnabled || m_TimingGraphEnabled;
        }
        else {
            if (overlay.enabled == enabled) return;
            overlay.enabled = enabled;
        }
        SDL_AtomicSet(&overlay.enabledForReaders, overlay.enabled);
        // The pre-rendered surface is kept so re-enabling doesn't require the
        // producer to paint it again.
        if (!enabled) overlay.text[0] = 0;
        ++overlay.revision;
        overlay.dirty = true;
        overlay.queued = std::chrono::steady_clock::now();
    }
    m_WorkReady.notify_one();
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    // Wait for any callback into the previous renderer before allowing its
    // destruction. An in-progress CPU raster job is invalidated by revision.
    std::lock_guard<std::mutex> rendererLock(m_RendererLock);
    {
        std::lock_guard<std::mutex> stateLock(m_StateLock);
        m_Renderer = renderer;
        m_HaveRenderer = renderer != nullptr;
        for (auto& overlay : m_Overlays) {
            ++overlay.revision;
            overlay.dirty = m_HaveRenderer;
            overlay.queued = std::chrono::steady_clock::now();
        }
    }
    for (int i = 0; i < OverlayMax; ++i) SDL_FreeSurface(getUpdatedOverlaySurface(OverlayType(i)));
    m_WorkReady.notify_one();
}

void OverlayManager::run()
{
    using Clock = std::chrono::steady_clock;
    auto ns = [](Clock::duration d) { return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); };
    unsigned next = 0;
    std::array<int, OverlayMax> rasterFontSizes{}; // Worker-owned font cache sizes.
    for (;;) {
        OverlayType type;
        char text[1024];
        SDL_Color color, background;
        SDL_Surface* painted;
        bool enabled, drawText;
        int fontSize;
        std::shared_ptr<const TimingGraphSnapshot> graph;
        uint64_t revision;
        Clock::time_point queued;
        {
            std::unique_lock<std::mutex> lock(m_StateLock);
            m_WorkReady.wait(lock, [&] {
                if (m_Stopping) return true;
                if (!m_HaveRenderer) return false;
                for (const auto& overlay : m_Overlays) if (overlay.dirty) return true;
                return false;
            });
            if (m_Stopping) return;
            // One coalesced request per overlay; rotate to avoid starving status
            // messages if diagnostic text changes faster than it can be drawn.
            while (!m_Overlays[next].dirty) next = (next + 1) % OverlayMax;
            type = OverlayType(next);
            next = (next + 1) % OverlayMax;
            auto& overlay = m_Overlays[type];
            overlay.dirty = false;
            enabled = overlay.enabled;
            drawText = enabled && overlay.text[0] && (type != OverlayDebug || m_StatsEnabled);
            fontSize = overlay.fontSize;
            revision = overlay.revision;
            queued = overlay.queued;
            color = overlay.color;
            background = overlay.background;
            // Copied under the lock because the producer can replace it at any
            // time, unless the producer said it doesn't need it kept
            painted = nullptr;
            if (enabled && overlay.paintedSurface) {
                if (overlay.paintedRetained) {
                    painted = SDL_DuplicateSurface(overlay.paintedSurface);
                }
                else {
                    painted = overlay.paintedSurface;
                    overlay.paintedSurface = nullptr;
                }
            }
            SDL_memcpy(text, overlay.text, sizeof(text));
            if (type == OverlayDebug && m_TimingGraphEnabled) graph = m_TimingGraph;
        }
        const auto started = Clock::now();
        auto& overlay = m_Overlays[type];
        SDL_Surface* surface = painted;
        const bool drawGraph = surface == nullptr && enabled && graph && graph->size() >= 2;
        if (surface == nullptr && (drawText || drawGraph)) {
            if ((!overlay.font || rasterFontSizes[type] != fontSize) && !m_FontData.isEmpty()) {
                auto resized = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()), 1, fontSize);
                if (!resized) continue;
                if (overlay.font) TTF_CloseFont(overlay.font);
                overlay.font = resized;
                rasterFontSizes[type] = fontSize;
            }
            const double scale = type == OverlayDebug ? fontSize / 20.0 : 1.0;
            if (drawText) {
                if (overlay.font) surface = RenderTextOutlinedWrapped(overlay.font, text, color,
                                                                      {0, 0, 0, 255}, int(std::lround(4 * scale)), int(std::lround(1024 * scale)));
                if (!surface) continue; // Keep the last successful overlay on failure.
                if (background.a != 0) {
                    SDL_Surface* boxed = AddBackground(surface, background, fontSize / 2);
                    if (!boxed) { SDL_FreeSurface(surface); continue; }
                    SDL_FreeSurface(surface);
                    surface = boxed;
                }
            }
            if (drawGraph && overlay.font) surface = composeTimingGraph(overlay.font, surface, *graph, scale);
        }
        const auto rasterized = Clock::now();
        std::lock_guard<std::mutex> rendererLock(m_RendererLock);
        bool publish;
        {
            std::lock_guard<std::mutex> stateLock(m_StateLock);
            publish = !m_Stopping && m_Renderer && overlay.revision == revision;
            if (publish) {
                SDL_AtomicSet(&overlay.publishedWidth, surface ? surface->w : 0);
                surface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&overlay.surface, surface);
            }
        }
        SDL_FreeSurface(surface); // Superseded result or previous unconsumed surface.
        if (publish) {
            const auto dispatch = Clock::now();
            m_Renderer->notifyOverlayUpdated(type);
            m_Renderer->recordOverlayTiming({type, revision, ns(started - queued),
                                            ns(rasterized - started), ns(Clock::now() - dispatch)});
        }
    }
}

SDL_Surface* OverlayManager::AddBackground(SDL_Surface* textSurface, SDL_Color background, int padding)
{
    // Renderers require ARGB8888 overlay surfaces
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0,
                                                          textSurface->w + padding * 2,
                                                          textSurface->h + padding * 2,
                                                          32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return nullptr;
    }

    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, background.r, background.g,
                                               background.b, background.a));

    SDL_Rect dst = { padding, padding, textSurface->w, textSurface->h };
    if (SDL_BlitSurface(textSurface, nullptr, surface, &dst) != 0) {
        SDL_FreeSurface(surface);
        return nullptr;
    }

    return surface;
}

SDL_Surface* OverlayManager::RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth) {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    int oldOutline = TTF_GetFontOutline(font);
    TTF_SetFontOutline(font, outlineWidth);

    // Verify that the string won't require wrapping (which could cause the outline and the text
    // to diverge due to different wrapping positions).
    //
    // FIXME: We do this rather than just disabling wrapping entirely (wrapWidth = 0) because we
    // need further testing to ensure that all renderers can handle non-NPOT overlay textures.
    for (const QString& line : QString(text).split('\n')) {
        int extent, count;
        if (TTF_MeasureUTF8(font, line.toUtf8(), wrapWidth, &extent, &count) == 0 && count < line.size()) {
            // If it requires wrapping, render it without the outline
            TTF_SetFontOutline(font, oldOutline);
            return TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
        }
    }

    // Draw text twice, but outline is a bit bigger
    auto outlineSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, outlineColor, wrapWidth);
    TTF_SetFontOutline(font, 0);
    auto textSurface = TTF_RenderUTF8_Blended_Wrapped(font, text, textColor, wrapWidth);
    TTF_SetFontOutline(font, oldOutline);

    if (outlineSurface == nullptr || textSurface == nullptr) {
        SDL_FreeSurface(outlineSurface);
        SDL_FreeSurface(textSurface);
        return nullptr;
    }

    // Merge the texts
    SDL_Rect dst = { outlineWidth, outlineWidth, textSurface->w, textSurface->h };
    SDL_BlitSurface(textSurface, nullptr, outlineSurface, &dst);

    SDL_FreeSurface(textSurface);
    return outlineSurface;
}
