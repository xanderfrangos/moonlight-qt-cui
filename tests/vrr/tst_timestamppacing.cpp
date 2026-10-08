#include "streaming/video/ffmpeg-renderers/pacer/timestamppacingpolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace TimestampPacing;

namespace {
int failures = 0;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

TimestampPacingOptions options(int smoothing, int targetPerMille = 990, int minMs = 2, int maxMs = 16)
{
    TimestampPacingOptions o;
    o.enabled = true;
    o.smoothing = smoothing;
    o.targetPerMille = targetPerMille;
    o.minBufferMs = minMs;
    o.maxBufferMs = maxMs;
    return o;
}

// A host stream: frame numbers, 90 kHz timestamps and the client time each
// frame is ready, with optional per-frame timestamp noise and arrival delay.
struct Stream {
    Policy policy;
    int32_t frame = 100;
    uint32_t rtp;
    double sourceUs = 0;
    uint64_t epochUs = 5000000000ULL;
    std::vector<Policy::Decision> decisions;

    Stream(const TimestampPacingOptions& o, int fps, uint32_t firstRtp = 12345) : rtp(firstRtp)
    {
        policy.configure(o, fps);
    }

    // Advances the source by periodUs and delivers the frame
    Policy::Decision next(double periodUs, double stampNoiseUs = 0, double arrivalDelayUs = 0,
                          uint32_t hostLatencyUs = 3000, uint64_t decoderQueueUs = 0)
    {
        sourceUs += periodUs;
        frame++;
        const uint32_t stamp = rtp + uint32_t(std::llround((sourceUs + stampNoiseUs) * 9 / 100));
        const uint64_t readyUs = epochUs + uint64_t(sourceUs) + 4000 + uint64_t(arrivalDelayUs);
        const auto decision = policy.schedule(true, frame, stamp, readyUs, hostLatencyUs, decoderQueueUs);
        decisions.push_back(decision);
        return decision;
    }
};

double percentile(std::vector<double> values, double p)
{
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(p / 100 * (values.size() - 1) + 0.5))];
}

// Absolute change between consecutive target intervals, over the last count
std::vector<double> targetJerk(const std::vector<Policy::Decision>& decisions, size_t count)
{
    std::vector<double> jerk;
    for (size_t i = decisions.size() - count + 2; i < decisions.size(); i++) {
        const double a = double(decisions[i - 1].targetUs) - double(decisions[i - 2].targetUs);
        const double b = double(decisions[i].targetUs) - double(decisions[i - 1].targetUs);
        jerk.push_back(std::fabs(b - a));
    }
    return jerk;
}

// Host timestamps on a steady cadence with one stamp in ten displaced 2 ms
// early, as measured from a real host. Delivery itself is steady.
void smoothingRemovesStampNoise()
{
    for (int smoothing = 0; smoothing <= 2; smoothing++) {
        Stream stream(options(smoothing), 110);
        for (int i = 0; i < 2000; i++) {
            stream.next(1000000.0 / 110, i % 10 == 5 ? -2000 : 0);
        }
        const double p99 = percentile(targetJerk(stream.decisions, 1000), 99);
        if (smoothing == 0) {
            check(p99 > 3000, "without smoothing, targets keep the timestamp noise");
        }
        else {
            check(p99 < 1200, "smoothing removes most of the timestamp noise");
        }

        // The graphs' inputs: the displaced stamp shows up as raw host jerk
        // of about 2 ms either side of it, and smoothing moves it back
        const auto& displaced = stream.decisions[1995];
        const auto& after = stream.decisions[1996];
        check(displaced.hostJerkValid && std::fabs(displaced.hostJerkUs - 2000) < 50,
              "a stamp 2 ms early changes the raw spacing by 2 ms");
        check(after.hostJerkValid && std::fabs(after.hostJerkUs - 4000) < 50,
              "and the compensating interval by 4 ms");
        if (smoothing == 0) {
            check(displaced.correctionUs == 0, "no smoothing never moves a frame");
        }
        else {
            check(displaced.correctionUs > 1000, "smoothing moves the early stamp most of the way back");
        }
    }
}

