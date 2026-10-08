#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "settings/timestamppacingoptions.h"

// The timing policy of the timestamp pacer: when each frame should be shown,
// from the host's RTP timestamps. Plain values in, plain values out, so it can
// be tested and replayed without a renderer. All times are microseconds on
// the client's monotonic clock unless they are named as source times.
//
// docs/timestamp-pacing-proposal.md has the reasoning and the measurements
// behind each of these pieces.
namespace TimestampPacing {

// Hosts that report capture-to-send latency report zero for a frame they
// re-send without a new capture. Those repeats are stamped about one timeout
// before they are sent, so following their timestamps would hold the stream
// back by that much. A host that never reports the latency gives no way to
// tell, so zero is only trusted after the host has shown it reports one.
//
// Once shown, it is trusted for the rest of the session. A static screen
// sends nothing but repeats, for as long as it stays static; forgetting the
// host reports latency after a while let those repeats in as real frames,
// and their backdated timestamps dragged the baseline tens of milliseconds
// late by the time the picture moved again.
class RepeatDetector
{
public:
    static constexpr uint32_t ReportsNeeded = 16;

    bool isRepeat(uint32_t hostLatencyUs)
    {
        if (hostLatencyUs != 0) {
            if (m_Reports < ReportsNeeded) {
                m_Reports++;
            }
            return false;
        }
        return m_Reports >= ReportsNeeded;
    }

private:
    uint32_t m_Reports = 0;
};

// The host's timeline, smoothed by a phase-locked loop. Individual host
// timestamps can sit a couple of milliseconds off a steady cadence even when
// the frames are captured and sent steadily, and showing frames at those
// times is visibly uneven. The loop follows the cadence and attenuates the
// per-frame noise; a large departure (a stall, a change in frame rate) snaps
// back to the raw timestamp. The period is only replaced outright when the
// latest frames agree on a clearly different rate, and a long-run period is
// kept beside it, so that frames returning to the usual rate after a slowdown
// pick it straight back up instead of re-anchoring until the loop catches up.
class Timeline
{
public:
    struct Sample {
        // Smoothed source time, in microseconds on the host's timeline
        double sourceUs = 0;
        // The timeline started over, so nothing learned against the old one
        // applies
        bool restarted = false;
        // The loop jumped back to the raw timestamp
        bool reanchored = false;
        // Smoothed minus raw source time: how far smoothing moved this frame
        double correctionUs = 0;
        // Change in the raw spacing of the host's timestamps from the
        // previous pair, when this frame and the two before it are
        // consecutive host frames
        double hostJerkUs = 0;
        bool hostJerkValid = false;
    };

    void configure(int smoothing, double nominalPeriodUs)
    {
        switch (smoothing) {
        case TimestampPacingOptions::SmoothingOff:
            m_PhaseGain = 1.0;
            m_FrequencyGain = 0.0;
            break;
        case TimestampPacingOptions::SmoothingLight:
            m_PhaseGain = 0.25;
            m_FrequencyGain = 0.01;
            break;
        default:
            m_PhaseGain = 0.1;
            m_FrequencyGain = 0.002;
            break;
        }
        m_NominalPeriodUs = nominalPeriodUs;
        m_PeriodUs = nominalPeriodUs;
        m_SteadyPeriodUs = nominalPeriodUs;
        m_Have = false;
    }

