#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace novabar
{
    inline double CaptureNowMilliseconds()
    {
        static const double scale = [] { LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
            return 1000.0 / frequency.QuadPart; }();
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now); return now.QuadPart * scale;
    }
    inline double CaptureRemainingDelay(double began, double now, double budget = 16)
    {
        if (!std::isfinite(began) || !std::isfinite(now) || !std::isfinite(budget) || now < began) return 0;
        return std::max(0.0, budget - (now - began));
    }
    inline constexpr double CaptureWakeCycleMilliseconds = 1000.0 / 240.0;
    struct CaptureWakeWaitPlan
    {
        double MinimumDelay = 0, DeadlineDelay = 0;
        bool WakeEnabled = false;
    };
    inline CaptureWakeWaitPlan CapturePlanWait(double began, double now, double budget = 16, bool wakeEnabled = false)
    {
        const double remaining = CaptureRemainingDelay(began, now, budget);
        if (remaining <= 0) return {};
        return {wakeEnabled ? CaptureRemainingDelay(began, now, std::min(budget, CaptureWakeCycleMilliseconds)) : 0,
            remaining, wakeEnabled};
    }
    enum class CapturePacerResult { Deadline, Frame };
    class CapturePacer
    {
        HANDLE m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002,
            TIMER_MODIFY_STATE | SYNCHRONIZE); // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        bool Arm(double milliseconds) noexcept
        {
            if (!m_timer) return false;
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(milliseconds * 10000));
            return !!SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
        }
        void Delay(double milliseconds)
        {
            if (milliseconds <= 0) return;
            const double began = CaptureNowMilliseconds();
            // This is our own successfully armed relative high-resolution
            // timer, preserving the original default pacing and floor precision.
            if (Arm(milliseconds) && WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0) return;
            Sleep(static_cast<DWORD>(std::ceil(CaptureRemainingDelay(began, CaptureNowMilliseconds(), milliseconds))));
        }
    public:
        CapturePacer() = default;
        CapturePacer(CapturePacer const&) = delete;
        CapturePacer& operator=(CapturePacer const&) = delete;
        ~CapturePacer() { if (m_timer) CloseHandle(m_timer); }
        CapturePacerResult Wait(double began, double budget = 16, HANDLE frameWake = nullptr)
        {
            const bool wakeEnabled = frameWake && frameWake != INVALID_HANDLE_VALUE;
            const auto plan = CapturePlanWait(began, CaptureNowMilliseconds(), budget, wakeEnabled);
            if (!plan.DeadlineDelay) return CapturePacerResult::Deadline;
            // Do not wait on/consume the source event until the minimum cycle
            // expires. Auto-reset events preserve/coalesce frames during this
            // timer-only phase instead of losing a pending notification.
            if (plan.MinimumDelay > 0) Delay(plan.MinimumDelay);
            const double remaining = CaptureRemainingDelay(began, CaptureNowMilliseconds(), budget);
            if (remaining <= 0)
                return wakeEnabled && WaitForSingleObject(frameWake, 0) == WAIT_OBJECT_0
                    ? CapturePacerResult::Frame : CapturePacerResult::Deadline;
            if (!wakeEnabled) { Delay(remaining); return CapturePacerResult::Deadline; }
            if (Arm(remaining))
            {
                const HANDLE handles[]{frameWake, m_timer};
                // The armed timer supplies the16ms deadline; the event is
                // never waited on alone without a bounded timeout.
                const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) return CapturePacerResult::Frame;
                if (result == WAIT_OBJECT_0 + 1 || result == WAIT_TIMEOUT) return CapturePacerResult::Deadline;
            }
            else
            {
                const auto result = WaitForSingleObject(frameWake, static_cast<DWORD>(std::ceil(remaining)));
                if (result == WAIT_OBJECT_0) return CapturePacerResult::Frame;
                if (result == WAIT_TIMEOUT) return CapturePacerResult::Deadline;
            }
            // Timer/handle failures retain the existing bounded deadline path.
            Delay(CaptureRemainingDelay(began, CaptureNowMilliseconds(), budget));
            return CapturePacerResult::Deadline;
        }
    };
    struct CapturePhaseTiming
    {
        uint64_t Count = 0;
        double Last = 0, Sum = 0, Max = 0;
        void Add(double duration) { ++Count; Last = duration; Sum += duration; Max = std::max(Max, duration); }
        double Average() const { return Count ? Sum / Count : 0; }
    };
    class CapturePhaseScope
    {
        CapturePhaseTiming* m_timing = nullptr;
        double m_began = 0;
    public:
        CapturePhaseScope(bool enabled, CapturePhaseTiming& timing) : m_timing(enabled ? &timing : nullptr)
        { if (m_timing) m_began = CaptureNowMilliseconds(); }
        ~CapturePhaseScope() { if (m_timing) m_timing->Add(CaptureNowMilliseconds() - m_began); }
    };
    template<class Call>
    auto CaptureMeasure(bool enabled, CapturePhaseTiming& timing, Call&& call)
    {
        CapturePhaseScope scope(enabled, timing);
        return std::forward<Call>(call)();
    }
}
