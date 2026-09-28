#include "presenttimingvsyncsource.h"

#include <Limelight.h>

#include <algorithm>

PresentTimingVsyncSource::PresentTimingVsyncSource()
    : m_Waiter(VrrTargetWaiterHooks{[]() { return LiGetMicroseconds(); }, {}, {}}),
      m_LastTickUs(0),
      m_WakeLeadUs(0),
      m_Ticks(0),
      m_LateWakeups(0)
{
}

PresentTimingVsyncSource::~PresentTimingVsyncSource()
{
    std::lock_guard<std::mutex> lock(m_Lock);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Reported-refresh V-sync: %llu ticks (%llu woke over 1 ms late); "
                "%llu reports used, %llu rejected, %llu rephases; period %.1f us",
                (unsigned long long)m_Ticks, (unsigned long long)m_LateWakeups,
                (unsigned long long)m_Clock.accepted(), (unsigned long long)m_Clock.rejected(),
                (unsigned long long)m_Clock.rephased(), m_Clock.periodUs());
    if (m_Ticks != 0 && !m_Clock.haveReport()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Reported-refresh V-sync: the compositor reported no refresh times; "
                    "Smooth V-Sync followed a free-running timer");
    }
}

bool PresentTimingVsyncSource::initialize(SDL_Window*, int displayFps)
{
    if (displayFps <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Clock.configure(1000000.0 / displayFps);
    return true;
}

bool PresentTimingVsyncSource::isAsync()
{
    return false;
}

void PresentTimingVsyncSource::observeRefresh(uint64_t refreshUs)
{
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Clock.observe(refreshUs);
}

double PresentTimingVsyncSource::periodUs()
{
    std::lock_guard<std::mutex> lock(m_Lock);
    return m_Clock.periodUs();
}

void PresentTimingVsyncSource::waitForVsync()
{
    const uint64_t nowUs = LiGetMicroseconds();
    uint64_t targetUs;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Clock.seed(nowUs);
        // Never tick twice for one refresh, even if the grid moved back
        const uint64_t afterUs = std::max(nowUs, m_LastTickUs +
                                          static_cast<uint64_t>(m_Clock.periodUs() / 2));
        targetUs = m_Clock.nextRefreshAfter(afterUs);
    }

    const VrrTargetWaitResult result = m_Waiter.waitUntil(targetUs, m_WakeLeadUs);

    // Learn how late the scheduler wakes this thread, like the VRR worker:
    // follow increases at once and let them decay slowly
    if (result.schedulerDelayValid) {
        if (result.schedulerDelayUs > m_WakeLeadUs) {
            m_WakeLeadUs = result.schedulerDelayUs;
        }
        else {
            m_WakeLeadUs -= (m_WakeLeadUs - result.schedulerDelayUs) / 16;
        }
    }
    if (result.finalNowUs > targetUs + 1000) {
        ++m_LateWakeups;
    }
    ++m_Ticks;
    m_LastTickUs = targetUs;
}