void bufferCoversArrivalJitter()
{
    std::mt19937 random(1);
    std::uniform_real_distribution<double> jitter(0, 5000);
    Stream stream(options(2), 60);
    for (int i = 0; i < 600; i++) {
        stream.next(1000000.0 / 60, 0, jitter(random));
    }
    const double bufferUs = stream.policy.bufferUs();
    check(bufferUs > 4500 && bufferUs < 6500, "the buffer settles near the 99th percentile plus margin");

    size_t onTime = 0;
    for (size_t i = 300; i < stream.decisions.size(); i++) {
        onTime += stream.decisions[i].latenessUs <= stream.decisions[i].bufferUs;
    }
    check(onTime * 100 >= (stream.decisions.size() - 300) * 98, "almost every frame is ready by its target");

    Stream wild(options(2, 990, 2, 16), 60);
    for (int i = 0; i < 600; i++) {
        wild.next(1000000.0 / 60, 0, i % 3 == 0 ? 40000 : 0);
    }
    check(wild.policy.bufferUs() == 16000, "the buffer stops at its maximum");

    Stream clean(options(2, 990, 3, 16), 60);
    for (int i = 0; i < 600; i++) {
        clean.next(1000000.0 / 60);
    }
    check(clean.policy.bufferUs() == 3000, "a clean stream holds the minimum buffer");
}

void bufferReleasesSlowly()
{
    Stream stream(options(2), 60);
    for (int i = 0; i < 300; i++) {
        stream.next(1000000.0 / 60, 0, i % 2 ? 8000 : 0);
    }
    const double grownUs = stream.policy.bufferUs();
    check(grownUs > 8000, "alternating late frames grow the buffer");
    // 1.5 s after the late frames leave the 3 s window
    for (int i = 0; i < 270; i++) {
        stream.next(1000000.0 / 60);
    }
    const double releasedUs = stream.policy.bufferUs();
    check(releasedUs < grownUs && releasedUs > grownUs - 1500,
          "the buffer releases at about half a millisecond per second");
}

// The window minimum steps when an early arrival leaves the window. Targets
// follow it at a bounded rate instead of jumping with it.
void baselineSlewsInsteadOfStepping()
{
    const double periodUs = 1000000.0 / 60;
    // A fixed 2 ms buffer, so only the baseline moves the targets
    Stream stream(options(2, 990, 2, 2), 60);
    for (int i = 0; i < 600; i++) {
        stream.next(periodUs, 0, i % 30 == 0 ? 0 : 3000);
    }
    const double beforeUs = stream.policy.baselineUs();
    for (int i = 0; i < 600; i++) {
        stream.next(periodUs, 0, 3000);
    }
    const auto jerk = targetJerk(stream.decisions, 600);
    check(*std::max_element(jerk.begin(), jerk.end()) < 100,
          "targets move by a fraction of a millisecond per frame when the minimum steps");
    check(std::fabs(stream.policy.baselineUs() - stream.policy.windowMinimumUs()) < 1,
          "the baseline reaches the new minimum");
    check(std::fabs(stream.policy.baselineUs() - beforeUs - 3000) < 50, "and moves by all of the step");

    // At the start, an earlier arrival is adopted at once
    Stream start(options(2), 60);
    start.next(periodUs, 0, 6000);
    for (int i = 0; i < 10; i++) {
        start.next(periodUs);
    }
    check(start.policy.baselineUs() == start.policy.windowMinimumUs(),
          "a new timeline's baseline isn't slewed");
}

// A delivery stall: 150 ms without frames, then the ones held up arrive
// together. Without leaving most of them out, this one stall would hold the
// buffer at its maximum for half a minute.
void deliveryStallsDontGrowBuffer()
{
    const double periodUs = 1000000.0 / 60;
    Stream stream(options(2), 60);
    for (int i = 0; i < 600; i++) {
        check(stream.next(periodUs).admitted || i == 0, "steady frames size the buffer");
    }
    for (int k = 0; k < 10; k++) {
        const auto decision = stream.next(periodUs, 0, 150000 - k * periodUs);
        check(decision.admitted == (decision.latenessUs <= 48000),
              "frames late by more than three maximum buffers don't size it");
    }
    for (int i = 0; i < 120; i++) {
        stream.next(periodUs);
    }
    check(stream.policy.bufferUs() == 2000, "a stall leaves the buffer at its minimum");
}

// A decoder that can't keep up: every frame waits longer than a period to
// enter it. The lateness that follows grows the buffer no matter how large
// it gets, so it is left out, and the buffer releases again.
void overloadDoesntGrowBuffer()
{
    const double periodUs = 1000000.0 / 60;
    for (const bool queueReported : { false, true }) {
        Stream stream(options(2), 60);
        for (int i = 0; i < 300; i++) {
            stream.next(periodUs);
        }
        for (int i = 0; i < 1200; i++) {
            stream.next(periodUs, 0, (i % 6) * 3000.0, 3000, queueReported ? 25000 : 0);
        }
        if (queueReported) {
            check(stream.policy.bufferUs() < 10000, "a sustained decoder backlog doesn't hold the buffer up");
        }
        else {
            check(stream.policy.bufferUs() > 15000, "the same lateness without a backlog does");
        }
    }
}

