#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Predicts display refreshes from occasional reported refresh times, for
// platforms that report when frames were shown but give no V-sync wakeup
// (Gamescope through VK_GOOGLE_display_timing). Reports may be sparse, so the
// clock keeps a grid (a known refresh plus a period) and corrects it from each
// report. Without reports it runs freely at the nominal rate.
//
// Pure logic with caller-supplied times so it can be tested deterministically.
class RefreshClock {
public:
    // A report further than this from the grid is not trusted on its own
    static constexpr double kOutlierFraction = 0.125;
    // Consecutive untrusted reports before the grid moves to them (for
    // example after the compositor restarted its refresh timer)
    static constexpr int kOutliersBeforeRephase = 3;
    // Reports further apart than this many refreshes only correct the phase:
    // with a slightly wrong period the refresh count between them could be
    // miscounted.
    static constexpr int64_t kMaxPeriodSpanRefreshes = 240;
    // The period estimate stays this close to the nominal rate
    static constexpr double kMaxPeriodDeviation = 0.02;

    void configure(double nominalPeriodUs)
    {
        m_NominalPeriodUs = nominalPeriodUs > 0 ? nominalPeriodUs : 16666.7;
        m_PeriodUs = m_NominalPeriodUs;
        m_AnchorUs = 0;
        m_Outliers = 0;
        m_Accepted = 0;
        m_Rejected = 0;
        m_Rephased = 0;
    }

    // A refresh the compositor reported, on the local microsecond clock
    void observe(uint64_t refreshUs)
    {
        if (refreshUs == 0) {
            return;
        }
        const double t = static_cast<double>(refreshUs);
        if (!haveReport()) {
            m_AnchorUs = t;
            m_HaveReport = true;
            ++m_Accepted;
            return;
        }

        const double k = std::round((t - m_AnchorUs) / m_PeriodUs);
        const double predicted = m_AnchorUs + k * m_PeriodUs;
        const double error = t - predicted;

        if (std::fabs(error) > m_PeriodUs * kOutlierFraction) {
            ++m_Rejected;
            if (++m_Outliers >= kOutliersBeforeRephase) {
                m_AnchorUs = t;
                m_Outliers = 0;
                ++m_Rephased;
            }
            return;
        }
        m_Outliers = 0;
        ++m_Accepted;

        // Reports are refresh times, so trust them mostly; the small blend
        // only softens timestamp noise.
        const double anchor = predicted + error * 0.5;
        if (k >= 1 && k <= kMaxPeriodSpanRefreshes) {
            m_PeriodUs += error / k * 0.25;
            m_PeriodUs = std::clamp(m_PeriodUs,
                                    m_NominalPeriodUs * (1.0 - kMaxPeriodDeviation),
                                    m_NominalPeriodUs * (1.0 + kMaxPeriodDeviation));
        }
        // Keep the newest refresh as the anchor so predictions stay close
        if (k >= 0) {
            m_AnchorUs = anchor;
        }
    }

    // Start a free-running grid at startUs if no report arrived yet
    void seed(uint64_t startUs)
    {
        if (m_AnchorUs == 0) {
            m_AnchorUs = static_cast<double>(startUs);
        }
    }

    // The first predicted refresh strictly after afterUs. Zero until seeded.
    uint64_t nextRefreshAfter(uint64_t afterUs) const
    {
        if (m_AnchorUs == 0) {
            return 0;
        }
        double k = std::floor((static_cast<double>(afterUs) - m_AnchorUs) / m_PeriodUs) + 1;
        uint64_t next = static_cast<uint64_t>(std::llround(m_AnchorUs + k * m_PeriodUs));
        // Rounding can land exactly on afterUs
        if (next <= afterUs) {
            next = static_cast<uint64_t>(std::llround(m_AnchorUs + (k + 1) * m_PeriodUs));
        }
        return next;
    }

