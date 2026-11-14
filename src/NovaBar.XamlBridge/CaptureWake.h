#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <memory>
#include <windows.h>

namespace novabar::capture_detail
{
// One unnamed auto-reset event per capture pool. A callback registration
// keeps the event object alive, so late callbacks never use a recycled HANDLE.
struct CaptureWakeState
{
    HANDLE Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> Enabled{false};
    ~CaptureWakeState()
    {
        if (Event)
            CloseHandle(Event);
    }
    CaptureWakeState() = default;
    CaptureWakeState(CaptureWakeState const &) = delete;
    CaptureWakeState &operator=(CaptureWakeState const &) = delete;
};
class CaptureFrameWake;
class CaptureWakeRegistration
{
    friend class CaptureFrameWake;
    std::shared_ptr<CaptureWakeState> m_state;
    std::atomic<bool> m_active{true};
    explicit CaptureWakeRegistration(std::shared_ptr<CaptureWakeState> state) : m_state(std::move(state)) {}

  public:
    CaptureWakeRegistration(CaptureWakeRegistration const &) = delete;
    CaptureWakeRegistration &operator=(CaptureWakeRegistration const &) = delete;
    // The FrameArrived callback does only these trivial atomic loads and
    // SetEvent. Frames, WGC sessions and D3D/context work remain on the MTA.
    void Notify() const noexcept
    {
        if (m_active.load(std::memory_order_acquire) && m_state->Enabled.load(std::memory_order_acquire) &&
            m_state->Event)
            SetEvent(m_state->Event);
    }
    void Close() noexcept
    {
        m_active.store(false, std::memory_order_release);
    }
};
class CaptureFrameWake
{
    std::shared_ptr<CaptureWakeState> m_state = std::make_shared<CaptureWakeState>();

  public:
    CaptureFrameWake() = default;
    CaptureFrameWake(CaptureFrameWake const &) = delete;
    CaptureFrameWake &operator=(CaptureFrameWake const &) = delete;
    ~CaptureFrameWake()
    {
        Enable(false);
    }
    HANDLE Handle() const noexcept
    {
        return m_state->Event;
    }
    void Enable(bool enabled) noexcept
    {
        m_state->Enabled.store(enabled, std::memory_order_release);
        if (!enabled && m_state->Event)
            ResetEvent(m_state->Event);
    }
    std::shared_ptr<CaptureWakeRegistration> Subscribe()
    {
        return std::shared_ptr<CaptureWakeRegistration>(new CaptureWakeRegistration(m_state));
    }
};
} // namespace novabar::capture_detail
