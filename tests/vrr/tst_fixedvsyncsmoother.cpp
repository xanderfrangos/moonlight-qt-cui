#include "../../app/streaming/video/ffmpeg-renderers/pacer/fixedvsyncsmoother.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

// Deterministic checks for Smooth V-Sync's host-timestamp scheduler. Frames
// and refreshes are replayed in time order, as the Pacer's decoder and V-sync
// threads would deliver them.
namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct Result {
    std::vector<FixedVsyncSmoother::Decision> decisions;
    std::vector<uint64_t> arrivals;
};

// captureUs(i) is the host capture time of frame i on the host clock,
// transitUs(i) the delay until it finished decoding on the client clock.
Result run(int streamFps, double hostHz, double displayHz, int frames,
           const std::function<double(int)>& stampJitterUs,
           const std::function<double(int)>& transitUs,
           uint32_t rtpBase = 1000)
{
    FixedVsyncSmoother smoother;
    smoother.configure(streamFps, static_cast<int>(displayHz + 0.5), 3000);

    Result result;
    const double hostPeriod = 1000000.0 / hostHz;
    const double displayPeriod = 1000000.0 / displayHz;
    double nextVsync = 1000000.0;
    for (int i = 0; i < frames; i++) {
        const double capture = 1000000.0 + i * hostPeriod;
        double arrival = capture + transitUs(i);
        if (!result.arrivals.empty() && arrival <= result.arrivals.back()) {
            arrival = result.arrivals.back() + 1;  // the decoder is in order
        }
        while (nextVsync <= arrival) {
            smoother.observeVsync(static_cast<uint64_t>(nextVsync));
            nextVsync += displayPeriod;
        }
        const uint32_t rtp = rtpBase + static_cast<uint32_t>(
            static_cast<int64_t>((capture - 1000000.0 + stampJitterUs(i)) * 0.09));
        result.decisions.push_back(smoother.admit(rtp, static_cast<uint64_t>(arrival)));
        result.arrivals.push_back(static_cast<uint64_t>(arrival));
    }
    return result;
}

// Refresh steps between consecutive slots, counted from frame `from`
std::vector<long> slotSteps(const Result& r, double displayHz, size_t from)
{
    std::vector<long> steps;
    const double period = 1000000.0 / displayHz;
    for (size_t i = from + 1; i < r.decisions.size(); i++) {
        const double delta = double(r.decisions[i].slotUs) - double(r.decisions[i - 1].slotUs);
        steps.push_back(std::lround(delta / period));
    }
    return steps;
}

size_t rephasesAfter(const Result& r, size_t from)
{
    size_t count = 0;
    for (size_t i = from; i < r.decisions.size(); i++) {
        count += r.decisions[i].rephased;
    }
    return count;
}

}

