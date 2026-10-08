#include "../../app/streaming/video/presentationlateness.h"

#include <cstdio>
#include <cstdint>

namespace {
int failures = 0;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

// A 60 FPS host stream presented on a client clock with an arbitrary epoch.
// extraUs delays one presentation without moving the host timeline.
struct Stream {
    PresentationLateness lateness;
    // The host's timestamp, which a test can jump without moving the
    // presentation clock
    uint32_t timestamp;
    uint64_t elapsedTicks = 0;
    uint64_t epochUs = 5000000000ULL;

    explicit Stream(uint32_t firstTimestamp = 12345) : timestamp(firstTimestamp) {}

    uint64_t present(uint64_t extraUs = 0)
    {
        const uint64_t latenessUs =
            lateness.observe(timestamp, epochUs + elapsedTicks * 100 / 9 + extraUs);
        timestamp += 1500;
        elapsedTicks += 1500;
        return latenessUs;
    }
};

bool near(uint64_t actualUs, uint64_t expectedUs)
{
    // Converting 90 kHz ticks to whole microseconds rounds by under 1 us
    return actualUs + 2 >= expectedUs && actualUs <= expectedUs + 2;
}
}

int main()
{
    Stream steady;
    bool allZero = true;
    for (int i = 0; i < 600; ++i) { allZero = allZero && near(steady.present(), 0); }
    check(allZero, "presentation at host cadence is never late");

    Stream jitter;
    for (int i = 0; i < 60; ++i) { jitter.present(); }
    check(near(jitter.present(5000), 5000), "a delayed frame is late by its delay");
    check(near(jitter.present(), 0), "the following on-time frame is not late");
    check(near(jitter.present(2000), 2000), "a later delayed frame is measured from the same baseline");

    // Every frame delayed equally is the pipeline's fixed latency, not lateness,
    // once the faster frames leave the window.
    Stream slower;
    for (int i = 0; i < 60; ++i) { slower.present(); }
    check(near(slower.present(4000), 4000), "a slower pipeline first reads as late");
    uint64_t lastUs = 0;
    for (int i = 0; i < 240; ++i) { lastUs = slower.present(4000); }
    check(near(lastUs, 0), "the baseline follows a lasting change within the window");

    Stream wrap(UINT32_MAX - 30000);
    allZero = true;
    for (int i = 0; i < 120; ++i) { allZero = allZero && near(wrap.present(), 0); }
    check(allZero, "RTP wrap continues the timeline");
    for (int i = 0; i < 10; ++i) { wrap.present(); }
    check(near(wrap.present(3000), 3000), "lateness is measured across the wrap");

    Stream jump;
    for (int i = 0; i < 60; ++i) { jump.present(); }
    jump.timestamp += 90000 * 600;
    check(near(jump.present(), 0), "a host timeline jump is a new baseline, not a late frame");
    check(near(jump.present(1000), 1000), "the new timeline measures lateness again");

    Stream backwards;
    for (int i = 0; i < 60; ++i) { backwards.present(); }
    backwards.timestamp -= 1500 * 30;
    check(near(backwards.present(), 0), "a backwards timestamp restarts the timeline");
    check(near(backwards.present(1500), 1500), "the restarted timeline measures lateness");

    // The client clock runs 100 ppm fast against the host's: 1.7 us per frame
    Stream drift;
    uint64_t worstUs = 0;
    for (int i = 0; i < 3600; ++i) {
        const uint64_t latenessUs = drift.present(uint64_t(i) * 17 / 10);
        if (latenessUs > worstUs) { worstUs = latenessUs; }
    }
    check(worstUs <= 400, "clock drift stays within the window's share of it");

    if (failures == 0) {
        std::printf("tst_presentationlateness: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
