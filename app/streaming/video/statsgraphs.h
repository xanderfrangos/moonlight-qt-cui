#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Overlay {

class OverlayManager;

// Cumulative counters read once per sampling interval. The graphs plot the
// difference between consecutive reads, so producers only have to keep
// monotonic totals instead of maintaining a second set of short windows
// alongside the one-second windows the text overlay uses.
struct StatsGraphCounters {
    uint64_t renderedFrames = 0;
    uint64_t networkDroppedFrames = 0;
    uint64_t jitterDroppedFrames = 0;
    // Tenths of a millisecond, matching VIDEO_STATS
    uint64_t totalHostProcessingLatency = 0;
    uint64_t framesWithHostProcessingLatency = 0;
    uint32_t networkLatencyMs = 0;
    bool networkLatencyValid = false;
};

// One sampling interval's worth of plotted values.
struct StatsGraphPoint {
    float renderedFps = 0;
    float hostProcessingLatencyMs = 0;
    float networkDroppedFrames = 0;
    float jitterDroppedFrames = 0;
    float networkLatencyMs = 0;
};

// Samples stream statistics on a fixed interval and publishes a painted plot of
// the last few seconds to the debug graph overlay. Sampling runs whether or not
// the overlay is visible, so the graphs already cover a full window when the
// user brings them up. Painting only happens while they're on screen.
class StatsGraphs
{
public:
    static constexpr int k_SampleIntervalMs = 100;
    static constexpr int k_WindowSeconds = 10;
    static constexpr int k_MaxSamples = (k_WindowSeconds * 1000) / k_SampleIntervalMs;

    StatsGraphs() = default;
    ~StatsGraphs();

    // The sampler is invoked on the sampling thread once per interval and must
    // fill in the counters it owns. stop() joins that thread, so it has to be
    // called before anything the sampler touches is torn down.
    void start(OverlayManager* overlayManager,
               std::function<void(StatsGraphCounters&)> sampler);
    void stop();

private:
    void run();
    void appendSample(const StatsGraphCounters& counters, double intervalSecs);

    // Owned exclusively by the sampling thread
    std::vector<StatsGraphPoint> m_Points;
    StatsGraphCounters m_LastCounters;
    bool m_HaveLastCounters = false;

    OverlayManager* m_OverlayManager = nullptr;
    std::function<void(StatsGraphCounters&)> m_Sampler;
    std::mutex m_Lock;
    std::condition_variable m_Wake;
    bool m_Stopping = false;
    std::thread m_Thread;
};

}
