#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <QMutex>
#include "vrr/readinesswindow.h"
#include "../../timinggraph.h"
#include "../../presentationlateness.h"

// Pacer work can happen on the decoder, render, V-sync, and VRR worker
// threads. Keep its cumulative measurements separate from VIDEO_STATS, which
// is owned by the decoder thread and is periodically windowed/reset there.
// A snapshot is copied while holding this mutex, so its counters and latest
// state always describe one coherent publication.
struct PacerTelemetrySnapshot {
    uint64_t sequence = 0;

    uint64_t renderedFrames = 0;
    uint64_t pacerDroppedFrames = 0;
    uint64_t totalClientProcessingTimeUs = 0;
    uint64_t totalQueuePacingTimeUs = 0;
    uint64_t totalRenderingTimeUs = 0;

    bool vrrActive = false;
    uint64_t vrrPacingDroppedFrames = 0;
    uint64_t vrrEligibleFrames = 0;
    uint64_t vrrPrepareLateFrames = 0;
    Vrr13::ReadinessWindow::Snapshot vrrReadiness;
    uint64_t vrrOnTimeTargetPerMillion = 0;
    bool vrrBufferAtLimit = false;
    uint64_t vrrQueueResidenceUs = 0;
    uint64_t vrrDecodeWaitUs = 0;
    uint64_t vrrBufferUs = 0;
    uint64_t vrrPreparationUs = 0, vrrPresentCallUs = 0, vrrGpuReadyWaitUs = 0;
    uint64_t vrrGpuReadyWaitFrames = 0;
    uint64_t vrrPresentedFrames = 0, vrrQueuePacingUs = 0;
    uint64_t vrrLatchedFrames = 0;
    uint64_t vrrMotionPairs = 0;
    uint64_t vrrMotionHitches = 0;
    uint64_t vrrCadenceIntervals = 0;
    uint64_t vrrCadenceHitches = 0;
    uint64_t vrrEstimatedCadenceIntervals = 0;
    uint64_t vrrEstimatedCadenceHitches = 0;
    uint64_t vrrTargetWaitEntryLateFrames = 0;
    uint64_t vrrPresentFailedFrames = 0;
    uint64_t vrrPresentCancelledFrames = 0;
    uint64_t vrrSpacingCorrections = 0;

    uint64_t vrrPrepareLatenessP50Us = 0;
    uint64_t vrrPrepareLatenessP95Us = 0;
    uint64_t vrrPrepareLatenessP99Us = 0;
    int64_t vrrSubmitErrorP50Us = 0;
    int64_t vrrSubmitErrorP95Us = 0;
    int64_t vrrSubmitErrorP99Us = 0;
    int64_t vrrSubmitErrorMaxUs = 0;

    // These are a decision-time sample, not an aggregate or a proxy for the
    // target used by a different frame.
    uint64_t vrrStateSequence = 0;
    uint64_t vrrStateSampleTimeUs = 0;
    int64_t vrrReadinessBudgetUs = 0;
    uint64_t vrrTimingBudgetUs = 0;
    uint64_t vrrRenderLeadUs = 0;
    uint64_t vrrRenderWakeLeadUs = 0;
    uint64_t vrrTargetWakeLeadUs = 0;
    uint64_t vrrGuardUs = 0;
    uint64_t vrrSourcePeriodUs = 0;
    uint64_t vrrAppliedBufferUs = 0, vrrBufferCapUs = 0, vrrGpuReadinessLeadUs = 0;

    // Timestamp pacing. Counters are cumulative; the rest is the latest state.
    bool timestampActive = false;
    // Frames scheduled to a target, and those ready only after it
    uint64_t timestampPacedFrames = 0;
    uint64_t timestampLateFrames = 0;
    // Paced frames left out of sizing the buffer: re-anchored timelines,
    // stalls, and frames while the decoder fell behind
    uint64_t timestampIgnoredFrames = 0;
    // Host repeats and frames without a timestamp, shown when ready
    uint64_t timestampUnpacedFrames = 0;
    // Replaced by a newer frame due at the same time
    uint64_t timestampSupersededFrames = 0;
    uint64_t timestampBufferUs = 0;
    uint64_t timestampSourcePeriodUs = 0;
    int64_t timestampTrimUs = 0;
    // Whether frames are being placed on a measured V-blank grid
    bool timestampVblankGrid = false;
    // TimestampPacer::DisplayMode from a compositor probe; 0 without one
    uint8_t timestampDisplayMode = 0;
    bool timestampDisplayModeMeasured = false;
    // From actual display times, where the renderer reports them: frames
    // shown on their planned V-blank and those a refresh or more late, and
    // the submit margin the misses added
    uint64_t timestampVblankHits = 0;
    uint64_t timestampVblankMisses = 0;
    uint64_t timestampExtraMarginUs = 0;
};