void repeatsAreShownOnArrival()
{
    Stream stream(options(2), 60);
    for (int i = 0; i < 60; i++) {
        stream.next(1000000.0 / 60);
    }
    const auto repeat = stream.next(1000000.0 / 60, -50000, 0, 0);
    check(repeat.repeat && !repeat.paced, "a zero host latency from a reporting host is a repeat");
    const auto real = stream.next(1000000.0 / 60);
    check(real.paced && !real.repeat, "the next real frame is paced again");

    Stream silent(options(2), 60);
    for (int i = 0; i < 60; i++) {
        const auto decision = silent.next(1000000.0 / 60, 0, 0, 0);
        check(decision.paced, "a host that never reports latency still has every frame paced");
    }
}

void followsRateChanges()
{
    Stream stream(options(2), 110);
    for (int i = 0; i < 300; i++) {
        stream.next(1000000.0 / 110);
    }
    for (int i = 0; i < 300; i++) {
        stream.next(1000000.0 / 60);
    }
    check(std::fabs(stream.policy.sourcePeriodUs() - 1000000.0 / 60) < 200,
          "the smoothed period follows a drop from 110 to 60 FPS");
    const double p99 = percentile(targetJerk(stream.decisions, 200), 99);
    check(p99 < 300, "the targets settle on the new cadence");
}

// A game that slows down for a moment and recovers: the smoother must pick
// the usual cadence straight back up rather than re-anchoring frame after
// frame until its loop catches up, which would show raw timestamp noise.
void recoversFromSlowdown()
{
    Stream stream(options(2), 110);
    for (int i = 0; i < 1500; i++) {
        stream.next(1000000.0 / 110);
    }
    for (int i = 0; i < 12; i++) {
        stream.next(25000);
    }
    const size_t back = stream.decisions.size();
    for (int i = 0; i < 300; i++) {
        stream.next(1000000.0 / 110, i % 10 == 5 ? -2000 : 0);
    }
    int reanchors = 0;
    for (size_t i = back + 1; i < stream.decisions.size(); i++) {
        reanchors += stream.decisions[i].reanchored;
    }
    check(reanchors == 0, "the usual cadence is picked straight back up after a slowdown");
    check(std::fabs(stream.policy.sourcePeriodUs() - 1000000.0 / 110) < 100,
          "a brief slowdown leaves the period where it was");
}

void survivesWrapAndRestart()
{
    Stream wrap(options(2), 60, UINT32_MAX - 3000);
    for (int i = 0; i < 300; i++) {
        wrap.next(1000000.0 / 60);
    }
    check(percentile(targetJerk(wrap.decisions, 280), 100) < 50, "RTP wrap continues the timeline");

    Stream backwards(options(2), 60);
    for (int i = 0; i < 100; i++) {
        backwards.next(1000000.0 / 60);
    }
    backwards.rtp -= 90000;
    check(backwards.next(1000000.0 / 60).restarted, "a backwards timestamp restarts the timeline");
}

void vblankGridFollowsWakeups()
{
    std::mt19937 random(2);
    std::uniform_real_distribution<double> late(0, 400);
    VblankGrid grid;
    grid.configure(1000000.0 / 120);
    // The display really runs at 119.88 Hz
    const double periodUs = 1000000.0 / 119.88;
    const double originUs = 1000000;
    for (int i = 0; i < 600; i++) {
        grid.observe(uint64_t(originUs + i * periodUs + late(random)));
    }
    const uint64_t nowUs = uint64_t(originUs + 599 * periodUs + 500);
    check(grid.valid(nowUs), "the grid is valid after enough wakeups");
    check(std::fabs(grid.periodUs() - periodUs) < periodUs * 0.002, "the grid measures the real refresh period");
    const double nextUs = originUs + 600 * periodUs;
    const double predictedUs = double(grid.atOrAfter(nextUs - 100));
    check(std::fabs(predictedUs - nextUs) < 400, "the next V-blank is predicted from the earliest wakeups");
    check(!grid.valid(nowUs + VblankGrid::StaleUs + 1), "a grid without recent wakeups is stale");

    // Reported display times can repeat or arrive late; neither restarts it
    VblankGrid events;
    events.configure(1000000.0 / 60);
    for (int i = 0; i < 20; i++) {
        const uint64_t atUs = uint64_t(1000000 + i * (1000000.0 / 60));
        events.observe(atUs);
        events.observe(atUs);
        if (i > 0) {
            events.observe(atUs - 16667);
        }
    }
    check(events.valid(uint64_t(1000000 + 19 * (1000000.0 / 60))), "repeated and late display times keep the grid");
    check(std::fabs(double(events.atOrAfter(1000000 + 20 * (1000000.0 / 60) - 100)) -
                    (1000000 + 20 * (1000000.0 / 60))) < 50,
          "and its prediction");
}

