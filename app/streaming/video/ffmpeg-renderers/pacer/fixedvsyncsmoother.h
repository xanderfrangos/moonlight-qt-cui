#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Host-timestamp playout for fixed-refresh V-Sync ("Smooth" V-Sync mode).
//
// Each decoded frame gets a due time on the local clock: the earliest refresh
// it may be shown on. The Pacer shows, at each refresh, the newest frame whose
// due time has been reached, so frames keep the host's capture spacing instead
// of the spacing in which they happened to finish decoding.
//
// The due time is built from two parts:
//   1. A smoothed host time. The raw RTP stamps jitter by a few milliseconds,
//      so they are fitted to the host's measured frame interval and only
//      nudged toward each raw stamp. A gap of several frames advances by
//      whole intervals. A single stamp off the cadence by more than half an
//      interval (a capture burst) is kept on the cadence; three in a row, or
//      a long stall, resynchronize to the raw stamps.
//   2. A playout offset mapping host time to local time. It follows a high
//      percentile of recent (arrival - smoothed host time), so almost every
//      frame has arrived by its due time. It moves slowly, which also absorbs
//      drift between the host clock and the local display clock, and it never
//      buffers more than two source frames beyond the fastest recent arrival.
//
// Each frame is then assigned a refresh: the one after the previous frame's,
// advanced by the rounded number of refreshes between their due times. That
// chain keeps small due-time jitter from flipping a frame between two
// neighbouring refreshes, and re-aligns only when drift moves it more than a
// millisecond from the first refresh at or after the due time.
//
// There is no single first-frame anchor: the first frames after connecting or
// after a keyframe are often late or bursty, so the offset always comes from a
// rolling window instead.
//
// Pure logic with caller-supplied times so it can be tested deterministically.
class FixedVsyncSmoother
{
public:
    struct Decision {
        // Earliest local time the frame may be shown (host time + offset)
        uint64_t dueUs = 0;
        // Refresh the frame is assigned to. Zero until a V-sync was observed.
        uint64_t slotUs = 0;
        // The slot chain was re-aligned to the due time for this frame
        bool rephased = false;
        // Host-relative microseconds since the first frame of this epoch
        int64_t hostUs = 0;
        int64_t smoothedHostUs = 0;
        int64_t offsetUs = 0;
        int64_t desiredOffsetUs = 0;
        // Arrival minus smoothed host time, local microseconds
        int64_t transitUs = 0;
        bool resynced = false;
    };

    // Margin kept between a frame's percentile arrival and its due time.
    static constexpr int64_t kMarginUs = 500;
    // Share of recent frames that should have arrived by their due time.
    static constexpr int kReadinessPercentile = 99;
    // Offset slew limits per admitted frame. Growing is faster than
    // shrinking so a burst of late frames is absorbed quickly, while latency
    // is only given back once the improvement has persisted.
    static constexpr int64_t kOffsetGrowUsPerFrame = 250;
    static constexpr int64_t kOffsetShrinkUsPerFrame = 40;
    // Frames admitted at the start of an epoch that take the desired offset
    // directly, before the window is meaningful.
    static constexpr int kWarmupFrames = 8;
    // How far before its due time a frame may be shown to keep its refresh
    // chain, before the chain re-aligns.
    static constexpr double kEarlyToleranceUs = 1000;
    // Consecutive stamps off the cadence by more than half an interval
    // before the smoothed host time snaps to the raw stamps
    static constexpr int kResyncAfterFrames = 3;
    // Recent host intervals used to measure the real frame interval
    static constexpr int kIntervalWindow = 64;

    // renderLeadUs is how long before a refresh a frame must be handed to
    // the renderer to be shown on it.
    void configure(int sourceFps, int displayHz, int64_t renderLeadUs)
    {
        m_RenderLeadUs = renderLeadUs;
        m_NominalSourcePeriodUs = sourceFps > 0 ? 1000000.0 / sourceFps : 16666.7;
        m_NominalDisplayPeriodUs = displayHz > 0 ? 1000000.0 / displayHz : 16666.7;
        // Two seconds of samples, bounded for very low or high stream rates
        m_WindowSize = std::clamp(sourceFps * 2, 30, 512);
        reset();
    }

    // Forget all timing state, for example after a long stall
    void reset()
    {
        m_HaveFrame = false;
        m_HostPeriodUs = m_NominalSourcePeriodUs;
        m_Intervals.clear();
        m_IntervalNext = 0;
        m_DisagreeingFrames = 0;
        m_DisplayPeriodUs = m_NominalDisplayPeriodUs;
        m_LastVsyncUs = 0;
        m_LastSlotUs = 0;
        m_LastSlotDueUs = 0;
        m_Transit.assign(m_WindowSize, 0);
        m_TransitCount = 0;
        m_TransitNext = 0;
        m_EpochFrames = 0;
        m_OffsetUs = 0;
        m_BufferUs = 0;
        ++m_Epoch;
    }

