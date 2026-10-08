#pragma once

#include "tracefile.h"
#include "vrr/tracequeue.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// Timestamp pacing's frame trace. It is written whenever VRR Pacing Mode's
// would be (MOONLIGHT_VRR_TRACE, which the Settings checkbox also sets), beside
// it: Moonlight.vrrtrace becomes Moonlight.tstrace. Same container format and
// file handling (TraceFile); its own rows, since timestamp pacing has none of
// VRR Pacing Mode's controller state and vrrreplay can't replay it.
//
// Every thread that touches a frame records a row: the decoder when it is
// scheduled, the pacing thread when it is released or replaced, the render
// thread when its present returns, and the V-sync and display-report threads.
// Recording copies a row into a bounded queue and never waits; the writer
// thread formats and writes it. A full queue drops rows, and the footer
// counts them.
class TimestampTrace
{
public:
    enum class Event : uint8_t {
        // Session settings, once at the start
        Session,
        // The policy chose a frame's target
        Scheduled,
        // A full queue evicted its oldest frame
        Evicted,
        // A newer frame due at the same time replaced this one
        Superseded,
        // The frame was handed to the renderer
        Released,
        // The frame's present call returned
        Presented,
        // The V-sync source woke
        Vsync,
        // The renderer reported a display time (Gamescope)
        DisplayEvent,
        // The renderer reported when a present was displayed (D3D11)
        FrameDisplayed,
        // The display mode in use changed
        DisplayMode,
    };

    // All times are LiGetMicroseconds(). Zero when not applicable.
    struct Row {
        Event event = Event::Session;
        uint64_t sequence = 0;
        uint64_t eventUs = 0;

        // Frame identity and delivery
        int32_t frameNumber = -1;
        bool rtpValid = false;
        uint32_t rtpTimestamp = 0;
        uint32_t hostLatencyUs = 0;
        uint64_t receiveUs = 0;
        uint64_t reassembledUs = 0;
        uint64_t decodeSubmitUs = 0;
        uint64_t decoderOutputUs = 0;
        uint64_t decodeCompleteUs = 0;
        uint64_t decoderQueueUs = 0;
        uint64_t readyUs = 0;

        // Scheduled: the policy's decision
        bool paced = false;
        bool admitted = false;
        bool repeat = false;
        bool restarted = false;
        bool reanchored = false;
        bool late = false;
        // The policy's target, and the one used after the hold limit
        uint64_t policyTargetUs = 0;
        uint64_t targetUs = 0;
        uint64_t bufferUs = 0;
        uint64_t latenessUs = 0;
        int64_t correctionUs = 0;
        int64_t hostJerkUs = 0;
        bool hostJerkValid = false;
        uint64_t sourcePeriodUs = 0;
        // Frames waiting after this event
        uint32_t queueDepth = 0;

        // Released
        uint64_t plannedReleaseUs = 0;
        bool vblankGrid = false;
        uint64_t vblankUs = 0;
        int64_t trimUs = 0;
        uint64_t renderLeadUs = 0;
        // Lead plus V-blank margin plus missed V-blank margin, on the grid
        uint64_t releaseLeadUs = 0;
        uint64_t extraMarginUs = 0;
        uint64_t wakeLeadUs = 0;
        uint64_t schedulerDelayUs = 0;
        bool schedulerDelayValid = false;
        uint32_t superseded = 0;

        // Presented
        uint64_t releaseUs = 0;
        uint64_t renderStartUs = 0;
        uint64_t presentUs = 0;
        uint64_t plannedPresentUs = 0;

        // Display reports
        uint64_t presentStartUs = 0;
        uint64_t displayUs = 0;
        uint64_t refreshPeriodUs = 0;
        uint64_t gridPeriodUs = 0;
        bool matched = false;
        bool missed = false;

        // DisplayMode, Released and Session
        uint8_t displayMode = 0;
        bool displayModeMeasured = false;
        uint32_t refreshSharePerMille = 0;

        // Session
        int32_t streamFps = 0;
        int32_t displayHz = 0;
        int32_t smoothing = 0;
        int32_t targetPerMille = 0;
        int32_t minBufferMs = 0;
        int32_t maxBufferMs = 0;
        int32_t vsyncMarginUs = 0;
        bool vblankGridAvailable = false;
        bool presentFlipsImmediately = false;
        bool compositorProbe = false;
        bool measureRefresh = false;
    };

    // The trace path for a VRR trace path: its .vrrtrace (or other) suffix
    // becomes .tstrace, and a .csv path stays CSV as <base>.tstrace.csv
    static QString pathFor(const QString& vrrTracePath);

    // Opens the trace when MOONLIGHT_VRR_TRACE asks for one, otherwise null
    static std::unique_ptr<TimestampTrace> openIfRequested();

    // Stops accepting rows, writes those queued and the footer
    ~TimestampTrace();

    // Any thread. Never blocks.
    void record(Row row);

private:
    TimestampTrace() = default;
    void run();
    void write(const Row& row);

    TraceFile m_File;
    Vrr13::TraceQueue<Row, 16384> m_Queue;
    std::atomic_bool m_Accepting { false };
    std::atomic_bool m_Stopping { false };
    std::atomic_uint m_ProducersActive { 0 };
    std::atomic_uint64_t m_Sequence { 0 };
    std::atomic_uint64_t m_Enqueued { 0 };
    std::atomic_uint64_t m_Dropped { 0 };
    std::thread m_Thread;
};