// The stats graphs sample ten times a second and plot counters only. Copying
// the percentile sample arrays and sorting them under the telemetry lock would
// put avoidable work on the presentation path at that rate, so they read this
// instead of a full snapshot.
struct PacerTelemetryCounters {
    uint64_t renderedFrames = 0;
    uint64_t pacerDroppedFrames = 0;
    // TimestampPacer::DisplayMode right now, and whether it was measured
    uint8_t timestampDisplayMode = 0;
    bool timestampDisplayModeMeasured = false;
};

// Per-sample statistics of one measure, in microseconds. Signed, since some
// timestamp pacing measures fall either side of zero.
struct PacerAccumulator {
    uint32_t count = 0;
    int64_t sumUs = 0;
    int64_t minUs = 0;
    int64_t maxUs = 0;

    void add(int64_t valueUs)
    {
        if (count == 0 || valueUs < minUs) {
            minUs = valueUs;
        }
        if (count == 0 || valueUs > maxUs) {
            maxUs = valueUs;
        }
        sumUs += valueUs;
        count++;
    }
};

// Intervals between presented frames, accumulated since the last take. The
// stats graphs plot the spread as well as the mean, because an interval
// average hides exactly the single late frame that is felt as a stutter.
struct PacerFrametimeStats {
    uint32_t count = 0;
    uint64_t sumUs = 0;
    uint64_t minUs = 0;
    uint64_t maxUs = 0;

    // Rendering time of each presented frame over the same window, bounded
    // the same way as the overlay's average rendering time
    uint32_t renderingCount = 0;
    uint64_t renderingSumUs = 0;
    uint64_t renderingMinUs = 0;
    uint64_t renderingMaxUs = 0;

    // Change in the interval between presented frames from the previous
    // interval. Frames more than 50 ms apart break the chain.
    PacerAccumulator presentedJerk;

    // Lateness against the host's RTP timeline of each frame the fixed
    // pacing path presented, over the same window. VRR paces to those
    // timestamps itself and doesn't report it.
    uint32_t latenessCount = 0;
    uint64_t latenessSumUs = 0;
    uint64_t latenessMinUs = 0;
    uint64_t latenessMaxUs = 0;

    // Timestamp pacing over the same window. Lateness is measured at
    // readiness against the buffer's baseline, for paced frames.
    PacerAccumulator timestampLateness;
    // Change in the raw spacing of host timestamps between consecutive frames
    PacerAccumulator timestampHostJerk;
    // Smoothed minus raw source time
    PacerAccumulator timestampCorrection;
    uint32_t timestampReanchors = 0;
    // From a frame's target to the V-blank it was assigned, on the V-blank grid
    PacerAccumulator timestampVblankWait;
    // Present return minus when the pacer expected it, and the time from
    // handing a frame to the renderer until its present returned
    PacerAccumulator timestampScheduleError;
    PacerAccumulator timestampReleaseToPresent;
    // Latest state. Unlike the rest, it carries over from one take to the
    // next, so it is never missing from an interval without frames.
    bool timestampStateValid = false;
    uint64_t timestampBufferUs = 0;
    int64_t timestampTrimUs = 0;
    uint64_t timestampRenderLeadUs = 0;
};

struct TimestampScheduleSample {
    bool paced = false;
    // Its lateness counted toward the buffer
    bool admitted = false;
    // Ready only after its target
    bool late = false;
    uint64_t latenessUs = 0;
    uint64_t bufferUs = 0;
    uint64_t sourcePeriodUs = 0;
    int64_t correctionUs = 0;
    bool reanchored = false;
    int64_t hostJerkUs = 0;
    bool hostJerkValid = false;
};