    Sample observe(int32_t frameNumber, uint32_t rtpTimestamp)
    {
        Sample sample;
        const int64_t frames = int64_t(frameNumber) - m_LastFrame;
        const int32_t ticks = int32_t(rtpTimestamp - m_LastRtp);
        // Host frame numbers count every frame sent, so a gap in them is a
        // gap in the cadence rather than a slower one. Anything that runs
        // backwards, or skips more than ten seconds, is a new timeline.
        if (!m_Have || frames <= 0 || frames > 1000 || ticks <= 0 || ticks > 900000) {
            m_Have = true;
            m_RawUs = 0;
            m_SmoothedUs = 0;
            m_GapCount = 0;
            m_LastSingleGapUs = 0;
            m_LastFrame = frameNumber;
            m_LastRtp = rtpTimestamp;
            sample.restarted = true;
            return sample;
        }

        // RTP video timestamps are 90 kHz
        const double gapUs = ticks * 100.0 / 9.0;
        m_RawUs += gapUs;
        m_LastFrame = frameNumber;
        m_LastRtp = rtpTimestamp;
        if (frames == 1) {
            m_Gaps[m_NextGap] = gapUs;
            m_NextGap = (m_NextGap + 1) % m_Gaps.size();
            m_GapCount = std::min(m_GapCount + 1, m_Gaps.size());
            if (m_LastSingleGapUs != 0) {
                sample.hostJerkUs = std::fabs(gapUs - m_LastSingleGapUs);
                sample.hostJerkValid = true;
            }
            m_LastSingleGapUs = gapUs;
        }
        else {
            m_LastSingleGapUs = 0;
        }

        if (m_PhaseGain >= 1.0) {
            m_SmoothedUs = m_RawUs;
            double newPeriodUs;
            if (recentRate(newPeriodUs)) {
                m_PeriodUs = clampPeriod(newPeriodUs);
            }
        }
        else {
            const double predictedUs = m_SmoothedUs + frames * m_PeriodUs;
            const double errorUs = m_RawUs - predictedUs;
            if (std::fabs(errorUs) > std::max(8000.0, m_PeriodUs / 2)) {
                m_SmoothedUs = m_RawUs;
                double newPeriodUs;
                if (recentRate(newPeriodUs) && std::fabs(newPeriodUs - m_PeriodUs) > m_PeriodUs * 0.15) {
                    m_PeriodUs = clampPeriod(newPeriodUs);
                }
                else if (frames == 1 && std::fabs(gapUs - m_SteadyPeriodUs) < m_SteadyPeriodUs * 0.1) {
                    m_PeriodUs = m_SteadyPeriodUs;
                }
                sample.reanchored = true;
            }
            else {
                m_SmoothedUs = predictedUs + m_PhaseGain * errorUs;
                m_PeriodUs = clampPeriod(m_PeriodUs + m_FrequencyGain * errorUs / frames);
                m_SteadyPeriodUs += SteadyGain * (m_PeriodUs - m_SteadyPeriodUs);
            }
        }

        sample.sourceUs = m_SmoothedUs;
        sample.correctionUs = m_SmoothedUs - m_RawUs;
        return sample;
    }

    double periodUs() const { return m_PeriodUs; }

    // Starts a new timeline at the next frame, at this period. For frames
    // that were followed but shouldn't have been, so that neither their
    // phase nor their rate carries over.
    void restart(double periodUs)
    {
        m_Have = false;
        m_PeriodUs = clampPeriod(periodUs);
        m_SteadyPeriodUs = m_PeriodUs;
    }

private:
    double clampPeriod(double periodUs) const
    {
        return std::max(m_NominalPeriodUs / 4, std::min(m_NominalPeriodUs * 4, periodUs));
    }

    // The rate of the latest single-frame gaps, when every one of them
    // agrees with their median to within 10%
    bool recentRate(double& periodUs) const
    {
        if (m_GapCount < m_Gaps.size()) {
            return false;
        }
        std::array<double, GapCount> sorted = m_Gaps;
        std::sort(sorted.begin(), sorted.end());
        periodUs = sorted[GapCount / 2];
        for (double gapUs : m_Gaps) {
            if (std::fabs(gapUs - periodUs) > periodUs * 0.1) {
                return false;
            }
        }
        return true;
    }

