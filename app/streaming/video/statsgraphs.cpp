#include "statsgraphs.h"
#include "overlaymanager.h"
#include "overlaypainter.h"

#include <algorithm>
#include <chrono>
#include <exception>

using namespace Overlay;

StatsGraphs::~StatsGraphs()
{
    stop();
}

void StatsGraphs::start(OverlayManager* overlayManager,
                        const StatsGraphConfig& config,
                        std::function<void(StatsGraphCounters&)> sampler)
{
    // Initialization can be retried with a different renderer without an
    // intervening reset(), so don't assume the previous run is already gone.
    stop();

    m_OverlayManager = overlayManager;
    m_Config = config;
    m_MaxSamples = std::max(2, (config.windowSeconds * 1000) / k_SampleIntervalMs);
    m_Sampler = std::move(sampler);
    m_Stopping = false;
    m_Points.clear();
    m_LastCounters = {};
    m_HaveLastCounters = false;

    try {
        m_Thread = std::thread(&StatsGraphs::run, this);
    }
    catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Stats graph sampler creation failed: %s", e.what());
    }
}

void StatsGraphs::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Stopping = true;
    }
    m_Wake.notify_one();
    if (m_Thread.joinable()) {
        m_Thread.join();
    }

    if (m_OverlayManager != nullptr) {
        m_OverlayManager->setOverlaySurface(OverlayDebugGraphs, nullptr);
        m_OverlayManager = nullptr;
    }
    m_Sampler = nullptr;
}

void StatsGraphs::setViewportSize(int width, int height)
{
    m_ViewportWidth.store(width, std::memory_order_relaxed);
    m_ViewportHeight.store(height, std::memory_order_relaxed);
}

float StatsGraphs::scale() const
{
    if (m_Config.sizePercent > 0) {
        return m_Config.sizePercent / 100.0f;
    }

    // The same range the gamepad menu scales over, authored against 1080p
    const int viewportHeight = m_ViewportHeight.load(std::memory_order_relaxed);
    return viewportHeight > 0 ? std::clamp(viewportHeight / 1080.0f, 0.75f, 3.0f) : 1.0f;
}

void StatsGraphs::run()
{
    using Clock = std::chrono::steady_clock;
    constexpr auto interval = std::chrono::milliseconds(k_SampleIntervalMs);

    auto lastSample = Clock::now();
    auto nextSample = lastSample + interval;
    auto lastRepaint = Clock::time_point{};
    bool wasEnabled = false;
    // Whether nothing is published, so a card with nothing to show isn't
    // republished (and its texture rebuilt) on every repaint.
    bool publishedBlank = true;
    // Widest the text overlay has been since it appeared. Its width changes
    // with every update, and following it exactly would resize a card that
    // has been squeezed beside it once a second.
    int textWidth = 0;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_Lock);
            if (m_Wake.wait_until(lock, nextSample, [this] { return m_Stopping; })) {
                return;
            }
        }

        const auto now = Clock::now();
        nextSample += interval;

        // Don't replay a long stall (a suspended machine, say) as a burst of
        // catch-up samples that would all land in the same instant.
        if (now > nextSample + interval * 4) {
            nextSample = now + interval;
        }

        StatsGraphCounters counters;
        m_Sampler(counters);
        appendSample(counters,
                     std::chrono::duration<double>(now - lastSample).count());
        lastSample = now;

        const bool enabled = m_OverlayManager->isOverlayEnabled(OverlayDebugGraphs);
        if (enabled) {
            // Repaint on its own slower schedule, but always immediately on the
            // tick the graphs are turned on so they don't appear blank.
            if (!wasEnabled ||
                    now - lastRepaint >= std::chrono::milliseconds(k_RepaintIntervalMs)) {
                // The summary stands in for the text overlay, so it only
                // appears while that is hidden.
                const bool textShown = m_OverlayManager->isOverlayEnabled(OverlayDebug);

                // Fit beside the text overlay, which sits on the other side
                // of the stream, rather than overlapping it. Its width is
                // only known once it has been drawn, so the card may overlap
                // it for a moment when the two appear together.
                textWidth = textShown ? std::max(textWidth, m_OverlayManager->getOverlayWidth(OverlayDebug))
                                      : 0;
                int maxWidth = m_ViewportWidth.load(std::memory_order_relaxed);
                const int maxHeight = m_ViewportHeight.load(std::memory_order_relaxed);
                if (maxWidth > 0) {
                    maxWidth = std::max(1, maxWidth - textWidth);
                }

                SDL_Surface* surface = Painter::paintStatsGraphs(m_Points,
                                                                 m_MaxSamples,
                                                                 m_Config,
                                                                 counters.streamInfo,
                                                                 !textShown,
                                                                 scale(),
                                                                 QSize(maxWidth, maxHeight));
                if (surface != nullptr || !publishedBlank) {
                    // Not retained: this repaints often enough to cover a
                    // renderer change by itself.
                    m_OverlayManager->setOverlaySurface(OverlayDebugGraphs, surface, false);
                }
                publishedBlank = surface == nullptr;
                lastRepaint = now;
            }
        }
        else if (wasEnabled) {
            // Don't keep a megabyte of pixels around while they aren't on screen
            m_OverlayManager->setOverlaySurface(OverlayDebugGraphs, nullptr);
            publishedBlank = true;
        }
        wasEnabled = enabled;
    }
}