struct VrrTelemetrySample {
    Overlay::TimingGraphInput graph;
    uint64_t queueResidenceUs = 0;
    uint64_t decodeWaitUs = 0;
    uint64_t bufferUs = 0;
    uint64_t submissionUs = 0;
    // Reset only at a real presentation epoch boundary. Local drops and host
    // stalls affect motion and must not erase the following interval.
    bool motionDiscontinuity = false;
    uint64_t decisionTimeUs = 0;
    uint64_t clientProcessingTimeUs = 0;
    uint64_t renderingTimeUs = 0;
    uint64_t preparationUs = 0, presentCallUs = 0, gpuReadyWaitUs = 0;
    bool gpuReadyWaitValid = false;
    bool latched = false;
    uint64_t preparationLatenessUs = 0;
    // Cumulative verified display-interval counters from the controller.
    uint64_t cadenceIntervals = 0;
    uint64_t cadenceHitches = 0;
    uint64_t estimatedCadenceIntervals = 0;
    uint64_t estimatedCadenceHitches = 0;
    int64_t submitErrorUs = 0;

    bool prepareLate = false;
    bool targetWaitEntryLate = false;
    bool spacingCorrected = false;
    bool presented = false;
    bool cancelled = false;

    int64_t readinessBudgetUs = 0;
    uint64_t timingBudgetUs = 0;
    uint64_t renderLeadUs = 0;
    uint64_t renderWakeLeadUs = 0;
    uint64_t targetWakeLeadUs = 0;
    uint64_t guardUs = 0;
    uint64_t sourcePeriodUs = 0;
    uint64_t bufferCapUs = 0, gpuReadinessLeadUs = 0;
};

class PacerTelemetry {
public:
    Overlay::TimingGraphSnapshot timingGraphSnapshot() const
    {
        Overlay::TimingGraphSnapshot points;
        points.reserve(Overlay::TimingGraphHistory::Capacity); // Allocate before locking.
        QMutexLocker lock(&m_Lock);
        m_TimingGraph.copyTo(points);
        return points;
    }
    PacerTelemetrySnapshot snapshot() const
    {
        QMutexLocker lock(&m_Lock);
        PacerTelemetrySnapshot snapshot = m_Snapshot;
        populatePrepareLatenessPercentilesLocked(snapshot);
        populateSubmitErrorPercentilesLocked(snapshot);
        return snapshot;
    }

    PacerTelemetryCounters counters() const
    {
        QMutexLocker lock(&m_Lock);
        return { m_Snapshot.renderedFrames, m_Snapshot.pacerDroppedFrames,
                 m_Snapshot.timestampDisplayMode, m_Snapshot.timestampDisplayModeMeasured };
    }

    // Just the VRR readiness window, without the percentile work snapshot()
    // does, for the graphs to read ten times a second
    Vrr13::ReadinessWindow::Snapshot vrrReadiness(bool* active) const
    {
        QMutexLocker lock(&m_Lock);
        *active = m_Snapshot.vrrActive;
        return m_Snapshot.vrrReadiness;
    }

    // Taking rather than reading keeps the accumulated window aligned with the
    // graph sampling interval instead of the whole session.
    PacerFrametimeStats takeFrametimeStats()
    {
        QMutexLocker lock(&m_Lock);
        const PacerFrametimeStats stats = m_Frametime;
        m_Frametime = {};
        m_Frametime.timestampStateValid = stats.timestampStateValid;
        m_Frametime.timestampBufferUs = stats.timestampBufferUs;
        m_Frametime.timestampTrimUs = stats.timestampTrimUs;
        m_Frametime.timestampRenderLeadUs = stats.timestampRenderLeadUs;
        return stats;
    }

    void beginVrrSession()
    {
        QMutexLocker lock(&m_Lock);
        m_Snapshot.vrrActive = true;
        touchLocked();
    }

    void beginTimestampSession()
    {
        QMutexLocker lock(&m_Lock);
        m_Snapshot.timestampActive = true;
        touchLocked();
    }