    double m_PhaseGain = 0.1;
    double m_FrequencyGain = 0.002;
    double m_NominalPeriodUs = 16667;
    double m_PeriodUs = 16667;
    // Follows the period over several seconds, through brief slowdowns
    double m_SteadyPeriodUs = 16667;
    static constexpr double SteadyGain = 0.002;
    double m_RawUs = 0;
    double m_SmoothedUs = 0;
    double m_LastSingleGapUs = 0;
    // Enough to tell a lasting change in frame rate from a brief slowdown
    static constexpr size_t GapCount = 9;
    std::array<double, GapCount> m_Gaps {};
    size_t m_GapCount = 0;
    size_t m_NextGap = 0;
    int64_t m_LastFrame = 0;
    uint32_t m_LastRtp = 0;
    bool m_Have = false;
};

// The delay between a frame's smoothed source time and when it is shown.
// The offset from source time to readiness also contains the unknown
// host/client clock relationship and the pipeline's fixed latency, so it is
// measured from the smallest offset of the last few seconds, which also
// follows clock drift. The delay covers the chosen share of recent lateness
// above that, grows at once when that share rises, and releases slowly.
//
// The smallest offset steps whenever an old minimum leaves the window, and
// every following target would step with it. Targets instead follow a
// baseline that moves toward it at a bounded rate, as VRR Pacing Mode's
// playout offset does: fast next to clock drift, too slow to see.
class PlayoutBuffer
{
public:
    static constexpr uint64_t WindowUs = 3000000;
    static constexpr uint64_t BucketUs = 250000;
    static constexpr size_t Buckets = WindowUs / BucketUs;
    static constexpr size_t HistoryCapacity = 1024;
    static constexpr double MarginUs = 500;
    // Release of 0.5 ms per second
    static constexpr double ReleasePerUs = 0.0005;
    // 2.4 ms per second, and at most 0.1 ms between two frames
    static constexpr double BaselineSlewPerUs = 0.0024;
    static constexpr double BaselineMaximumStepUs = 100;
    // Until this many frames have been seen on a timeline, an earlier
    // arrival is adopted at once, so startup isn't paid for at the slew rate
    static constexpr uint32_t BaselineWarmupFrames = 64;

    void configure(int targetPerMille, uint64_t minimumUs, uint64_t maximumUs)
    {
        m_TargetPerMille = targetPerMille;
        m_MinimumUs = double(minimumUs);
        m_MaximumUs = double(std::max(minimumUs, maximumUs));
        m_DelayUs = m_MinimumUs;
        m_LastUpdateUs = 0;
        resetHistory();
    }

    // Keeps the delay, which is still the best guess for a new timeline
    void resetHistory()
    {
        m_Slots = {};
        m_HistoryCount = 0;
        m_NextHistory = 0;
        m_BaselineUs = 0;
        m_AppliedBaselineUs = 0;
        m_BaselineFrames = 0;
        m_LastSampleUs = 0;
    }

    // Records one frame's offset from source time to readiness and returns
    // how late it was against the recent baseline
    uint64_t addSample(uint64_t readyUs, double offsetUs)
    {
        const uint64_t bucket = readyUs / BucketUs;
        Slot& slot = m_Slots[bucket % Buckets];
        if (!slot.used || slot.bucket != bucket) {
            slot = { bucket, offsetUs, true };
        }
        else if (offsetUs < slot.minimumUs) {
            slot.minimumUs = offsetUs;
        }

        m_BaselineUs = offsetUs;
        for (const Slot& other : m_Slots) {
            if (other.used && other.bucket + Buckets > bucket && other.minimumUs < m_BaselineUs) {
                m_BaselineUs = other.minimumUs;
            }
        }

        if (m_BaselineFrames == 0) {
            m_AppliedBaselineUs = m_BaselineUs;
        }
        else if (m_BaselineFrames < BaselineWarmupFrames) {
            m_AppliedBaselineUs = std::min(m_AppliedBaselineUs, m_BaselineUs);
        }
        else if (readyUs > m_LastSampleUs) {
            // A pause buys one bounded step, not a jump
            const double stepUs = std::min(BaselineMaximumStepUs, (readyUs - m_LastSampleUs) * BaselineSlewPerUs);
            m_AppliedBaselineUs += std::max(-stepUs, std::min(stepUs, m_BaselineUs - m_AppliedBaselineUs));
        }
        if (m_BaselineFrames < BaselineWarmupFrames) {
            m_BaselineFrames++;
        }
        m_LastSampleUs = std::max(m_LastSampleUs, readyUs);

        // Measured against the true minimum, so a baseline still on its way
        // to it doesn't count as lateness
        return uint64_t(std::llround(offsetUs - m_BaselineUs));
    }