    Decision admit(uint32_t rtpTimestamp90k, uint64_t arrivalUs)
    {
        Decision d;

        if (!m_HaveFrame) {
            m_HaveFrame = true;
            m_LastRtpTicks = 0;
            m_LastHostUs = 0;
            m_SmoothedHostUs = 0;
            d.resynced = true;
        }
        else {
            // Unwrap the 32-bit 90 kHz clock relative to the previous frame
            const int32_t stepTicks = static_cast<int32_t>(rtpTimestamp90k - m_LastRtp);
            m_LastRtpTicks += stepTicks;
            const double hostUs = m_LastRtpTicks * (1000.0 / 90.0);
            const double deltaUs = hostUs - m_LastHostUs;

            if (deltaUs <= 0) {
                // Reordered or repeated stamp. Snap to it.
                m_SmoothedHostUs = hostUs;
                d.resynced = true;
            }
            else {
                observeHostInterval(deltaUs);
                const double intervals = std::max(1.0, std::round(deltaUs / m_HostPeriodUs));
                const double predicted = m_SmoothedHostUs + intervals * m_HostPeriodUs;
                const double error = hostUs - predicted;
                if (intervals > 30) {
                    // A long stall. Predicting through it means nothing.
                    m_SmoothedHostUs = hostUs;
                    m_DisagreeingFrames = 0;
                    d.resynced = true;
                }
                else if (std::fabs(error) > m_HostPeriodUs / 2) {
                    // One stamp far off the cadence is usually a capture
                    // burst (two frames a few ms apart). Keep it on the
                    // cadence so it does not share a refresh with its
                    // neighbour; only a persistent change resynchronizes.
                    if (++m_DisagreeingFrames >= kResyncAfterFrames) {
                        m_SmoothedHostUs = hostUs;
                        m_DisagreeingFrames = 0;
                        d.resynced = true;
                    }
                    else {
                        m_SmoothedHostUs = predicted;
                    }
                }
                else {
                    m_DisagreeingFrames = 0;
                    m_SmoothedHostUs = predicted + error / 16.0;
                }
            }
            m_LastHostUs = hostUs;
        }
        m_LastRtp = rtpTimestamp90k;

        const int64_t smoothed = static_cast<int64_t>(std::llround(m_SmoothedHostUs));
        const int64_t transit = static_cast<int64_t>(arrivalUs) - smoothed;
        m_Transit[m_TransitNext] = transit;
        m_TransitNext = (m_TransitNext + 1) % m_WindowSize;
        m_TransitCount = std::min(m_TransitCount + 1, m_WindowSize);

        // The offset is chosen so the readiness percentile of recent frames
        // arrived by their due time, but never more than one source frame
        // later than the fastest recent arrival.
        // Until the ring wraps, its samples are the first m_TransitCount slots
        m_Scratch.assign(m_Transit.begin(), m_Transit.begin() + m_TransitCount);
        const size_t rank = (static_cast<size_t>(m_TransitCount) * kReadinessPercentile + 99) / 100 - 1;
        std::nth_element(m_Scratch.begin(), m_Scratch.begin() + rank, m_Scratch.end());
        const int64_t percentile = m_Scratch[rank];
        const int64_t fastest = *std::min_element(m_Scratch.begin(), m_Scratch.end());
        const int64_t maxBufferUs = static_cast<int64_t>(2 * m_HostPeriodUs);
        const int64_t desired = std::min(percentile + kMarginUs, fastest + maxBufferUs);

        // A resync only snaps the smoothed time to the raw stamp. Both stay
        // on the same unwrapped host timeline, so the window remains valid.
        if (m_EpochFrames < kWarmupFrames) {
            m_OffsetUs = desired;
        }
        else if (desired > m_OffsetUs) {
            m_OffsetUs += std::min(desired - m_OffsetUs, kOffsetGrowUsPerFrame);
        }
        else {
            m_OffsetUs -= std::min(m_OffsetUs - desired, kOffsetShrinkUsPerFrame);
        }
        ++m_EpochFrames;

        d.hostUs = static_cast<int64_t>(std::llround(m_LastHostUs));
        d.smoothedHostUs = smoothed;
        d.transitUs = transit;
        d.desiredOffsetUs = desired;
        d.offsetUs = m_OffsetUs;
        m_BufferUs = m_OffsetUs - fastest;
        const int64_t due = smoothed + m_OffsetUs + m_RenderLeadUs;
        d.dueUs = due > 0 ? static_cast<uint64_t>(due) : 0;
        assignSlot(d);
        return d;
    }

    // Called at each refresh with the local time the V-sync source woke us
    void observeVsync(uint64_t vsyncUs)
    {
        if (m_LastVsyncUs != 0 && vsyncUs > m_LastVsyncUs) {
            const double delta = static_cast<double>(vsyncUs - m_LastVsyncUs);
            if (std::fabs(delta - m_NominalDisplayPeriodUs) < m_NominalDisplayPeriodUs * 0.25) {
                m_DisplayPeriodUs += (delta - m_DisplayPeriodUs) / 32.0;
            }
        }
        m_LastVsyncUs = vsyncUs;
    }