    void recordTimestampSchedule(const TimestampScheduleSample& sample)
    {
        QMutexLocker lock(&m_Lock);
        if (sample.paced) {
            ++m_Snapshot.timestampPacedFrames;
            m_Snapshot.timestampLateFrames += sample.late;
            m_Snapshot.timestampIgnoredFrames += !sample.admitted;
            m_Frametime.timestampLateness.add((int64_t)sample.latenessUs);
            m_Frametime.timestampCorrection.add(sample.correctionUs);
            m_Frametime.timestampReanchors += sample.reanchored;
            if (sample.hostJerkValid) {
                m_Frametime.timestampHostJerk.add(sample.hostJerkUs);
            }
        }
        else {
            ++m_Snapshot.timestampUnpacedFrames;
        }
        m_Snapshot.timestampBufferUs = sample.bufferUs;
        m_Snapshot.timestampSourcePeriodUs = sample.sourcePeriodUs;
        m_Frametime.timestampStateValid = true;
        m_Frametime.timestampBufferUs = sample.bufferUs;
        touchLocked();
    }

    void recordTimestampRelease(int64_t trimUs, bool vblankGrid,
                                bool vblankWaitValid, int64_t vblankWaitUs)
    {
        QMutexLocker lock(&m_Lock);
        m_Snapshot.timestampTrimUs = trimUs;
        m_Snapshot.timestampVblankGrid = vblankGrid;
        m_Frametime.timestampTrimUs = trimUs;
        if (vblankWaitValid) {
            m_Frametime.timestampVblankWait.add(vblankWaitUs);
        }
        touchLocked();
    }

    void recordTimestampPresent(bool scheduleErrorValid, int64_t scheduleErrorUs,
                                uint64_t releaseToPresentUs, uint64_t renderLeadUs)
    {
        QMutexLocker lock(&m_Lock);
        if (scheduleErrorValid) {
            m_Frametime.timestampScheduleError.add(scheduleErrorUs);
        }
        m_Frametime.timestampReleaseToPresent.add((int64_t)releaseToPresentUs);
        m_Frametime.timestampRenderLeadUs = renderLeadUs;
        touchLocked();
    }

    void recordTimestampVblankResult(bool missed, uint64_t extraMarginUs)
    {
        QMutexLocker lock(&m_Lock);
        if (missed) {
            ++m_Snapshot.timestampVblankMisses;
        }
        else {
            ++m_Snapshot.timestampVblankHits;
        }
        m_Snapshot.timestampExtraMarginUs = extraMarginUs;
        touchLocked();
    }

    // measured is set when the mode came from V-blank times rather than from
    // the compositor
    void recordTimestampDisplayMode(uint8_t mode, bool measured)
    {
        QMutexLocker lock(&m_Lock);
        m_Snapshot.timestampDisplayMode = mode;
        m_Snapshot.timestampDisplayModeMeasured = measured;
        touchLocked();
    }

    void recordTimestampSuperseded()
    {
        QMutexLocker lock(&m_Lock);
        ++m_Snapshot.pacerDroppedFrames;
        ++m_Snapshot.timestampSupersededFrames;
        touchLocked();
    }

    void recordLegacyDrop()
    {
        QMutexLocker lock(&m_Lock);
        ++m_Snapshot.pacerDroppedFrames;
        touchLocked();
    }

    void recordLegacyFrame(uint64_t clientProcessingTimeUs,
                           uint64_t renderingTimeUs,
                           uint64_t presentUs,
                           bool rtpTimestampValid = false,
                           uint32_t rtpTimestamp = 0)
    {
        QMutexLocker lock(&m_Lock);
        recordPresentedTimingLocked(clientProcessingTimeUs,
                                    renderingTimeUs, 0, presentUs);
        if (rtpTimestampValid && presentUs != 0) {
            const uint64_t latenessUs = m_Lateness.observe(rtpTimestamp, presentUs);
            if (m_Frametime.latenessCount == 0 || latenessUs < m_Frametime.latenessMinUs) {
                m_Frametime.latenessMinUs = latenessUs;
            }
            if (latenessUs > m_Frametime.latenessMaxUs) {
                m_Frametime.latenessMaxUs = latenessUs;
            }
            m_Frametime.latenessSumUs += latenessUs;
            m_Frametime.latenessCount++;
        }
        touchLocked();
    }

    void recordVrrDrop()
    {
        QMutexLocker lock(&m_Lock);
        ++m_Snapshot.pacerDroppedFrames;
        ++m_Snapshot.vrrPacingDroppedFrames;
        touchLocked();
    }