    // Counts a frame's lateness toward the delay. See Policy::schedule for
    // the frames that aren't.
    void admit(uint64_t readyUs, uint64_t latenessUs)
    {
        m_History[m_NextHistory] = { readyUs, (uint32_t)std::min<uint64_t>(latenessUs, UINT32_MAX) };
        m_NextHistory = (m_NextHistory + 1) % HistoryCapacity;
        m_HistoryCount = std::min(m_HistoryCount + 1, HistoryCapacity);
    }

    // Moves the delay toward the configured share of recent lateness. Called
    // after the frame's target is chosen, so that target uses the delay in
    // force before it arrived.
    void updateDelay(uint64_t nowUs)
    {
        size_t count = 0;
        for (size_t i = 0; i < m_HistoryCount; i++) {
            const Entry& entry = m_History[i];
            if (entry.atUs + WindowUs > nowUs) {
                m_Scratch[count++] = entry.latenessUs;
            }
        }

        // With no admitted frames in the window there is no evidence for
        // holding a delay, so it releases toward the minimum
        double wantUs = m_MinimumUs;
        if (count != 0) {
            const size_t rank = std::min(count - 1, (size_t(m_TargetPerMille) * (count - 1) + 500) / 1000);
            std::nth_element(m_Scratch.begin(), m_Scratch.begin() + rank, m_Scratch.begin() + count);
            wantUs = std::max(m_MinimumUs, std::min(m_MaximumUs, m_Scratch[rank] + MarginUs));
        }
        if (wantUs > m_DelayUs) {
            m_DelayUs = wantUs;
        }
        else if (m_LastUpdateUs != 0 && nowUs > m_LastUpdateUs) {
            m_DelayUs = std::max(wantUs, m_DelayUs - (nowUs - m_LastUpdateUs) * ReleasePerUs);
        }
        m_LastUpdateUs = nowUs;
    }

    // The baseline targets are placed from
    double baselineUs() const { return m_AppliedBaselineUs; }
    // The smallest offset in the window, which that baseline follows
    double windowMinimumUs() const { return m_BaselineUs; }
    double delayUs() const { return m_DelayUs; }

private:
    struct Slot {
        uint64_t bucket = 0;
        double minimumUs = 0;
        bool used = false;
    };
    struct Entry {
        uint64_t atUs = 0;
        uint32_t latenessUs = 0;
    };

    std::array<Slot, Buckets> m_Slots {};
    std::array<Entry, HistoryCapacity> m_History {};
    std::array<uint32_t, HistoryCapacity> m_Scratch {};
    size_t m_HistoryCount = 0;
    size_t m_NextHistory = 0;
    int m_TargetPerMille = 990;
    double m_MinimumUs = 2000;
    double m_MaximumUs = 16000;
    double m_DelayUs = 2000;
    double m_BaselineUs = 0;
    double m_AppliedBaselineUs = 0;
    uint32_t m_BaselineFrames = 0;
    uint64_t m_LastSampleUs = 0;
    uint64_t m_LastUpdateUs = 0;
};

// Whether the client keeps up with the stream: the mean time frames waited
// to enter the decoder over the last second, against the source period. A
// decoder slower than the stream makes every frame later than the last, and
// a deeper buffer would only add that to the standing delay. Averaged over a
// second, as VRR Pacing Mode's service gate is, so one slow frame doesn't
// count as overload.
class ServiceWindow
{
public:
    static constexpr uint64_t BucketUs = 250000;
    static constexpr size_t Buckets = 4;

