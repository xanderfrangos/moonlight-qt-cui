#include "statsgraphs.h"
#include "overlaymanager.h"
#include "overlaypainter.h"

#include <chrono>
#include <exception>

using namespace Overlay;

StatsGraphs::~StatsGraphs()
{
    stop();
}

void StatsGraphs::start(OverlayManager* overlayManager,
                        std::function<void(StatsGraphCounters&)> sampler)
{
    // Initialization can be retried with a different renderer without an
    // intervening reset(), so don't assume the previous run is already gone.
    stop();

    m_OverlayManager = overlayManager;
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

void StatsGraphs::run()
{
    using Clock = std::chrono::steady_clock;
    constexpr auto interval = std::chrono::milliseconds(k_SampleIntervalMs);

    auto lastSample = Clock::now();
    auto nextSample = lastSample + interval;
    auto lastRepaint = Clock::time_point{};
    bool wasEnabled = false;

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
                m_OverlayManager->setOverlaySurface(OverlayDebugGraphs,
                                                    Painter::paintStatsGraphs(m_Points,
                                                                              k_MaxSamples,
                                                                              k_WindowSeconds));
                lastRepaint = now;
            }
        }
        else if (wasEnabled) {
            // Don't keep a megabyte of pixels around while they aren't on screen
            m_OverlayManager->setOverlaySurface(OverlayDebugGraphs, nullptr);
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

    if (m_Points.size() == (size_t)k_MaxSamples) {
        m_Points.erase(m_Points.begin());
    }
    m_Points.push_back(point);
}