    void recordVrrReadiness(uint64_t atUs, uint64_t latenessUs, bool dropped,
                           uint64_t targetPerMillion, bool capacityLimited,
                           bool decisionValid, bool thresholdedMissPolicy,
                           bool meaningfulMissesOnly = false, bool intervalPolicy = false,
                           const Vrr13::IntervalBuffer::Stats* interval = nullptr)
    {
        QMutexLocker lock(&m_Lock);
        m_ReadinessWindow.record(atUs, latenessUs, dropped);
        const auto intervalStats = interval ? *interval : m_Snapshot.vrrReadiness.interval;
        m_Snapshot.vrrReadiness = m_ReadinessWindow.snapshot(
            0, thresholdedMissPolicy, meaningfulMissesOnly);
        m_Snapshot.vrrReadiness.intervalPolicy = intervalPolicy;
        m_Snapshot.vrrReadiness.interval = intervalStats;
        m_Snapshot.vrrOnTimeTargetPerMillion = targetPerMillion;
        if (decisionValid) m_Snapshot.vrrBufferAtLimit = capacityLimited;
        touchLocked();
    }

    void recordVrrOutcome(bool presented, bool cancelled)
    {
        QMutexLocker lock(&m_Lock);
        recordVrrOutcomeLocked(presented, cancelled);
        touchLocked();
    }

    void recordVrrFrame(const VrrTelemetrySample& sample)
    {
        QMutexLocker lock(&m_Lock);
        m_TimingGraph.record(sample.graph);

        if (sample.motionDiscontinuity) {
            m_LastMotionSubmissionUs = m_LastMotionIntervalUs = 0;
        }
        if (sample.presented && !sample.cancelled && sample.submissionUs) {
            if (m_LastMotionSubmissionUs && sample.submissionUs > m_LastMotionSubmissionUs) {
                const uint64_t interval = sample.submissionUs - m_LastMotionSubmissionUs;
                if (m_LastMotionIntervalUs) {
                    ++m_Snapshot.vrrMotionPairs;
                    const uint64_t jerk = std::max(interval, m_LastMotionIntervalUs) -
                        std::min(interval, m_LastMotionIntervalUs);
                    m_Snapshot.vrrMotionHitches += jerk > 2000;
                }
                m_LastMotionIntervalUs = interval;
            }
            else {
                m_LastMotionIntervalUs = 0;
            }
            m_LastMotionSubmissionUs = sample.submissionUs;
        }
        ++m_Snapshot.vrrEligibleFrames;
        m_Snapshot.vrrCadenceIntervals = sample.cadenceIntervals;
        m_Snapshot.vrrCadenceHitches = sample.cadenceHitches;
        m_Snapshot.vrrEstimatedCadenceIntervals = sample.estimatedCadenceIntervals;
        m_Snapshot.vrrEstimatedCadenceHitches = sample.estimatedCadenceHitches;
        if (sample.prepareLate) {
            ++m_Snapshot.vrrPrepareLateFrames;
            addPrepareLatenessLocked(sample.preparationLatenessUs);
        }
        if (sample.targetWaitEntryLate) {
            ++m_Snapshot.vrrTargetWaitEntryLateFrames;
        }
        // Submission error is meaningful only for output that was actually
        // presented. Cancelled submissions are reported separately.
        if (sample.presented && !sample.cancelled) {
            addSubmitErrorLocked(sample.submitErrorUs);
        }
        if (sample.spacingCorrected) {
            ++m_Snapshot.vrrSpacingCorrections;
        }
        recordVrrOutcomeLocked(sample.presented, sample.cancelled);
        if (sample.presented && !sample.cancelled) {
            m_Snapshot.vrrQueueResidenceUs += sample.queueResidenceUs;
            m_Snapshot.vrrDecodeWaitUs += sample.decodeWaitUs;
            m_Snapshot.vrrBufferUs += sample.bufferUs;
            m_Snapshot.vrrPreparationUs += sample.preparationUs;
            m_Snapshot.vrrPresentCallUs += sample.presentCallUs;
            if (sample.gpuReadyWaitValid) {
                m_Snapshot.vrrGpuReadyWaitUs += sample.gpuReadyWaitUs;
                ++m_Snapshot.vrrGpuReadyWaitFrames;
            }
            m_Snapshot.vrrLatchedFrames += sample.latched;
            const uint64_t priorQueueUs = m_Snapshot.totalQueuePacingTimeUs;
            recordPresentedTimingLocked(sample.clientProcessingTimeUs,
                                        sample.renderingTimeUs, sample.decodeWaitUs,
                                        sample.submissionUs);
            m_Snapshot.vrrQueuePacingUs += m_Snapshot.totalQueuePacingTimeUs - priorQueueUs;
            ++m_Snapshot.vrrPresentedFrames;
        }

        touchLocked();
        m_Snapshot.vrrStateSequence = m_Snapshot.sequence;
        m_Snapshot.vrrStateSampleTimeUs = sample.decisionTimeUs;
        m_Snapshot.vrrReadinessBudgetUs = sample.readinessBudgetUs;
        m_Snapshot.vrrTimingBudgetUs = sample.timingBudgetUs;
        m_Snapshot.vrrRenderLeadUs = sample.renderLeadUs;
        m_Snapshot.vrrRenderWakeLeadUs = sample.renderWakeLeadUs;
        m_Snapshot.vrrTargetWakeLeadUs = sample.targetWakeLeadUs;
        m_Snapshot.vrrGuardUs = sample.guardUs;
        m_Snapshot.vrrSourcePeriodUs = sample.sourcePeriodUs;
        m_Snapshot.vrrAppliedBufferUs = sample.bufferUs;
        m_Snapshot.vrrBufferCapUs = sample.bufferCapUs;
        m_Snapshot.vrrGpuReadinessLeadUs = sample.gpuReadinessLeadUs;
    }

private:
    Vrr13::ReadinessWindow m_ReadinessWindow;
    static constexpr size_t kPrepareLatenessSampleCount = 128;
    static constexpr size_t kSubmitErrorSampleCount = 128;

