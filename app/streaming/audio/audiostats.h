#pragma once

#include "streaming/video/statsgraphs.h"

#include <mutex>

// Audio measurements for the performance graphs. The audio renderer records
// into this from its device thread and the graph sampler takes it once per
// interval. It belongs to the session, so the totals carry on across audio
// renderer reinitialization.
class AudioStats
{
public:
    // Called once per device request with the time since the previous
    // request, the audio it asked for, and what was left buffered after it
    void recordDeviceRequest(float intervalMs, float requestMs, float bufferedMs)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (intervalMs > 0) {
            m_DeviceInterval.add(intervalMs);
        }
        m_DeviceRequest.add(requestMs);
        m_Buffered.add(bufferedMs);
    }

    void setTargetMs(float targetMs)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_TargetMs = targetMs;
    }

    void recordUnderrun()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Underruns++;
    }

    void recordConcealment()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Concealments++;
    }

    // Fills in the audio counters. The per-request values are taken, so each
    // interval reports only the requests made within it.
    void sample(Overlay::StatsGraphCounters& counters)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        counters.audioBuffered = m_Buffered;
        counters.audioDeviceInterval = m_DeviceInterval;
        counters.audioDeviceRequest = m_DeviceRequest;
        counters.audioTargetMs = m_TargetMs;
        counters.audioUnderruns = m_Underruns;
        counters.audioConcealments = m_Concealments;
        m_Buffered = {};
        m_DeviceInterval = {};
        m_DeviceRequest = {};
    }

private:
    std::mutex m_Lock;
    Overlay::StatsGraphAccumulator m_Buffered;
    Overlay::StatsGraphAccumulator m_DeviceInterval;
    Overlay::StatsGraphAccumulator m_DeviceRequest;
    float m_TargetMs = 0;
    uint64_t m_Underruns = 0;
    uint64_t m_Concealments = 0;
};