    // Assign the frame a refresh. Consecutive frames follow the previous
    // frame's refresh by the rounded number of refreshes between their due
    // times, so jitter in the due time cannot flip a frame between two
    // neighbouring refreshes. The chain re-aligns to the first refresh at or
    // after the due time only once drift moves it outside a tolerance band.
    void assignSlot(Decision& d)
    {
        if (m_LastVsyncUs == 0) {
            return;
        }

        const double period = m_DisplayPeriodUs;
        const double due = static_cast<double>(d.dueUs);
        const auto ceilToGrid = [&](double t) {
            const double k = std::ceil((t - static_cast<double>(m_LastVsyncUs)) / period);
            return static_cast<double>(m_LastVsyncUs) + k * period;
        };

        double slot;
        if (m_LastSlotUs == 0) {
            slot = ceilToGrid(due);
            d.rephased = true;
        }
        else {
            const double steps = std::round((due - m_LastSlotDueUs) / period);
            slot = m_LastSlotUs + steps * period;
            // Keep the chain only while it agrees, within a small tolerance,
            // with the first refresh at or after the due time. That holds
            // frames steady near a refresh boundary when the stream rate
            // divides the refresh rate (60 on 60 or 120), and falls back to
            // the plain rule for uneven ratios such as 60 on 90, where the
            // rounded step would otherwise drift.
            const double lead = slot - due;
            if (lead < -kEarlyToleranceUs || lead >= period + kEarlyToleranceUs) {
                slot = ceilToGrid(due);
                d.rephased = true;
            }
        }

        // While the host sends frames no faster than the display refreshes,
        // two frames must never share a refresh: that would skip one for no
        // reason (for example after a resync moves the due time back). Hold
        // such a frame to the next refresh instead, unless the chain has
        // fallen several refreshes behind its due times.
        if (m_LastSlotUs != 0 && m_HostPeriodUs >= 0.9 * period &&
                slot < m_LastSlotUs + period - 1) {
            const double held = m_LastSlotUs + period;
            if (held - due < 3 * period) {
                slot = held;
            }
        }

        m_LastSlotUs = slot;
        m_LastSlotDueUs = due;
        d.slotUs = static_cast<uint64_t>(std::llround(slot));
    }

    // The host's real frame interval, measured rather than taken from the
    // requested stream rate: a 90 FPS stream of a game capped at 60 sends a
    // frame every ~16.7 ms, not 11.1 ms. A trimmed mean of recent intervals
    // is used because host stamps are often bimodal (e.g. 15.9 / 18.2 ms for
    // a 59 FPS game), where a median would pick one of the two modes.
    void observeHostInterval(double deltaUs)
    {
        if (deltaUs < 2000 || deltaUs > 200000) {
            return;
        }
        if (static_cast<int>(m_Intervals.size()) < kIntervalWindow) {
            m_Intervals.push_back(deltaUs);
        }
        else {
            m_Intervals[m_IntervalNext] = deltaUs;
            m_IntervalNext = (m_IntervalNext + 1) % kIntervalWindow;
        }
        if (m_Intervals.size() < 8) {
            return;
        }
        m_IntervalScratch = m_Intervals;
        std::sort(m_IntervalScratch.begin(), m_IntervalScratch.end());
        const size_t trim = m_IntervalScratch.size() / 10;
        double sum = 0;
        for (size_t i = trim; i < m_IntervalScratch.size() - trim; i++) {
            sum += m_IntervalScratch[i];
        }
        m_HostPeriodUs = sum / (m_IntervalScratch.size() - 2 * trim);
    }

    int64_t offsetUs() const { return m_OffsetUs; }
    // How far the playout offset sits above the fastest recent arrival:
    // the buffering Smooth currently adds on top of the unavoidable transit
    int64_t bufferUs() const { return m_BufferUs; }

    uint64_t displayPeriodUs() const { return static_cast<uint64_t>(m_DisplayPeriodUs); }
    uint64_t hostPeriodUs() const { return static_cast<uint64_t>(m_HostPeriodUs); }
    uint64_t epoch() const { return m_Epoch; }

private:
    int64_t m_RenderLeadUs = 0;
    double m_NominalSourcePeriodUs = 16666.7;
    double m_NominalDisplayPeriodUs = 16666.7;
    double m_HostPeriodUs = 16666.7;
    double m_DisplayPeriodUs = 16666.7;
    uint64_t m_LastVsyncUs = 0;
    double m_LastSlotUs = 0;
    double m_LastSlotDueUs = 0;

    bool m_HaveFrame = false;
    uint32_t m_LastRtp = 0;
    int64_t m_LastRtpTicks = 0;
    double m_LastHostUs = 0;
    double m_SmoothedHostUs = 0;
    int m_DisagreeingFrames = 0;
    std::vector<double> m_Intervals;
    std::vector<double> m_IntervalScratch;
    int m_IntervalNext = 0;

    int m_WindowSize = 120;
    std::vector<int64_t> m_Transit;
    std::vector<int64_t> m_Scratch;
    int m_TransitCount = 0;
    int m_TransitNext = 0;
    int m_EpochFrames = 0;
    int64_t m_OffsetUs = 0;
    int64_t m_BufferUs = 0;
    uint64_t m_Epoch = 0;
};