    static size_t percentileIndex(size_t count, size_t percentile)
    {
        // Use the nearest-rank definition. count is nonzero at each call.
        return ((count * percentile + 99) / 100) - 1;
    }

    void touchLocked()
    {
        ++m_Snapshot.sequence;
    }

    void recordVrrOutcomeLocked(bool presented, bool cancelled)
    {
        if (cancelled) {
            ++m_Snapshot.vrrPresentCancelledFrames;
        }
        else if (!presented) {
            ++m_Snapshot.vrrPresentFailedFrames;
        }
    }

    void recordPresentedTimingLocked(uint64_t clientProcessingTimeUs,
                                     uint64_t renderingTimeUs,
                                     uint64_t decodeWaitUs = 0,
                                     uint64_t presentUs = 0)
    {
        // Interval between this presentation and the previous one. A zero
        // timestamp means the caller has none to offer, which only breaks the
        // chain rather than inventing an interval.
        if (presentUs != 0) {
            if (m_LastPresentUs != 0 && presentUs > m_LastPresentUs) {
                const uint64_t intervalUs = presentUs - m_LastPresentUs;
                if (intervalUs <= 50000 && m_LastPresentIntervalUs != 0) {
                    m_Frametime.presentedJerk.add(
                        std::llabs((int64_t)intervalUs - (int64_t)m_LastPresentIntervalUs));
                }
                m_LastPresentIntervalUs = intervalUs <= 50000 ? intervalUs : 0;
                if (m_Frametime.count == 0 || intervalUs < m_Frametime.minUs) {
                    m_Frametime.minUs = intervalUs;
                }
                if (intervalUs > m_Frametime.maxUs) {
                    m_Frametime.maxUs = intervalUs;
                }
                m_Frametime.sumUs += intervalUs;
                m_Frametime.count++;
            }
            m_LastPresentUs = presentUs;
        }

        // GPU decode synchronization remains in internal client timing, but
        // is neither rendering nor the queue delay shown in the overlay.
        const uint64_t boundedRenderingTimeUs = std::min(
            renderingTimeUs, clientProcessingTimeUs);
        m_Snapshot.totalClientProcessingTimeUs += clientProcessingTimeUs;
        m_Snapshot.totalRenderingTimeUs += boundedRenderingTimeUs;
        if (m_Frametime.renderingCount == 0 ||
                boundedRenderingTimeUs < m_Frametime.renderingMinUs) {
            m_Frametime.renderingMinUs = boundedRenderingTimeUs;
        }
        if (boundedRenderingTimeUs > m_Frametime.renderingMaxUs) {
            m_Frametime.renderingMaxUs = boundedRenderingTimeUs;
        }
        m_Frametime.renderingSumUs += boundedRenderingTimeUs;
        m_Frametime.renderingCount++;
        m_Snapshot.totalQueuePacingTimeUs +=
            clientProcessingTimeUs - boundedRenderingTimeUs -
            std::min(decodeWaitUs, clientProcessingTimeUs - boundedRenderingTimeUs);
        ++m_Snapshot.renderedFrames;
    }

