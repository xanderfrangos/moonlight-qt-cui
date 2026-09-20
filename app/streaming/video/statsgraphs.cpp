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
            m_OverlayManager->setOverlaySurface(OverlayDebugGraphs,
                                                Painter::paintStatsGraphs(m_Points,
                                                                          k_MaxSamples,
                                                                          k_WindowSeconds));
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

    // Latencies are averages over the interval rather than counters. An
    // interval with no frames in it carries no new measurement, so hold the
    // previous value instead of plotting a zero that would read as an
    // improvement.
    if (!m_Points.empty()) {
        point.hostProcessingLatencyMs = m_Points.back().hostProcessingLatencyMs;
        point.networkLatencyMs = m_Points.back().networkLatencyMs;
    }

    if (intervalSecs > 0) {
        point.renderedFps = (float)(delta(counters.renderedFrames,
                                          m_LastCounters.renderedFrames) / intervalSecs);
    }
    point.networkDroppedFrames = (float)delta(counters.networkDroppedFrames,
                                              m_LastCounters.networkDroppedFrames);
    point.jitterDroppedFrames = (float)delta(counters.jitterDroppedFrames,
                                             m_LastCounters.jitterDroppedFrames);

    const uint64_t latencyFrames = delta(counters.framesWithHostProcessingLatency,
                                         m_LastCounters.framesWithHostProcessingLatency);
    if (latencyFrames > 0) {
        point.hostProcessingLatencyMs =
                (float)(delta(counters.totalHostProcessingLatency,
                              m_LastCounters.totalHostProcessingLatency) /
                        (latencyFrames * 10.0));
    }

    if (counters.networkLatencyValid) {
        point.networkLatencyMs = (float)counters.networkLatencyMs;
    }

    m_LastCounters = counters;

    if (m_Points.size() == (size_t)k_MaxSamples) {
        m_Points.erase(m_Points.begin());
    }
    m_Points.push_back(point);
}