    void reset() { m_Slots = {}; }

    void add(uint64_t atUs, uint64_t queueUs)
    {
        const uint64_t bucket = atUs / BucketUs;
        Slot& slot = m_Slots[bucket % Buckets];
        if (slot.count == 0 || slot.bucket != bucket) {
            slot = { bucket, 0, 0 };
        }
        slot.sumUs += queueUs;
        slot.count++;
    }

    bool keepsUp(uint64_t nowUs, double periodUs) const
    {
        const uint64_t bucket = nowUs / BucketUs;
        uint64_t sumUs = 0, count = 0;
        for (const Slot& slot : m_Slots) {
            if (slot.count != 0 && slot.bucket + Buckets > bucket) {
                sumUs += slot.sumUs;
                count += slot.count;
            }
        }
        return count == 0 || double(sumUs) <= periodUs * count;
    }

private:
    struct Slot {
        uint64_t bucket = 0;
        uint64_t sumUs = 0;
        uint32_t count = 0;
    };
    std::array<Slot, Buckets> m_Slots {};
};

// The display's V-blank times, estimated from V-sync source wakeups. A wakeup
// can be late but never early, so the earliest recent evidence sets the
// phase, while the period follows the observed spacing within a few percent
// of the nominal refresh rate.
class VblankGrid
{
public:
    static constexpr uint64_t StaleUs = 250000;
    static constexpr uint32_t ObservationsNeeded = 8;

    void configure(double nominalPeriodUs)
    {
        m_NominalPeriodUs = nominalPeriodUs;
        m_PeriodUs = nominalPeriodUs;
        m_Observations = 0;
        m_Outliers = 0;
    }

    void observe(uint64_t atUs)
    {
        // Reported display times can repeat or arrive slightly out of order.
        // Only a large step back is a new clock or display.
        if (m_Observations != 0 && atUs <= m_LastUs && m_LastUs - atUs < 1000000) {
            return;
        }
        if (m_Observations == 0 || atUs <= m_LastUs) {
            m_AnchorUs = double(atUs);
            m_LastUs = atUs;
            m_Observations = 1;
            return;
        }

        const double diffUs = double(atUs - m_LastUs);
        const double vblanks = std::max(1.0, std::round(diffUs / m_PeriodUs));
        if (vblanks <= 4) {
            m_PeriodUs += 0.02 * (diffUs / vblanks - m_PeriodUs);
            m_PeriodUs = std::max(m_NominalPeriodUs * 0.97, std::min(m_NominalPeriodUs * 1.03, m_PeriodUs));
        }
        m_LastUs = atUs;
        if (m_Observations < ObservationsNeeded) {
            m_Observations++;
        }

        const double predictedUs = m_AnchorUs + std::round((atUs - m_AnchorUs) / m_PeriodUs) * m_PeriodUs;
        const double errorUs = atUs - predictedUs;
        if (errorUs < 0) {
            m_AnchorUs = double(atUs);
            m_Outliers = 0;
        }
        else if (errorUs > m_PeriodUs / 3) {
            // A run of late wakeups at a consistent phase is a real shift
            if (++m_Outliers >= 4) {
                m_AnchorUs = double(atUs);
                m_Outliers = 0;
            }
        }
        else {
            m_AnchorUs = predictedUs + 0.05 * errorUs;
            m_Outliers = 0;
        }
    }

    bool valid(uint64_t nowUs) const
    {
        return m_Observations >= ObservationsNeeded && nowUs < m_LastUs + StaleUs;
    }

    double periodUs() const { return m_PeriodUs; }
    double nominalPeriodUs() const { return m_NominalPeriodUs; }

