#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "pacertelemetry.h"
#include "fixedvsyncsmoother.h"
#include "vrr/vrrtypes.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

#include <atomic>
#include <memory>

class VrrPacingWorker;
class FixedVsyncTrace;

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer);

    ~Pacer();

    // Stop all producer threads before a final telemetry snapshot is merged
    // by the decoder. It is safe to call this more than once.
    void shutdown();

    PacerTelemetrySnapshot telemetrySnapshot() const;

    // Counters only, without the percentile computation telemetrySnapshot()
    // performs. Used by the high-rate stats graph sampler.
    PacerTelemetryCounters telemetryCounters() const;
    Vrr13::ReadinessWindow::Snapshot vrrReadiness(bool* active) const;

    // Frames buffered ahead of display. VRR keeps its own queue; the legacy
    // paths split theirs between pacing and rendering, so this is their sum.
    uint32_t queueDepth();

    // Presented-frame intervals and rendering times since the last call.
    // Resets on read.
    PacerFrametimeStats takeFrametimeStats();

    // Only the active VRR worker consumes the decoder-facing pacing metadata.
    void submitFrame(PacedFrame&& frame);

    void submitFrame(AVFrame* frame);

    bool isVrrActive() const;

    bool initialize(SDL_Window* window, int maxVideoFps,
                    bool enablePacing, bool enableVsync,
                    bool enableVrr, int vrrDisplayRefreshHz,
                    bool smoothVrrFrameTiming = true,
                    const QString& calibrationKey = QString(),
                    int vrrLatencyMode = 0,
                    int vsyncMode = 0);

    // The StreamingPreferences::VsyncMode actually running, or -1 while VRR
    // is active. Smooth falls back to Default without a V-sync source.
    int fixedVsyncMode() const;

    void notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    // Smooth V-Sync: show the newest frame whose assigned refresh is due
    void handleSmoothVsync(uint64_t tickUs);

    // Smooth: the newest queued frame eligible for the refresh at showUs,
    // or -1. Called with the queue lock held.
    int findSmoothFrameLocked(uint64_t showUs) const;
    // Smooth: hand the chosen frame to the renderer for the refresh the
    // tick at tickUs decided, and record that tick. Unlocks the queue.
    void sendSmoothFrameAndUnlock(int chosen, uint64_t tickUs, uint64_t showUs,
                                  int queueBefore);

    // Refresh accounting shared by every fixed V-Sync mode with a V-sync source
    // Copied from a frame before it is handed to the renderer, which may
    // free it at any time afterwards
    struct ShownFrameInfo {
        int64_t rtp = -1;
        uint64_t arrivalUs = 0;
        uint64_t slotUs = 0;

        explicit ShownFrameInfo(const AVFrame* frame);
    };
    // Smooth calls this with the queue lock held, from the V-sync thread or
    // (for a frame that arrived after its tick) the decoder thread
    void recordVsyncTick(uint64_t tickUs, bool sent, uint32_t skipped,
                         int queueBefore, int queueAfter, const ShownFrameInfo* shown,
                         bool late);
    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    // Smooth: assign the frame its refresh. Called with the queue lock held.
    void admitSmoothFrameLocked(AVFrame* frame);

    // Mailbox: discard every frame still waiting in the queue so the one
    // about to be enqueued is the only candidate. Called with the lock held;
    // it is dropped while freeing frames.
    void replaceQueuedFramesForMailbox(QQueue<AVFrame*>& queue);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;
    bool m_Shutdown;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    int m_RendererAttributes;
    // StreamingPreferences::VsyncMode in effect. Never Mailbox or Smooth
    // while the VRR worker runs.
    int m_VsyncMode;
    bool m_Mailbox;
    bool m_Smooth;
    // Guarded by m_FrameQueueLock
    FixedVsyncSmoother m_Smoother;
    // Frames discarded between refreshes (Mailbox replacement), reported
    // with the next refresh
    std::atomic<uint32_t> m_SkipsSinceTick;
    // V-sync thread only, or guarded by m_FrameQueueLock under Smooth
    bool m_PrevTickRepeated;
    // Smooth: a refresh whose frame had not arrived at its tick. The frame
    // is handed over as soon as it arrives, until the deadline, so the
    // V-sync thread never blocks waiting for it (a blocked wait can sleep
    // through the next V-sync). Guarded by m_FrameQueueLock.
    struct PendingSmoothTick {
        uint64_t tickUs = 0;
        uint64_t showUs = 0;
        uint64_t deadlineUs = 0;
        int queueBefore = 0;
    };
    PendingSmoothTick m_PendingSmoothTick;
    std::unique_ptr<FixedVsyncTrace> m_Trace;
    PacerTelemetry m_Telemetry;
    std::unique_ptr<VrrPacingWorker> m_VrrWorker;
};