    void addPrepareLatenessLocked(uint64_t latenessUs)
    {
        m_PrepareLatenessSamples[m_NextPrepareLatenessSample] = latenessUs;
        m_NextPrepareLatenessSample =
            (m_NextPrepareLatenessSample + 1) % kPrepareLatenessSampleCount;
        m_PrepareLatenessSampleSize = std::min(
            m_PrepareLatenessSampleSize + 1, kPrepareLatenessSampleCount);
    }

    void populatePrepareLatenessPercentilesLocked(
        PacerTelemetrySnapshot& snapshot) const
    {
        if (m_PrepareLatenessSampleSize == 0) {
            return;
        }

        std::array<uint64_t, kPrepareLatenessSampleCount> sortedSamples =
            m_PrepareLatenessSamples;
        std::sort(sortedSamples.begin(),
                  sortedSamples.begin() + m_PrepareLatenessSampleSize);
        snapshot.vrrPrepareLatenessP50Us = sortedSamples[
            percentileIndex(m_PrepareLatenessSampleSize, 50)];
        snapshot.vrrPrepareLatenessP95Us = sortedSamples[
            percentileIndex(m_PrepareLatenessSampleSize, 95)];
        snapshot.vrrPrepareLatenessP99Us = sortedSamples[
            percentileIndex(m_PrepareLatenessSampleSize, 99)];
    }

    void addSubmitErrorLocked(int64_t errorUs)
    {
        m_SubmitErrorSamples[m_NextSubmitErrorSample] = errorUs;
        m_NextSubmitErrorSample =
            (m_NextSubmitErrorSample + 1) % kSubmitErrorSampleCount;
        m_SubmitErrorSampleSize = std::min(
            m_SubmitErrorSampleSize + 1, kSubmitErrorSampleCount);
    }

    void populateSubmitErrorPercentilesLocked(
        PacerTelemetrySnapshot& snapshot) const
    {
        if (m_SubmitErrorSampleSize == 0) {
            return;
        }

        std::array<int64_t, kSubmitErrorSampleCount> sortedSamples =
            m_SubmitErrorSamples;
        std::sort(sortedSamples.begin(),
                  sortedSamples.begin() + m_SubmitErrorSampleSize);
        snapshot.vrrSubmitErrorP50Us = sortedSamples[
            percentileIndex(m_SubmitErrorSampleSize, 50)];
        snapshot.vrrSubmitErrorP95Us = sortedSamples[
            percentileIndex(m_SubmitErrorSampleSize, 95)];
        snapshot.vrrSubmitErrorP99Us = sortedSamples[
            percentileIndex(m_SubmitErrorSampleSize, 99)];
        snapshot.vrrSubmitErrorMaxUs = sortedSamples[
            m_SubmitErrorSampleSize - 1];
    }

    mutable QMutex m_Lock;
    PacerTelemetrySnapshot m_Snapshot;
    Overlay::TimingGraphHistory m_TimingGraph;
    PacerFrametimeStats m_Frametime;
    PresentationLateness m_Lateness;
    uint64_t m_LastPresentUs = 0;
    uint64_t m_LastPresentIntervalUs = 0;
    uint64_t m_LastMotionSubmissionUs = 0;
    uint64_t m_LastMotionIntervalUs = 0;
    std::array<uint64_t, kPrepareLatenessSampleCount> m_PrepareLatenessSamples {};
    size_t m_NextPrepareLatenessSample = 0;
    size_t m_PrepareLatenessSampleSize = 0;
    std::array<int64_t, kSubmitErrorSampleCount> m_SubmitErrorSamples {};
    size_t m_NextSubmitErrorSample = 0;
    size_t m_SubmitErrorSampleSize = 0;
};