    // The first V-blank at or after a time
    uint64_t atOrAfter(double timeUs) const
    {
        const double vblanks = std::ceil((timeUs - m_AnchorUs) / m_PeriodUs);
        return uint64_t(std::llround(m_AnchorUs + vblanks * m_PeriodUs));
    }

private:
    double m_NominalPeriodUs = 16667;
    double m_PeriodUs = 16667;
    double m_AnchorUs = 0;
    uint64_t m_LastUs = 0;
    uint32_t m_Observations = 0;
    uint32_t m_Outliers = 0;
};

// On a fixed-refresh display a frame is shown at the first V-blank at or
// after its target. When the source rate is a whole multiple of the refresh
// interval, slow clock drift moves every target across a V-blank boundary
// together, and noise at the boundary would alternate repeats and skips. The
// lock shifts targets by a trim that keeps them mid-interval, so drift costs
// one deliberate repeat or skip each time the trim wraps. The trim stays
// within half a refresh either side of zero: a target then still lands on a
// V-blank no earlier than itself, and on average no later than it would
// without the lock.
class PhaseLock
{
public:
    static constexpr double Gain = 0.05;
    static constexpr double LockTolerance = 0.02;

    void reset() { m_TrimUs = 0; }

    double trimUs() const { return m_TrimUs; }

    uint64_t assign(uint64_t targetUs, const VblankGrid& grid) const
    {
        return grid.atOrAfter(targetUs + m_TrimUs);
    }

    void update(uint64_t targetUs, uint64_t vblankUs, double refreshPeriodUs, double sourcePeriodUs)
    {
        const double ratio = sourcePeriodUs / refreshPeriodUs;
        const double nearest = std::round(ratio);
        if (nearest < 1 || std::fabs(ratio - nearest) > LockTolerance * nearest) {
            m_TrimUs *= 0.95;
            return;
        }

        const double marginUs = vblankUs - (targetUs + m_TrimUs);
        m_TrimUs += Gain * (marginUs - refreshPeriodUs / 2);
        if (m_TrimUs >= refreshPeriodUs / 2) {
            m_TrimUs -= refreshPeriodUs;
        }
        else if (m_TrimUs < -refreshPeriodUs / 2) {
            m_TrimUs += refreshPeriodUs;
        }
    }

private:
    double m_TrimUs = 0;
};

// Whether the display refreshes at its nominal rate or only when frames
// arrive (VRR, or Windows changing the refresh rate on demand), from the
// times of real V-blanks. A fixed-rate display has a V-blank every refresh
// whether or not anything was presented. An adaptive one has a V-blank only
// for a new frame, so over a second it shows clearly fewer than the nominal
// rate, unless the stream runs at that rate, where the two behave alike
// anyway. Judged over whole seconds with a band between the two thresholds,
// so it doesn't flap.
class RefreshClassifier
{
public:
    enum class Result : uint8_t {
        Unknown,
        Fixed,
        Adaptive,
    };

    static constexpr uint64_t WindowUs = 1000000;
    // Shares of the nominal refresh rate
    static constexpr double AdaptiveBelow = 0.85;
    static constexpr double FixedAbove = 0.95;
    // A wakeup gap this long is a stall of the source, not refreshes the
    // display skipped; even a VRR display's slowest refresh is far shorter
    static constexpr uint64_t StallUs = 100000;

    void configure(double nominalPeriodUs)
    {
        m_NominalPeriodUs = nominalPeriodUs;
        m_Result = Result::Unknown;
        m_WindowStartUs = 0;
        m_LastUs = 0;
        m_Count = 0;
        m_LastShare = 0;
    }

