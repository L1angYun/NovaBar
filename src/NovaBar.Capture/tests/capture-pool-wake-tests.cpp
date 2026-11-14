#include "../SceneBackdropSource.h"
#include <cstdio>
using namespace novabar;
namespace wake = novabar::capture_detail;
int checks = 0, failures = 0;
void Check(bool ok, char const *name)
{
    ++checks;
    if (!ok)
        ++failures;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
}
namespace
{
struct CaptureBoundary
{
    inline static std::shared_ptr<wake::CaptureWakeRegistration> Callback;
    inline static bool FailStart = false;
    HWND WindowHandle{};
    uint64_t Serial = 0;
    std::shared_ptr<wake::CaptureWakeRegistration> Registration;
    ~CaptureBoundary()
    {
        Close();
    }
    HWND Window() const
    {
        return WindowHandle;
    }
    uint64_t Generation() const
    {
        return Serial;
    }
    void Start(HWND window)
    {
        Close();
        WindowHandle = window;
    }
    void Start(HWND window, ID3D11Device *, ID3D11DeviceContext *,
               std::shared_ptr<wake::CaptureWakeRegistration> registration)
    {
        Close();
        Registration = std::move(registration);
        Callback = Registration;
        if (FailStart)
            winrt::throw_hresult(E_FAIL);
        WindowHandle = window;
    }
    void Close()
    {
        if (Registration)
            Registration->Close();
        Registration.reset();
        WindowHandle = nullptr;
        Serial = 0;
    }
    bool DrainLatest()
    {
        if (!WindowHandle)
            return false;
        ++Serial;
        return true;
    }
    bool Read(PixelRect region, PixelFrame &out, bool, bool)
    {
        if (!WindowHandle)
            return false;
        out.Bounds = region;
        out.Width = region.Right - region.Left;
        out.Height = region.Bottom - region.Top;
        out.Pixels.assign(size_t(out.Width) * out.Height * 4, 255);
        return true;
    }
};
struct Environment
{
    inline static uint64_t Tick = 100;
    inline static bool Alive = true;
    inline static std::vector<scene_detail::WindowSnapshot> Windows;
    struct Owner
    {
        explicit Owner(DWORD) {}
        bool Alive() const
        {
            return Environment::Alive;
        }
    };
    static uint64_t Now()
    {
        return Tick;
    }
    static bool Valid(scene_detail::WindowSnapshot const &item)
    {
        return std::any_of(Windows.begin(), Windows.end(), [&](auto const &current)
                           { return current.Window == item.Window && current.Process == item.Process; });
    }
    static auto VisibleWindows(PixelRect)
    {
        return Windows;
    }
};
using Pool = BasicSceneCapturePool<CaptureBoundary, Environment>;
scene_detail::WindowSnapshot Item(uintptr_t id, DWORD pid = 7)
{
    scene_detail::WindowSnapshot item;
    item.Window = reinterpret_cast<HWND>(id);
    item.Process = pid;
    item.Rectangle = {0, 0, 10, 8};
    return item;
}
void Notify(std::shared_ptr<wake::CaptureWakeRegistration> const &registration)
{
    if (registration)
        registration->Notify();
}
void Routing()
{
    auto item = Item(1);
    Environment::Windows = {item};
    Environment::Alive = true;
    Environment::Tick = 100;
    std::shared_ptr<wake::CaptureWakeRegistration> old, last;
    HANDLE borrowed{};
    {
        Pool pool;
        borrowed = pool.FrameWakeHandle();
        Check(!!borrowed, "one real auto-reset event belongs to the pool");
        Check(pool.Read(item, item.Rectangle).Ready, "production pool acquires source through fake WGC boundary");
        old = CaptureBoundary::Callback;
        Check(!!old, "production pool passes bounded wake registration to session start");
        Notify(old);
        Check(WaitForSingleObject(borrowed, 0) == WAIT_TIMEOUT,
              "CPU/default pool acquisition preserves disabled event state");
        pool.EnableFrameWake(true);
        Notify(old);
        Check(WaitForSingleObject(borrowed, 0) == WAIT_OBJECT_0, "visible GPU activation enables source callback wake");
        Environment::Alive = false;
        Environment::Tick += 1100;
        pool.Warm(item.Rectangle, {});
        Notify(old);
        Check(WaitForSingleObject(borrowed, 0) == WAIT_TIMEOUT,
              "source process close and eviction disable retained old callback");
        Environment::Alive = true;
        item.Process = 9;
        Environment::Windows = {item};
        Check(pool.Read(item, item.Rectangle).Ready, "same numeric window with new owner starts a fresh session");
        last = CaptureBoundary::Callback;
        Notify(old);
        Check(WaitForSingleObject(borrowed, 0) == WAIT_TIMEOUT, "old callback cannot signal new source ownership");
        Notify(last);
        Check(WaitForSingleObject(borrowed, 0) == WAIT_OBJECT_0,
              "replacement session callback uses same bounded pool event");
        pool.EnableFrameWake(false);
        Notify(last);
        Check(WaitForSingleObject(borrowed, 0) == WAIT_TIMEOUT, "hidden and CPU mode disable frame-triggered wake");
        pool.EnableFrameWake(true);
    }
    Notify(last);
    Check(WaitForSingleObject(borrowed, 0) == WAIT_TIMEOUT,
          "pool teardown revokes sessions and leaves late callbacks inert");
    old.reset();
    last.reset();
    CaptureBoundary::Callback.reset();
    DWORD flags{};
    Check(!GetHandleInformation(borrowed, &flags), "pool event closes after last retained callback lifetime ends");
}
void FailedAndBounded()
{
    Environment::Alive = true;
    Environment::Tick = 4000;
    auto item = Item(44);
    Environment::Windows = {item};
    {
        Pool pool;
        pool.EnableFrameWake(true);
        CaptureBoundary::FailStart = true;
        Check(!pool.Read(item, item.Rectangle).Ready, "failed WGC session start stays unavailable");
        Notify(CaptureBoundary::Callback);
        Check(WaitForSingleObject(pool.FrameWakeHandle(), 0) == WAIT_TIMEOUT,
              "failed session startup disables its captured callback");
        CaptureBoundary::FailStart = false;
        Environment::Tick += 1001;
        Check(pool.Read(item, item.Rectangle).Ready, "bounded retry creates a new session registration");
        Notify(CaptureBoundary::Callback);
        Check(WaitForSingleObject(pool.FrameWakeHandle(), 0) == WAIT_OBJECT_0,
              "successful retry restores source-event wake");
    }
    CaptureBoundary::Callback.reset();
    DWORD before{}, active{}, after{};
    GetProcessHandleCount(GetCurrentProcess(), &before);
    std::vector<std::shared_ptr<wake::CaptureWakeRegistration>> callbacks;
    {
        Pool pool;
        pool.EnableFrameWake(true);
        for (uintptr_t i = 1; i <= 40; ++i)
        {
            auto next = Item(i + 100);
            Environment::Windows.push_back(next);
            ++Environment::Tick;
            Check(pool.Read(next, next.Rectangle).Ready,
                  "source churn remains supported within session eviction bounds");
            callbacks.push_back(CaptureBoundary::Callback);
        }
        GetProcessHandleCount(GetCurrentProcess(), &active);
        Check(active == before + 1, "forty source callbacks and eviction keep exactly one pool event handle");
        Notify(callbacks.front());
        Check(WaitForSingleObject(pool.FrameWakeHandle(), 0) == WAIT_TIMEOUT,
              "evicted session callback cannot wake active replacement captures");
        for (auto const &callback : callbacks)
            Notify(callback);
        Check(WaitForSingleObject(pool.FrameWakeHandle(), 0) == WAIT_OBJECT_0 &&
                  WaitForSingleObject(pool.FrameWakeHandle(), 0) == WAIT_TIMEOUT,
              "many live frame callbacks coalesce instead of queueing unbounded work");
    }
    callbacks.clear();
    CaptureBoundary::Callback.reset();
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(after == before, "source churn and pool destruction balance all event handles");
}
} // namespace
int main()
{
    Routing();
    FailedAndBounded();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