// A 59.94 FPS source on a 60 Hz display drifts across one V-blank boundary
// every 16.7 s. With noisy targets, the lock must spend each crossing as one
// repeat, not as alternating repeats and skips.
void phaseLockAvoidsBoundaryFlicker()
{
    std::mt19937 random(3);
    std::normal_distribution<double> noise(0, 300);
    VblankGrid grid;
    const double refreshUs = 1000000.0 / 60;
    grid.configure(refreshUs);
    for (int i = 0; i < 20; i++) {
        grid.observe(uint64_t(1000000 + i * refreshUs));
    }

    PhaseLock lock;
    const double sourceUs = 1000000.0 / 59.94;
    uint64_t lastVblank = 0;
    int irregular = 0, adjacentIrregular = 0, lastIrregularAt = -100;
    double waitSumUs = 0, earliestUs = 0;
    for (int i = 0; i < 3600; i++) {
        const uint64_t targetUs = uint64_t(1000000 + 8000 + i * sourceUs + noise(random));
        const uint64_t vblankUs = lock.assign(targetUs, grid);
        lock.update(targetUs, vblankUs, grid.periodUs(), sourceUs);
        if (i >= 100) {
            const double waitUs = double(vblankUs) - double(targetUs);
            waitSumUs += waitUs;
            earliestUs = std::min(earliestUs, waitUs);
        }
        if (lastVblank != 0 && std::llabs(int64_t(vblankUs - lastVblank) - int64_t(refreshUs)) > 1000) {
            irregular++;
            if (i - lastIrregularAt < 30) {
                adjacentIrregular++;
            }
            lastIrregularAt = i;
        }
        lastVblank = vblankUs;
    }
    // One minute of drift is 3.6 crossings
    check(irregular >= 2 && irregular <= 8, "drift costs one repeat per boundary crossing");
    check(adjacentIrregular == 0, "no crossing alternates repeats and skips");
    // Noise can put a target a little past its V-blank; the lock itself
    // never shows a frame early and adds no latency on average
    check(earliestUs > -1500, "a locked target is not shown before its time");
    const double averageWaitUs = waitSumUs / 3500;
    check(averageWaitUs > refreshUs * 0.35 && averageWaitUs < refreshUs * 0.65,
          "on average a frame waits half a refresh for its V-blank");
}
}

// Only frames later than the usual lag count as missed: a compositor that
// always adds a refresh costs latency, not smoothness
void missesAreJudgedAgainstUsualLag()
{
    const double periodUs = 1000000.0 / 120;
    MissDetector composed;
    int misses = 0;
    for (int i = 0; i < 600; i++) {
        misses += composed.observe(periodUs + 300, periodUs);
    }
    check(misses == 0, "a constant extra refresh of lag is not a miss");

    MissDetector direct;
    misses = 0;
    for (int i = 0; i < 600; i++) {
        misses += direct.observe(i % 20 == 10 ? periodUs + 200 : 200, periodUs);
    }
    check(misses == 30, "a frame a refresh later than the rest is a miss");

    // After a lasting change, the new lag becomes the usual one
    MissDetector shifted;
    for (int i = 0; i < 200; i++) {
        shifted.observe(0, periodUs);
    }
    misses = 0;
    for (int i = 0; i < 400; i++) {
        misses += shifted.observe(periodUs, periodUs);
    }
    check(misses == int(MissDetector::Window) - 1, "a lasting extra refresh stops counting once it fills the window");
}

int main()
{
    smoothingRemovesStampNoise();
    bufferCoversArrivalJitter();
    bufferReleasesSlowly();
    baselineSlewsInsteadOfStepping();
    deliveryStallsDontGrowBuffer();
    overloadDoesntGrowBuffer();
    repeatsAreShownOnArrival();
    followsRateChanges();
    recoversFromSlowdown();
    survivesWrapAndRestart();
    vblankGridFollowsWakeups();
    phaseLockAvoidsBoundaryFlicker();
    missesAreJudgedAgainstUsualLag();

    if (failures == 0) {
        std::printf("tst_timestamppacing: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