void StatsGraphs::appendSample(const StatsGraphCounters& counters, double intervalSecs)
{
    // Counters restart when the decoder is reinitialized mid-session. Treat a
    // decrease as a fresh baseline rather than letting unsigned math
    // manufacture an enormous delta.
    auto delta = [](uint64_t current, uint64_t previous) -> uint64_t {
        return current >= previous ? current - previous : 0;
    };

    if (!m_HaveLastCounters) {
        // The first read only establishes the baseline. There's nothing to
        // plot until there are two of them to subtract.
        m_LastCounters = counters;
        m_HaveLastCounters = true;
        return;
    }

    StatsGraphPoint point;
    const float intervalMs = (float)(intervalSecs * 1000.0);

    // An interval with no frames in it carries no new measurement. For
    // frametime the interval itself is a lower bound on the real value, which
    // reads as a spike; for the latencies there is no bound to infer, so the
    // previous value is held rather than plotting a zero that would look like
    // an improvement.
    auto applyPerFrame = [&](const StatsGraphAccumulator& accumulator,
                             float previousAverage, float fallback,
                             float& average, float& minimum, float& maximum) {
        if (accumulator.count != 0) {
            average = accumulator.average();
            minimum = accumulator.min;
            maximum = accumulator.max;
        }
        else {
            average = fallback > 0 ? fallback : previousAverage;
            minimum = maximum = average;
        }
    };

    const StatsGraphPoint previous = m_Points.empty() ? StatsGraphPoint() : m_Points.back();

    applyPerFrame(counters.incomingFrametime, previous.incomingFrametimeMs, intervalMs,
                  point.incomingFrametimeMs, point.incomingFrametimeMinMs,
                  point.incomingFrametimeMaxMs);
    applyPerFrame(counters.renderingFrametime, previous.renderingFrametimeMs, intervalMs,
                  point.renderingFrametimeMs, point.renderingFrametimeMinMs,
                  point.renderingFrametimeMaxMs);
    applyPerFrame(counters.hostProcessingLatency, previous.hostProcessingLatencyMs, 0,
                  point.hostProcessingLatencyMs, point.hostProcessingLatencyMinMs,
                  point.hostProcessingLatencyMaxMs);
    applyPerFrame(counters.reassembly, previous.reassemblyMs, 0,
                  point.reassemblyMs, point.reassemblyMinMs, point.reassemblyMaxMs);
    applyPerFrame(counters.decodingTime, previous.decodingTimeMs, 0,
                  point.decodingTimeMs, point.decodingTimeMinMs, point.decodingTimeMaxMs);
    applyPerFrame(counters.renderingTime, previous.renderingTimeMs, 0,
                  point.renderingTimeMs, point.renderingTimeMinMs, point.renderingTimeMaxMs);
    applyPerFrame(counters.decodingFrametime, previous.decodingFrametimeMs, intervalMs,
                  point.decodingFrametimeMs, point.decodingFrametimeMinMs,
                  point.decodingFrametimeMaxMs);

    if (intervalSecs > 0) {
        point.videoMbps = (float)(delta(counters.videoBytes, m_LastCounters.videoBytes) *
                                  8.0 / 1000000.0 / intervalSecs);

        // The packet counts only restart with the connection, and start()
        // takes a fresh baseline for that, so a decrease here is a 32-bit
        // wrap that plain unsigned subtraction already handles.
        const uint32_t packets =
                (uint32_t)(counters.videoDataPackets - m_LastCounters.videoDataPackets) +
                (uint32_t)(counters.videoFecPackets - m_LastCounters.videoFecPackets);
        point.networkMbps = (float)((double)packets * counters.videoPacketWireBytes *
                                    8.0 / 1000000.0 / intervalSecs);
    }
    point.networkDroppedFrames = (float)delta(counters.networkDroppedFrames,
                                              m_LastCounters.networkDroppedFrames);
    point.jitterDroppedFrames = (float)delta(counters.jitterDroppedFrames,
                                             m_LastCounters.jitterDroppedFrames);

    if (counters.networkLatencyValid) {
        point.networkLatencyMs = (float)counters.networkLatencyMs;
        point.networkJitterMs = (float)counters.networkJitterMs;
    }
    else {
        point.networkLatencyMs = previous.networkLatencyMs;
        point.networkJitterMs = previous.networkJitterMs;
    }

    point.queueDepth = (float)counters.queueDepth;

    m_LastCounters = counters;

    if (m_Points.size() >= (size_t)m_MaxSamples) {
        m_Points.erase(m_Points.begin());
    }
    m_Points.push_back(point);
}