    bool haveReport() const { return m_HaveReport; }
    double periodUs() const { return m_PeriodUs; }
    uint64_t accepted() const { return m_Accepted; }
    uint64_t rejected() const { return m_Rejected; }
    uint64_t rephased() const { return m_Rephased; }

private:
    double m_NominalPeriodUs = 16666.7;
    double m_PeriodUs = 16666.7;
    double m_AnchorUs = 0;
    bool m_HaveReport = false;
    int m_Outliers = 0;
    uint64_t m_Accepted = 0;
    uint64_t m_Rejected = 0;
    uint64_t m_Rephased = 0;
};

// How long before a refresh Smooth must hand a frame to the renderer. Learned
// from how long recent frames took, once both they and the renderer were
// ready, until the renderer had submitted them (render thread wakeup plus CPU
// render and submit time), at a
// high percentile, plus a margin for GPU work and the compositor, which are
// not measured. It never goes below the floor, so a fast machine keeps the
// previous fixed lead; a slow one gets a longer lead instead of missing
// refreshes.
class RenderLeadEstimator {
public:
    static constexpr int kWindow = 128;
    static constexpr int kUpdateEvery = 16;
    static constexpr int kMinSamples = 32;
    // Hand-overs slower than this are stalls, not render cost
    static constexpr int64_t kMaxSpanUs = 100000;
    // Lead change per update: grow quickly to stop misses, shrink slowly
    static constexpr int64_t kGrowPerUpdateUs = 500;
    static constexpr int64_t kShrinkPerUpdateUs = 100;
    static constexpr double kPercentile = 0.95;

    void configure(int64_t floorUs, int64_t marginUs, int64_t ceilingUs)
    {
        m_FloorUs = floorUs;
        m_MarginUs = marginUs;
        m_CeilingUs = std::max(ceilingUs, floorUs);
        m_LeadUs = std::min(m_FloorUs, m_CeilingUs);
        m_Spans.assign(kWindow, 0);
        m_Count = 0;
        m_Next = 0;
        m_SinceUpdate = 0;
        m_LastPercentileUs = 0;
    }

    // Returns true when the lead changed
    bool observe(int64_t spanUs)
    {
        if (spanUs < 0 || spanUs > kMaxSpanUs || m_Spans.empty()) {
            return false;
        }
        m_Spans[m_Next] = spanUs;
        m_Next = (m_Next + 1) % kWindow;
        m_Count = std::min(m_Count + 1, kWindow);
        if (++m_SinceUpdate < kUpdateEvery || m_Count < kMinSamples) {
            return false;
        }
        m_SinceUpdate = 0;

        m_Scratch.assign(m_Spans.begin(), m_Spans.begin() + m_Count);
        const size_t index = std::min<size_t>(m_Scratch.size() - 1,
                static_cast<size_t>(kPercentile * m_Scratch.size()));
        std::nth_element(m_Scratch.begin(), m_Scratch.begin() + index, m_Scratch.end());
        m_LastPercentileUs = m_Scratch[index];

        const int64_t target = std::clamp(m_LastPercentileUs + m_MarginUs, m_FloorUs, m_CeilingUs);
        const int64_t previous = m_LeadUs;
        if (target > m_LeadUs) {
            m_LeadUs = std::min(target, m_LeadUs + kGrowPerUpdateUs);
        }
        else if (target < m_LeadUs) {
            m_LeadUs = std::max(target, m_LeadUs - kShrinkPerUpdateUs);
        }
        return m_LeadUs != previous;
    }

    int64_t leadUs() const { return m_LeadUs; }
    int64_t lastPercentileUs() const { return m_LastPercentileUs; }

private:
    int64_t m_FloorUs = 3000;
    int64_t m_MarginUs = 2000;
    int64_t m_CeilingUs = 8000;
    int64_t m_LeadUs = 3000;
    std::vector<int64_t> m_Spans;
    std::vector<int64_t> m_Scratch;
    int m_Count = 0;
    int m_Next = 0;
    int m_SinceUpdate = 0;
    int64_t m_LastPercentileUs = 0;
};
