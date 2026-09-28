#pragma once

#include "vrr/tracequeue.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// Fixed V-Sync diagnostics for the legacy Pacer (Default, Mailbox and Smooth
// modes). Written beside MOONLIGHT_VRR_TRACE as
// "<trace>.vsync-<pid>-<ms>.csv" whenever that trace is enabled, so the
// Settings trace checkbox and the tracing launchers capture it too. Rows are
// queued without blocking and written by a background thread; a full queue
// drops rows and the footer reports how many.
class FixedVsyncTrace {
public:
    struct Row {
        // Static literal: "tick" (one per refresh), "admit" (one per frame),
        // "refresh" (a refresh the compositor reported) or "lead" (Smooth's
        // hand-over lead changed)
        const char* event = "tick";
        uint64_t timeUs = 0;
        int64_t rtp = -1;
        uint64_t arrivalUs = 0;
        uint64_t dueUs = 0;
        uint64_t slotUs = 0;
        // tick: sent, skipped, queue_before, queue_after, missed_slot, late
        // admit: host_us, smoothed_host_us, transit_us, desired_offset_us,
        //        resynced, rephased
        // refresh: uncertainty_us
        // lead: lead_us, p95_render_span_us
        int64_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
        int64_t offsetUs = 0;
        uint64_t displayPeriodUs = 0;
    };

    static std::unique_ptr<FixedVsyncTrace> create(int mode, int sourceFps, int displayHz);
    ~FixedVsyncTrace();

    void record(Row row);

private:
    FixedVsyncTrace(const QString& path, int mode, int sourceFps, int displayHz);
    void write(const QString& path, int mode, int sourceFps, int displayHz);

    Vrr13::TraceQueue<Row, 8192> m_Queue;
    std::atomic<bool> m_Stop{false}, m_Accept{true};
    std::atomic<uint64_t> m_Dropped{0};
    std::thread m_Thread;
};
