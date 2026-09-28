#pragma once

#include "pacer.h"
#include "refreshclock.h"
#include "vrr/vrrtargetwaiter.h"

#include <mutex>

// A V-sync source for compositors that report when frames were shown but
// offer no V-sync wakeup (Gamescope). It wakes the Pacer at predicted
// refreshes from a RefreshClock that the renderer's reported display times
// keep in phase, using the VRR path's precise waiter. Without reports it
// runs freely at the display's nominal rate.
class PresentTimingVsyncSource : public IVsyncSource
{
public:
    PresentTimingVsyncSource();
    virtual ~PresentTimingVsyncSource();

    virtual bool initialize(SDL_Window* window, int displayFps) override;

    virtual bool isAsync() override;

    virtual void waitForVsync() override;

    // Any thread: a refresh the compositor reported, local microseconds
    void observeRefresh(uint64_t refreshUs);

    // Current refresh period estimate
    double periodUs();

private:
    std::mutex m_Lock;
    RefreshClock m_Clock;
    VrrTargetWaiter m_Waiter;
    // V-sync thread only
    uint64_t m_LastTickUs;
    uint64_t m_WakeLeadUs;
    uint64_t m_Ticks;
    uint64_t m_LateWakeups;
};