    Result observe(uint64_t atUs)
    {
        if (m_WindowStartUs == 0 || atUs <= m_LastUs || atUs - m_LastUs > StallUs) {
            m_WindowStartUs = atUs;
            m_LastUs = atUs;
            m_Count = 0;
            return m_Result;
        }
        m_LastUs = atUs;
        m_Count++;
        if (atUs - m_WindowStartUs >= WindowUs) {
            m_LastShare = m_Count * m_NominalPeriodUs / double(atUs - m_WindowStartUs);
            if (m_LastShare < AdaptiveBelow) {
                m_Result = Result::Adaptive;
            }
            else if (m_LastShare > FixedAbove) {
                m_Result = Result::Fixed;
            }
            m_WindowStartUs = atUs;
            m_Count = 0;
        }
        return m_Result;
    }

    Result result() const { return m_Result; }
    // V-blanks seen in the last whole window, as a share of the nominal rate
    double lastShare() const { return m_LastShare; }

private:
    double m_NominalPeriodUs = 16667;
    Result m_Result = Result::Unknown;
    uint64_t m_WindowStartUs = 0;
    uint64_t m_LastUs = 0;
    uint32_t m_Count = 0;
    double m_LastShare = 0;
};

// Judges, from when frames actually reached the screen, which ones missed
// their planned V-blank. A compositor can add a constant refresh or more of
// lag (Windows composing a windowed frame), which costs latency but not
// smoothness, so a frame has only missed when its lag is above the least lag
// seen over the last second or so.
class MissDetector
{
public:
    static constexpr size_t Window = 128;

    void reset()
    {
        m_Count = 0;
        m_Next = 0;
    }

    // Returns whether a frame shown lagUs after its planned V-blank missed it
    bool observe(double lagUs, double periodUs)
    {
        const int lag = int(std::max(0.0, std::min(8.0, std::round(lagUs / periodUs))));
        m_Lags[m_Next] = uint8_t(lag);
        m_Next = (m_Next + 1) % Window;
        m_Count = std::min(m_Count + 1, Window);
        return lag > *std::min_element(m_Lags.begin(), m_Lags.begin() + m_Count);
    }

private:
    std::array<uint8_t, Window> m_Lags {};
    size_t m_Count = 0;
    size_t m_Next = 0;
};

// Chooses each frame's target from its timestamp and readiness
class Policy
{
public:
    struct Decision {
        // When the frame should be shown. Zero when it isn't paced.
        uint64_t targetUs = 0;
        // How late it was ready against the recent baseline
        uint64_t latenessUs = 0;
        uint64_t bufferUs = 0;
        // False for frames shown as soon as they can be: repeats, and frames
        // without a timestamp
        bool paced = false;
        bool repeat = false;
        bool restarted = false;
        bool reanchored = false;
        // Whether its lateness counts toward the buffer
        bool admitted = false;
        // From the timeline; see Timeline::Sample
        double correctionUs = 0;
        double hostJerkUs = 0;
        bool hostJerkValid = false;
    };

    // Lateness past this many times the maximum buffer is a stall. On a real
    // capture, any lower multiple shrank the buffer enough to show more
    // frames late and less evenly: frames late by more than the maximum
    // still count against the chosen share on time.
    static constexpr double StallBufferMultiple = 3;

    // A run of unrecognized zero-latency frames shorter than the buffer's
    // window can't have replaced its minimum, so it isn't worth a restart
    static constexpr uint64_t UnreportedRestartUs = PlayoutBuffer::WindowUs;

    void configure(const TimestampPacingOptions& options, int streamFps)
    {
        const TimestampPacingOptions resolved = options.resolved();
        const double periodUs = 1000000.0 / std::max(1, streamFps);
        m_Timeline.configure(resolved.smoothing, periodUs);
        m_Buffer.configure(resolved.targetPerMille,
                           uint64_t(resolved.minBufferMs) * 1000,
                           uint64_t(resolved.maxBufferMs) * 1000);
        m_MaximumBufferUs = resolved.maxBufferMs * 1000.0;
        m_Repeats = {};
        m_Service.reset();
        m_UnreportedSinceUs = 0;
        m_UnreportedPeriodUs = 0;
    }

