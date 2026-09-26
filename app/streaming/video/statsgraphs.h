#pragma once

#include <atomic>
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

// How presentation is synchronized to the display. Not "None", which X11
// headers define as a macro.
enum class StatsGraphSyncMode {
    Off,
    VSync,
    Vrr,
};

// What is being streamed, for the summary the graph card shows in place of the
// text overlay when that is hidden. Plain values only, so it can ride along in
// the counters without allocating under their lock.
struct StatsGraphStreamInfo {
    // Decoded size, which can change mid-stream. Zero until the first frame.
    int width = 0;
    int height = 0;
    // Negotiated frame rate, not the measured one
    int frameRate = 0;
    // A VIDEO_FORMAT_* value
    int videoFormat = 0;
    // Output bits per color component when the active renderer can report it.
    // Zero means the renderer doesn't expose the output depth.
    int outputBitsPerComponent = 0;
    // The host can switch in and out of HDR mid-stream
    bool hdr = false;
    // Static strings from IFFmpegRenderer::getRendererName(). The backend is
    // null unless it differs from the frontend.
    const char* renderer = nullptr;
    const char* backendRenderer = nullptr;
    // Static short name of the upscaler in use, or null
    const char* upscaler = nullptr;
    // What the pacer actually runs, which is fixed V-sync when VRR was
    // requested but isn't available
    StatsGraphSyncMode syncMode = StatsGraphSyncMode::Off;
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
    // Rolling scores in percent, matching the text overlay's. Each is invalid
    // until its window has enough frames, and the VRR one outside VRR.
    float incomingSmoothness = 0;
    bool incomingSmoothnessValid = false;
    float vrrSmoothness = 0;
    bool vrrSmoothnessValid = false;

    // Per-frame values, taken and reset on every sample. All in milliseconds.
    StatsGraphAccumulator incomingFrametime;
    StatsGraphAccumulator renderingFrametime;
    StatsGraphAccumulator hostProcessingLatency;
    StatsGraphAccumulator reassembly;
    // Interval between frames leaving the decoder
    StatsGraphAccumulator decodingFrametime;
    StatsGraphAccumulator decodingTime;
    StatsGraphAccumulator renderingTime;

    StatsGraphStreamInfo streamInfo;
};

// Which graphs to draw and how big, fixed for the session.
struct StatsGraphConfig {
    // Bit n shows the graph with StreamingPreferences::PerformanceGraph ID n
    uint32_t visibleGraphs = 0;
    // Scale in percent, or 0 to follow the stream window's height
    int sizePercent = 100;
    // Height of each plot at 100% scale, in pixels
    int plotHeight = 40;
    // Opacity of the card behind the graphs, in percent
    int backgroundOpacity = 75;
    // Length of the plotted history
    int windowSeconds = 10;
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
    float decodingFrametimeMs = 0;
    float decodingFrametimeMinMs = 0;
    float decodingFrametimeMaxMs = 0;
    float decodingTimeMs = 0;
    float decodingTimeMinMs = 0;
    float decodingTimeMaxMs = 0;
    float renderingTimeMs = 0;
    float renderingTimeMinMs = 0;
    float renderingTimeMaxMs = 0;
    float videoMbps = 0;
    // Everything the video stream puts on the network: payload, FEC parity,
    // shard padding and packet headers
    float networkMbps = 0;
    float networkDroppedFrames = 0;
    float jitterDroppedFrames = 0;
    float networkLatencyMs = 0;
    float networkJitterMs = 0;
    float queueDepth = 0;
    float incomingSmoothness = 0;
    float vrrSmoothness = 0;
};

// Samples stream statistics on a fixed interval and publishes a painted plot of
// the last few seconds to the debug graph overlay. While the debug text overlay
// is hidden, the card also summarizes the stream it would otherwise describe.
// Sampling runs whether or not the overlay is visible, so the graphs already
// cover a full window when the user brings them up. Painting only happens
// while they're on screen.
class StatsGraphs
{
public:
    static constexpr int k_SampleIntervalMs = 100;
    // Repainting is deliberately slower than sampling. Every sample still
    // reaches the plot; only the refresh rate differs. A republish costs a
    // megabyte-scale surface allocation and a GPU texture rebuild in the
    // renderer, while the render thread contends for the same overlay state
    // lock, so doing it ten times a second is far more pressure than any
    // other overlay in this app applies.
    static constexpr int k_RepaintIntervalMs = 200;

    StatsGraphs() = default;
    ~StatsGraphs();

    // The sampler is invoked on the sampling thread once per interval and must
    // fill in the counters it owns. stop() joins that thread, so it has to be
    // called before anything the sampler touches is torn down.
    void start(OverlayManager* overlayManager,
               const StatsGraphConfig& config,
               std::function<void(StatsGraphCounters&)> sampler);
    void stop();

    // The stream window's size in pixels. An automatic size scales against
    // its height, and every size shrinks to fit it. Safe to call from any
    // thread.
    void setViewportSize(int width, int height);

private:
    void run();
    float scale() const;
    void appendSample(const StatsGraphCounters& counters, double intervalSecs);

    // Owned exclusively by the sampling thread
    std::vector<StatsGraphPoint> m_Points;
    StatsGraphCounters m_LastCounters;
    bool m_HaveLastCounters = false;

    OverlayManager* m_OverlayManager = nullptr;
    StatsGraphConfig m_Config;
    // Samples in a full window, from the configured history length
    int m_MaxSamples = 0;
    std::atomic<int> m_ViewportWidth{0};
    std::atomic<int> m_ViewportHeight{0};
    std::function<void(StatsGraphCounters&)> m_Sampler;
    std::mutex m_Lock;
    std::condition_variable m_Wake;
    bool m_Stopping = false;
    std::thread m_Thread;
};

}