int main()
{
    const auto none = [](int) { return 0.0; };
    const auto steady = [](int) { return 5000.0; };

    {
        // Ideal 60 on 60: every frame on the next refresh, never before it
        // has arrived plus the render lead.
        const Result r = run(60, 60.0, 60.0, 600, none, steady);
        const auto steps = slotSteps(r, 60.0, 30);
        bool allOne = true;
        for (long s : steps) allOne = allOne && s == 1;
        expect(allOne, "ideal 60 on 60 assigns consecutive refreshes");
        expect(rephasesAfter(r, 30) == 0, "ideal cadence never re-aligns after warmup");
        bool ready = true;
        for (size_t i = 30; i < r.decisions.size(); i++) {
            ready = ready && r.decisions[i].dueUs >= r.arrivals[i] + 3000;
        }
        expect(ready, "due time leaves the render lead after arrival");
    }

    {
        // Host stamps jitter by +-2 ms around the true capture time. The
        // assigned refreshes must not flip between neighbours.
        const Result r = run(60, 60.0, 60.0, 1200,
                             [](int i) { return (i % 2 ? 2000.0 : -2000.0) + (i % 7) * 150.0; },
                             steady);
        const auto steps = slotSteps(r, 60.0, 30);
        size_t irregular = 0;
        for (long s : steps) irregular += s != 1;
        expect(irregular == 0, "host stamp jitter does not move frames between refreshes");
    }

    {
        // 60 FPS on 120 Hz: a steady 2-refresh rhythm, the case where showing
        // frames on arrival alternates 1 and 3 refreshes.
        const Result r = run(60, 60.0, 120.0, 1200,
                             [](int i) { return (i % 3) * 700.0; },
                             [](int i) { return 5000.0 + (i % 5) * 900.0; });
        const auto steps = slotSteps(r, 120.0, 30);
        size_t irregular = 0;
        for (long s : steps) irregular += s != 2;
        expect(irregular == 0, "60 FPS on 120 Hz keeps a 2-refresh rhythm");
    }

    {
        // 59.94 FPS host on a 60 Hz display drifts by one refresh every ~17 s.
        // The chain re-aligns rarely, not frame to frame.
        const Result r = run(60, 59.94, 60.0, 60 * 60, none, steady);
        const size_t rephases = rephasesAfter(r, 30);
        expect(rephases >= 1 && rephases <= 8, "clock drift re-aligns a few times per minute");
    }

    {
        // A burst of late frames grows the offset within the slew limit and
        // never past two source frames above the fastest arrival.
        const Result r = run(60, 60.0, 60.0, 600, none,
                             [](int i) { return (i >= 300 && i < 330) ? 25000.0 : 5000.0; });
        bool slew = true;
        bool bounded = true;
        // Offsets carry the host/client clock base, so compare against the
        // settled offset before the burst.
        const int64_t settled = r.decisions[299].offsetUs;
        for (size_t i = 31; i < r.decisions.size(); i++) {
            const int64_t change = r.decisions[i].offsetUs - r.decisions[i - 1].offsetUs;
            slew = slew && change <= FixedVsyncSmoother::kOffsetGrowUsPerFrame &&
                   change >= -FixedVsyncSmoother::kOffsetShrinkUsPerFrame;
            bounded = bounded && r.decisions[i].offsetUs <= settled + 2 * 16667 + 50;
        }
        expect(slew, "offset changes stay within the slew limits");
        expect(bounded, "offset stays within two source frames of the fastest arrival");
        expect(r.decisions[329].offsetUs > r.decisions[299].offsetUs + 3000,
               "offset grows during a burst of late frames");
    }

    {
        // A host stall (game paused for a second) resynchronizes the host
        // timeline instead of predicting through the gap.
        FixedVsyncSmoother smoother;
        smoother.configure(60, 60, 3000);
        uint64_t vsync = 1000000;
        for (uint64_t i = 0; i < 60; i++) {
            while (vsync <= 1000000 + i * 16667 + 5000) {
                smoother.observeVsync(vsync);
                vsync += 16667;
            }
            smoother.admit(static_cast<uint32_t>(1000 + i * 1500), 1000000 + i * 16667 + 5000);
        }
        const auto d = smoother.admit(1000 + 59 * 1500 + 90000, 1000000 + 59 * 16667 + 1000000 + 5000);
        expect(d.resynced, "a long host gap resynchronizes");
        expect(d.dueUs >= 1000000 + 59 * 16667 + 1000000 + 5000,
               "the first frame after a stall is not scheduled before it arrived");
    }

    {
        // RTP stamps wrap at 2^32 without disturbing the timeline
        const Result r = run(60, 60.0, 60.0, 300, none, steady, 0xFFFFFFFFu - 90000u);
        const auto steps = slotSteps(r, 60.0, 30);
        bool allOne = true;
        for (long s : steps) allOne = allOne && s == 1;
        expect(allOne, "RTP wraparound keeps consecutive refreshes");
    }

    {
        // A 90 FPS stream of a game capped near 60 (the Steam Deck captures of
        // 2026-09-28): host stamps alternate 15.9 / 18.2 ms. The measured
        // interval must follow the content, not the requested stream rate.
        FixedVsyncSmoother smoother;
        smoother.configure(90, 90, 3000);
        uint64_t vsync = 1000000;
        uint32_t rtp = 1000;
        uint64_t host = 0;
        std::vector<FixedVsyncSmoother::Decision> decisions;
        for (int i = 0; i < 600; i++) {
            host += (i % 2) ? 18200 : 15900;
            const uint64_t arrival = 1000000 + host + 5000 + (i % 5) * 700;
            while (vsync <= arrival) {
                smoother.observeVsync(vsync);
                vsync += 11111;
            }
            rtp = 1000 + static_cast<uint32_t>(host * 9 / 100);
            decisions.push_back(smoother.admit(rtp, arrival));
        }
        expect(smoother.hostPeriodUs() > 16500 && smoother.hostPeriodUs() < 17600,
               "host interval is measured from the content, not the requested rate");
        size_t shared = 0;
        size_t resyncs = 0;
        for (size_t i = 60; i < decisions.size(); i++) {
            shared += decisions[i].slotUs <= decisions[i - 1].slotUs;
            resyncs += decisions[i].resynced;
        }
        expect(shared == 0, "59 FPS content on 90 Hz never puts two frames on one refresh");
        expect(resyncs == 0, "bimodal host stamps do not resynchronize");
    }

    {
        // A capture burst: one frame stamped 7 ms after its predecessor in an
        // otherwise steady 60 FPS stream. It keeps its own refresh.
        FixedVsyncSmoother smoother;
        smoother.configure(60, 60, 3000);
        uint64_t vsync = 1000000;
        std::vector<FixedVsyncSmoother::Decision> decisions;
        for (int i = 0; i < 300; i++) {
            const uint64_t host = static_cast<uint64_t>(i) * 16667 - (i == 200 ? 9667 : 0);
            const uint64_t arrival = 1000000 + static_cast<uint64_t>(i) * 16667 + 5000;
            while (vsync <= arrival) {
                smoother.observeVsync(vsync);
                vsync += 16667;
            }
            decisions.push_back(smoother.admit(1000 + static_cast<uint32_t>(host * 9 / 100), arrival));
        }
        expect(!decisions[200].resynced, "a single early stamp does not resynchronize");
        expect(decisions[200].slotUs > decisions[199].slotUs &&
               decisions[201].slotUs > decisions[200].slotUs,
               "a burst frame keeps its own refresh");
    }

    if (failures == 0) {
        std::printf("tst_fixedvsyncsmoother: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
