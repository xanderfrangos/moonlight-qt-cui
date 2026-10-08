#pragma once

// TEMPORARY: a per-frame timing log for modeling a pacer that follows host
// RTP timestamps (docs/timestamp-pacing-proposal.md). Remove it once that
// model exists. Enabled by setting MOONLIGHT_PACING_LOG=1; each decoder
// instance then writes %TEMP%\moonlight-pacing-<time>.csv.
//
// Recording only copies a small record under a mutex. Formatting and file I/O
// happen on the log's own writer thread.

#include <atomic>
#include <cstdint>

class QString;

namespace PacingLog {

enum class Event : uint8_t {
    // The decoder produced a frame
    Decoded,
    // The fixed pacing path's present call returned
    Presented,
    // The fixed pacing path discarded a decoded frame
    Dropped,
    // The V-sync source woke the pacer
    Vsync,
    // The timestamp pacer chose a frame's target
    Scheduled,
};

// Not "None", which X11 headers define as a macro
enum class DropReason : uint8_t {
    NotDropped,
    // handleVsync() kept the pacing queue at its drop target
    VsyncCatchUp,
    // renderFrame() kept the render queue at its drop target
    RenderCatchUp,
    // A frame arrived while the queue was at MAX_QUEUED_FRAMES
    PacingQueueFull,
    RenderQueueFull,
    // The timestamp pacer showed a newer frame due at the same time
    TimestampSuperseded,
    // The timestamp pacer's queue was full
    TimestampQueueFull,
};

// All times are LiGetMicroseconds(), zero when not applicable
struct Record {
    Event event = Event::Decoded;
    DropReason reason = DropReason::NotDropped;
    bool rtpValid = false;
    uint8_t frameType = 0;
    int32_t frameNumber = -1;
    uint32_t rtpTimestamp = 0;
    uint32_t bytes = 0;
    uint32_t hostLatencyUs = 0;
    uint32_t queueDepth = 0;
    uint64_t eventUs = 0;
    uint64_t receiveUs = 0;
    uint64_t reassembledUs = 0;
    uint64_t presentationUs = 0;
    uint64_t decodeSubmitUs = 0;
    uint64_t decoderOutputUs = 0;
    uint64_t renderBeginUs = 0;
    // Scheduled rows: the chosen target (zero when shown on arrival), the
    // buffer in force and the frame's lateness against the recent baseline
    uint64_t targetUs = 0;
    uint64_t bufferUs = 0;
    uint64_t latenessUs = 0;
    bool repeat = false;
};

extern std::atomic<bool> g_Active;

inline bool active()
{
    return g_Active.load(std::memory_order_relaxed);
}

// Opens a new log when MOONLIGHT_PACING_LOG is set. The description becomes
// a header comment, and must say what the session is running.
void start(const QString& description);

// Flushes and closes the log. Producers must have stopped first; anything
// recorded afterwards is discarded.
void stop();

void record(const Record& record);

}
