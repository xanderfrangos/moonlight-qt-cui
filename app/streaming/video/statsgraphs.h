#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Overlay {

class OverlayManager;

// Per-frame values seen within one sampling interval. Producers accumulate into
// this and the sampling thread takes it, so the window it covers lines up
// exactly with the interval being plotted. Keeping the spread rather than only
// the mean is the point: an interval average hides the single late frame that
// is actually felt as a stutter.
struct StatsGraphAccumulator {
    uint32_t count = 0;
    double sum = 0;
    float min = 0;
    float max = 0;

    void add(float value)
    {
        if (count == 0 || value < min) {
            min = value;
        }
        if (count == 0 || value > max) {
            max = value;
        }
        sum += value;
        count++;
    }

    float average() const { return count != 0 ? (float)(sum / count) : 0; }
};

// Cumulative counters read once per sampling interval. The graphs plot the
// difference between consecutive reads, so producers only have to keep
// monotonic totals instead of maintaining a second set of short windows
// alongside the one-second windows the text overlay uses.
struct StatsGraphCounters {
    // Video payload as delivered, not including FEC overhead
    uint64_t videoBytes = 0;
    // Packets the host sent, data and FEC parity, as announced by each FEC
    // block header. These are 32-bit and wrap, unlike the other totals.
    uint32_t videoDataPackets = 0;
    uint32_t videoFecPackets = 0;
    // Size of every one of those packets on the wire, IP header included.
    // The host pads all shards of a block to the same size for FEC, so this
    // is a constant for the session.
    uint32_t videoPacketWireBytes = 0;
    uint64_t networkDroppedFrames = 0;
    uint64_t jitterDroppedFrames = 0;
    uint32_t networkLatencyMs = 0;
    uint32_t networkJitterMs = 0;
    bool networkLatencyValid = false;
    // A gauge, not a counter: frames buffered ahead of display right now
    uint32_t queueDepth = 0;

    // Per-frame values, taken and reset on every sample. All in milliseconds.
    StatsGraphAccumulator incomingFrametime;
    StatsGraphAccumulator renderingFrametime;
    StatsGraphAccumulator hostProcessingLatency;
    StatsGraphAccumulator reassembly;
};

// One sampling interval's worth of plotted values.
struct StatsGraphPoint {
    // Milliseconds per frame. The graphs show the equivalent frame rate
    // alongside, since variance is far easier to read in frametime than in an
    // averaged rate. Values measured per frame carry their spread across the
    // interval as well as the mean; the rest have only one sample per interval
    // and leave min and max equal to the value.
    float incomingFrametimeMs = 0;
    float incomingFrametimeMinMs = 0;
    float incomingFrametimeMaxMs = 0;
    float renderingFrametimeMs = 0;
    float renderingFrametimeMinMs = 0;
    float renderingFrametimeMaxMs = 0;
    float hostProcessingLatencyMs = 0;
    float hostProcessingLatencyMinMs = 0;
    float hostProcessingLatencyMaxMs = 0;
    float reassemblyMs = 0;
    float reassemblyMinMs = 0;
    float reassemblyMaxMs = 0;
    float videoMbps = 0;
    // Everything the video stream puts on the network: payload, FEC parity,
    // shard padding and packet headers
    float networkMbps = 0;
    float networkDroppedFrames = 0;
    float jitterDroppedFrames = 0;
    float networkLatencyMs = 0;
    float networkJitterMs = 0;
    float queueDepth = 0;
};

// Samples stream statistics on a fixed interval and publishes a painted plot of
// the last few seconds to the debug graph overlay. Sampling runs whether or not
// the overlay is visible, so the graphs already cover a full window when the
// user brings them up. Painting only happens while they're on screen.
class StatsGraphs
{
public:
    static constexpr int k_SampleIntervalMs = 100;
    // Repainting is deliberately slower than sampling. Every sample still
    // reaches the plot; only the refresh rate differs. A republish costs a
    // megabyte-scale surface allocation, a copy of it inside the overlay
    // manager, and a GPU texture rebuild in the renderer, all while the render
    // thread contends for the same overlay state lock, so doing it ten times a
    // second is far more pressure than any other overlay in this app applies.
    static constexpr int k_RepaintIntervalMs = 200;
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
