#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "pacertelemetry.h"
#include "vrr/vrrtypes.h"
#include "settings/timestamppacingoptions.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

#include <memory>

class VrrPacingWorker;
class TimestampPacer;

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

    // Returns whether a V-blank was actually observed, rather than the wait
    // failing or giving up.
    virtual bool waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
        return false;
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
    Overlay::TimingGraphSnapshot timingGraphSnapshot() const;

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

    // Only VRR and timestamp pacing consume the decoder-facing pacing metadata.
    void submitFrame(PacedFrame&& frame);

    void submitFrame(AVFrame* frame);

    bool isVrrActive() const;

    // Whether frames are paced to their host timestamps. It and VRR are
    // never both active.
    bool isTimestampPacingActive() const;

    // Whether timestamp pacing can place frames on a V-blank grid
    bool isTimestampVblankGridAvailable() const;

    // TEMPORARY: the pacing mode, for the pacing log's header
    QString describeForPacingLog() const;

    bool initialize(SDL_Window* window, int maxVideoFps,
                    bool enablePacing, bool enableVsync,
                    bool enableVrr, int vrrDisplayRefreshHz,
                    bool smoothVrrFrameTiming = true,
                    const QString& calibrationKey = QString(),
                    int vrrLatencyMode = 0, VrrTimingOptions vrrTimingOptions = {},
                    TimestampPacingOptions timestampPacing = {});

    void notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

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
    PacerTelemetry m_Telemetry;
    std::unique_ptr<VrrPacingWorker> m_VrrWorker;
    std::unique_ptr<TimestampPacer> m_TimestampPacer;
};
