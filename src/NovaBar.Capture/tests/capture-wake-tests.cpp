#include "../../NovaBar.XamlBridge/CaptureWake.h"
#include "../CaptureTiming.h"
#include <atomic>
#include <cstdio>
#include <limits>
#include <thread>
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
void Plans()
{
    const auto start = CapturePlanWait(100, 100, 16, true);
    Check(std::abs(start.MinimumDelay - 1000.0 / 240.0) < 1e-9 && start.DeadlineDelay == 16 && start.WakeEnabled,
          "frame wake has at least4ms and at most240cycles per second");
    auto partial = CapturePlanWait(100, 102.5, 16, true);
    Check(std::abs(partial.MinimumDelay - (1000.0 / 240.0 - 2.5)) < 1e-9 && partial.DeadlineDelay == 13.5,
          "work elapsed is subtracted from the minimum coalescing floor");
    auto floor = CapturePlanWait(100, 105, 16, true);
    Check(floor.MinimumDelay == 0 && floor.DeadlineDelay == 11 && floor.WakeEnabled,
          "after minimum cycle an event can wake before the16ms fallback");
    auto cpu = CapturePlanWait(100, 102.5, 16, false);
    Check(cpu.MinimumDelay == 0 && cpu.DeadlineDelay == 13.5 && !cpu.WakeEnabled,
          "CPU hidden and idle pacing retain the existing16ms deadline");
    for (double now : {116., 120.})
    {
        auto done = CapturePlanWait(100, now, 16, true);
        Check(done.MinimumDelay == 0 && done.DeadlineDelay == 0 && !done.WakeEnabled,
              "an exhausted pass never waits for a source event");
    }
    auto bad = CapturePlanWait(100, 99, 16, true);
    Check(bad.MinimumDelay == 0 && bad.DeadlineDelay == 0 && !bad.WakeEnabled,
          "clock regression cannot create an unbounded wake wait");
    auto nan = CapturePlanWait(100, std::numeric_limits<double>::quiet_NaN(), 16, true);
    Check(nan.DeadlineDelay == 0 && !nan.WakeEnabled, "nonfinite timing cannot arm an unbounded wake wait");
}
void Lifetime()
{
    wake::CaptureFrameWake owner;
    Check(owner.Handle() != nullptr, "pool owns one unnamed auto-reset event");
    auto first = owner.Subscribe();
    first->Notify();
    Check(WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT, "disabled pool does not signal callbacks");
    owner.Enable(true);
    for (unsigned i = 0; i < 1000; ++i)
        first->Notify();
    Check(WaitForSingleObject(owner.Handle(), 0) == WAIT_OBJECT_0 &&
              WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT,
          "frame flood coalesces into one auto-reset wake");
    first->Close();
    first->Notify();
    Check(WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT,
          "closed session callback cannot signal retained pool");
    auto replacement = owner.Subscribe();
    replacement->Notify();
    Check(WaitForSingleObject(owner.Handle(), 0) == WAIT_OBJECT_0,
          "new session registration survives predecessor close");
    replacement->Notify();
    owner.Enable(false);
    Check(WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT, "leaving visible GPU mode clears pending wake");
    replacement->Notify();
    Check(WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT, "hidden callback cannot wake the broker");
    std::shared_ptr<wake::CaptureWakeRegistration> late;
    HANDLE borrowed{};
    {
        wake::CaptureFrameWake temporary;
        temporary.Enable(true);
        borrowed = temporary.Handle();
        late = temporary.Subscribe();
    }
    DWORD flags{};
    Check(GetHandleInformation(borrowed, &flags) != FALSE,
          "in-flight registration retains safe event ownership after pool teardown");
    late->Notify();
    Check(WaitForSingleObject(borrowed, 0) == WAIT_TIMEOUT,
          "pool destruction disables late callbacks before releasing its ownership");
    late.reset();
    Check(GetHandleInformation(borrowed, &flags) == FALSE, "last callback owner closes its own retained event handle");
}
void ActualWait()
{
    wake::CaptureFrameWake owner;
    owner.Enable(true);
    auto signal = owner.Subscribe();
    CapturePacer pacer;
    signal->Notify();
    const double began = CaptureNowMilliseconds();
    const auto pendingResult = pacer.Wait(began, 16, owner.Handle());
    const double elapsed = CaptureNowMilliseconds() - began;
    Check(elapsed >= 4.0, "pending frame cannot bypass the minimum broker work cycle");
    Check(pendingResult == CapturePacerResult::Frame && WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT,
          "minimum timer phase preserves pending signal until event wait consumes it");
    signal->Notify();
    const double legacy = CaptureNowMilliseconds();
    pacer.Wait(legacy);
    Check(CaptureNowMilliseconds() - legacy >= 15.0 && WaitForSingleObject(owner.Handle(), 0) == WAIT_OBJECT_0,
          "legacy default wait retains16ms behavior and never consumes unrelated frame signal");
    const double fallback = CaptureNowMilliseconds();
    pacer.Wait(fallback, 16, owner.Handle());
    Check(CaptureNowMilliseconds() - fallback >= 15.0, "no arriving frame still completes through the16ms deadline");
    std::thread notify(
        [signal]
        {
            CapturePacer producer;
            producer.Wait(CaptureNowMilliseconds(), 6);
            signal->Notify();
        });
    const double future = CaptureNowMilliseconds();
    pacer.Wait(future, 16, owner.Handle());
    notify.join();
    Check(CaptureNowMilliseconds() - future >= 4.0 && WaitForSingleObject(owner.Handle(), 0) == WAIT_TIMEOUT,
          "own producer event wakes after coalescing without a callback GPU/context operation");
}
void CallbackRace()
{
    std::thread([] {}).join();
    DWORD before{}, after{};
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (unsigned i = 0; i < 48; ++i)
    {
        auto owner = std::make_unique<wake::CaptureFrameWake>();
        owner->Enable(true);
        auto registration = owner->Subscribe();
        std::atomic<bool> stop = false;
        std::thread producer(
            [&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    registration->Notify();
                    std::this_thread::yield();
                }
            });
        registration->Close();
        owner.reset();
        stop.store(true, std::memory_order_relaxed);
        producer.join();
        registration.reset();
    }
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(before == after, "concurrent session close and pool teardown leave a bounded balanced event count");
}
int main()
{
    Plans();
    Lifetime();
    ActualWait();
    CallbackRace();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