    // decoderQueueUs is how long the frame waited to enter the decoder
    Decision schedule(bool timestampValid, int32_t frameNumber, uint32_t rtpTimestamp,
                      uint64_t readyUs, uint32_t hostLatencyUs, uint64_t decoderQueueUs = 0)
    {
        Decision decision;
        if (!timestampValid) {
            decision.bufferUs = uint64_t(m_Buffer.delayUs());
            return decision;
        }
        m_Service.add(readyUs, decoderQueueUs);
        if (m_Repeats.isRepeat(hostLatencyUs)) {
            decision.repeat = true;
            decision.bufferUs = uint64_t(m_Buffer.delayUs());
            return decision;
        }

        // Zero host latency that wasn't recognized as a repeat: a host that
        // went static before it had reported enough to trust its zeros. Those
        // frames are followed, and given long enough to fill the buffer's
        // window, their backdated timestamps move the baseline and the rate.
        // When the host reports latency again, start over from the rate in
        // force before them.
        if (hostLatencyUs == 0) {
            if (m_UnreportedSinceUs == 0) {
                m_UnreportedSinceUs = readyUs;
                m_UnreportedPeriodUs = m_Timeline.periodUs();
            }
        }
        else if (m_UnreportedSinceUs != 0) {
            if (readyUs >= m_UnreportedSinceUs + UnreportedRestartUs) {
                m_Timeline.restart(m_UnreportedPeriodUs);
            }
            m_UnreportedSinceUs = 0;
        }

        const Timeline::Sample source = m_Timeline.observe(frameNumber, rtpTimestamp);
        if (source.restarted) {
            m_Buffer.resetHistory();
        }
        decision.restarted = source.restarted;
        decision.reanchored = source.reanchored;
        decision.correctionUs = source.correctionUs;
        decision.hostJerkUs = source.hostJerkUs;
        decision.hostJerkValid = source.hostJerkValid;

        decision.latenessUs = m_Buffer.addSample(readyUs, double(readyUs) - source.sourceUs);

        // Lateness that says nothing about the next frame doesn't size the
        // buffer. Left out: a new or re-anchored timeline, whose lateness is
        // against a phase that no longer applies; a stall, late by far more
        // than the largest buffer could cover, whose handful of frames would
        // otherwise pin it at its maximum for half a minute; and any frame
        // while the decoder can't keep up with the stream.
        decision.admitted = !source.restarted && !source.reanchored &&
                            decision.latenessUs <= m_MaximumBufferUs * StallBufferMultiple &&
                            m_Service.keepsUp(readyUs, m_Timeline.periodUs());
        if (decision.admitted) {
            m_Buffer.admit(readyUs, decision.latenessUs);
        }
        const double targetUs = source.sourceUs + m_Buffer.baselineUs() + m_Buffer.delayUs();
        decision.targetUs = targetUs > 0 ? uint64_t(std::llround(targetUs)) : 0;
        decision.bufferUs = uint64_t(m_Buffer.delayUs());
        decision.paced = true;
        m_Buffer.updateDelay(readyUs);
        return decision;
    }

    double sourcePeriodUs() const { return m_Timeline.periodUs(); }
    double bufferUs() const { return m_Buffer.delayUs(); }
    double baselineUs() const { return m_Buffer.baselineUs(); }
    double windowMinimumUs() const { return m_Buffer.windowMinimumUs(); }

private:
    RepeatDetector m_Repeats;
    Timeline m_Timeline;
    PlayoutBuffer m_Buffer;
    ServiceWindow m_Service;
    double m_MaximumBufferUs = 16000;
    // When the current run of unrecognized zero-latency frames began, and
    // the source period before it
    uint64_t m_UnreportedSinceUs = 0;
    double m_UnreportedPeriodUs = 0;
};

}
