#include "../CaptureScheduler.h"
#include "../CaptureTiming.h"
#include <cstdio>
#include <vector>

int main()
{
    int checks = 0, failed = 0;
    const auto check = [&](bool ok, const char* label)
    { ++checks; if (!ok) { ++failed; std::printf("FAIL: %s\n", label); } };
    check(std::abs(novabar::CaptureRemainingDelay(100.0,110.25)-5.75)<1e-8,
        "pacing subtracts actual fractional work rather than a zero coarse tick");
    check(novabar::CaptureRemainingDelay(100.0,116.0)==0
        && novabar::CaptureRemainingDelay(100.0,120.0)==0,
        "exhausted work budgets never add an extra sleep");
    check(novabar::CaptureRemainingDelay(100.0,99.0)==0,
        "a regressed clock never creates an oversized sleep");
    {
        novabar::CaptureScheduler scheduler; double now=100.2;
        std::vector<size_t> updated;
        scheduler.Run(1,now,[&]{return now;},[](size_t){return false;},[](size_t){return true;},[](size_t){});
        now=110.2;
        scheduler.Run(2,now,[&]{return now;},[](size_t i){return i==0;},[](size_t){return true;},
            [&](size_t i){updated.push_back(i);if(i==0)now+=15.8;});
        check(updated==std::vector<size_t>({0,1}),"fractional budget remains available after15.8ms of visible work");
        check(scheduler.ShouldWarm(false,1250.1,1234.3),"warming uses fractional elapsed time without integer truncation");
    }
    {
        novabar::CaptureScheduler scheduler;
        double now = 100;
        std::vector<size_t> updated;
        scheduler.Run(3, 100, [&] { return now; }, [](size_t i) { return i == 1; }, [](size_t) { return true; },
            [&](size_t i) { updated.push_back(i); now += i == 1 ? 1 : 40; });
        check(updated == std::vector<size_t>({1,0}), "a heavy hidden target preceding a visible target never delays visible work");
        check(updated.size() == 2, "a pass updates at most one hidden target");
    }
    {
        novabar::CaptureScheduler scheduler;
        double now = 100;
        std::vector<size_t> updated;
        for (int pass = 0; pass < 4; ++pass)
        {
            const double start = now;
            scheduler.Run(3, start, [&] { return now; }, [](size_t) { return true; }, [](size_t) { return false; },
                [&](size_t i) { updated.push_back(i); now += 20; });
        }
        check(updated == std::vector<size_t>({0,1,2,0}), "visible targets rotate fairly when one update exhausts the budget");
        updated.clear();
        scheduler.Run(3, now - 20, [&] { return now; }, [](size_t) { return true; }, [](size_t) { return false; },
            [&](size_t i) { updated.push_back(i); ++now; });
        check(updated == std::vector<size_t>({1}), "one visible update survives a budget exhausted by request discovery");
    }
    {
        novabar::CaptureScheduler scheduler;
        double now = 100;
        std::vector<size_t> updated;
        const auto pass = [&]
        {
            const double start = now;
            scheduler.Run(3, start, [&] { return now; }, [](size_t i) { return i == 0; }, [](size_t) { return true; },
                [&](size_t i) { updated.push_back(i); now += i == 0 ? 20 : 1; });
        };
        pass(); // Visible finishes at120; first hidden finishes at121.
        pass(); // Visible finishes at141; hidden still has20ms of grace.
        pass(); // Visible finishes at161; hidden is now40ms old.
        check(updated == std::vector<size_t>({0,1,0,0,2}), "hidden targets rotate and eventually run under continuous visible budget pressure");
    }
    {
        novabar::CaptureScheduler scheduler;
        double now = 100;
        std::vector<size_t> updated;
        scheduler.Run(4, 100, [&] { return now; }, [](size_t) { return false; }, [](size_t) { return false; },
            [&](size_t i) { updated.push_back(i); });
        check(updated.empty(), "static hidden targets that are not due consume no update slot");
        scheduler.Run(4, 100, [&] { return now; }, [](size_t) { return false; }, [](size_t i) { return i == 2 || i == 3; },
            [&](size_t i) { updated.push_back(i); });
        scheduler.Run(4, 100, [&] { return now; }, [](size_t) { return false; }, [](size_t i) { return i == 2 || i == 3; },
            [&](size_t i) { updated.push_back(i); });
        check(updated == std::vector<size_t>({2,3}), "hidden scanning skips targets that are not due and preserves its own cursor");
        scheduler.Run(0, 100, [&] { return now; }, [](size_t) { return true; }, [](size_t) { return true; },
            [&](size_t i) { updated.push_back(i); });
        check(updated == std::vector<size_t>({2,3}), "an empty target collection performs no callbacks");
    }
    {
        novabar::CaptureScheduler scheduler;
        double now = 100;
        std::vector<size_t> updated;
        scheduler.Run(2, 80, [&] { return now; }, [](size_t) { return false; }, [](size_t) { return false; },
            [&](size_t i) { updated.push_back(i); });
        ++now;
        scheduler.Run(2, 80, [&] { return now; }, [](size_t) { return false; }, [](size_t) { return true; },
            [&](size_t i) { updated.push_back(i); });
        check(updated == std::vector<size_t>({0}), "finding no due hidden work does not postpone a later eligible hidden target");
        now = 132;
        scheduler.Run(2, 100, [&] { return now; }, [](size_t) { return false; }, [](size_t) { return true; },
            [&](size_t i) { updated.push_back(i); });
        check(updated == std::vector<size_t>({0}), "hidden pressure work waits until its32 millisecond boundary");
        now = 133;
        scheduler.Run(2, 100, [&] { return now; }, [](size_t) { return false; }, [](size_t) { return true; },
            [&](size_t i) { updated.push_back(i); });
        check(updated == std::vector<size_t>({0,1}), "hidden pressure work is eligible exactly32 milliseconds after completion");
        now = 150;
        scheduler.Run(1, 150, [&] { return now; }, [](size_t) { return false; }, [](size_t) { return true; },
            [&](size_t i) { updated.push_back(i); });
        check(updated == std::vector<size_t>({0,1,0}), "a smaller target collection safely reuses the hidden cursor");
    }
    {
        novabar::CaptureScheduler scheduler;
        check(!scheduler.ShouldWarm(true, 1000, 1000), "visible panel capture suppresses speculative warming");
        check(scheduler.ShouldWarm(false, 1000, 1000), "idle budget permits due speculative warming");
        scheduler.DidWarm(1000);
        check(!scheduler.ShouldWarm(false, 1249, 1249), "speculative warming waits a full 250 milliseconds");
        check(scheduler.ShouldWarm(false, 1250, 1250), "speculative warming resumes at its 250 millisecond boundary");
        check(scheduler.ShouldWarm(false, 1250, 1235), "speculative warming can use the last millisecond of an available budget");
        check(!scheduler.ShouldWarm(false, 1250, 1234), "speculative warming never exceeds an exhausted 16 millisecond pass budget");
    }
    std::printf("Capture scheduler: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
