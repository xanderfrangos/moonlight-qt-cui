#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// How late each presented frame is against the host's RTP timeline. The
// offset from a frame's RTP timestamp to its presentation also contains the
// unknown host/client clock relationship and the pipeline's fixed latency,
// so lateness is measured from the smallest offset seen recently: the frame
// that got through fastest is on time, and every other frame is late by how
// much longer it took. That is the delay a pacer following RTP timestamps
// would have to add to present every frame at the host's cadence.
class PresentationLateness
{
public:
    // The baseline is the minimum over this many buckets: long enough to
    // keep the fastest frame through a burst, short enough that host/client
    // clock drift (tens of ppm, well under 1 ms over the window) is followed.
    static constexpr uint64_t BucketUs = 250000;
    static constexpr size_t Buckets = 12;
    // A larger step in the offset between consecutive frames is a new host
    // timeline, not a late frame.
    static constexpr int64_t DiscontinuityUs = 1000000;

    uint64_t observe(uint32_t rtpTimestamp, uint64_t presentUs)
    {
        // Unsigned subtraction handles RTP wrap. A repeated or backwards
        // timestamp starts a new timeline.
        const uint32_t ticks = rtpTimestamp - m_PreviousTimestamp;
        if (!m_HaveFrame || ticks == 0 || ticks >= 0x80000000U) {
            restart(rtpTimestamp);
        }
        else {
            m_Ticks += ticks;
        }
        m_PreviousTimestamp = rtpTimestamp;

        // RTP video timestamps are 90 kHz
        const int64_t offsetUs = int64_t(presentUs) - int64_t(m_Ticks * 100 / 9);
        if (m_HaveOffset && (offsetUs > m_PreviousOffsetUs + DiscontinuityUs ||
                             offsetUs < m_PreviousOffsetUs - DiscontinuityUs)) {
            m_Slots = {};
        }
        m_PreviousOffsetUs = offsetUs;
        m_HaveOffset = true;

        const uint64_t bucket = presentUs / BucketUs;
        Slot& slot = m_Slots[bucket % Buckets];
        if (!slot.used || slot.bucket != bucket) {
            slot = { bucket, offsetUs, true };
        }
        else if (offsetUs < slot.minimumUs) {
            slot.minimumUs = offsetUs;
        }

        int64_t baselineUs = offsetUs;
        for (const Slot& other : m_Slots) {
            if (other.used && other.bucket + Buckets > bucket && other.minimumUs < baselineUs) {
                baselineUs = other.minimumUs;
            }
        }
        return uint64_t(offsetUs - baselineUs);
    }

private:
    struct Slot {
        uint64_t bucket = 0;
        int64_t minimumUs = 0;
        bool used = false;
    };

    void restart(uint32_t rtpTimestamp)
    {
        m_Ticks = rtpTimestamp;
        m_Slots = {};
        m_HaveFrame = true;
        m_HaveOffset = false;
    }

    std::array<Slot, Buckets> m_Slots {};
    uint64_t m_Ticks = 0;
    int64_t m_PreviousOffsetUs = 0;
    uint32_t m_PreviousTimestamp = 0;
    bool m_HaveFrame = false;
    bool m_HaveOffset = false;
};
