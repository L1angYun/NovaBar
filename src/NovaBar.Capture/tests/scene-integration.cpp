#include <cstdio>
#include <thread>
#include "../SceneBackdropSource.h"

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        auto brush = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(window, GWLP_USERDATA)));
        FillRect(dc, &client, brush); DeleteObject(brush); EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int failures = 0;
    const auto check = [&](bool value, const char* name)
    { if (!value) { ++failures; printf("FAIL: %s\n", name); } };
    WNDCLASSW type{}; type.lpfnWndProc = TestWindowProc;
    type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"NovaBarSceneOwnWindowTest";
    RegisterClassW(&type);
    const auto create = [&](int x, int y, int w, int h, COLORREF color, DWORD extra = 0)
    {
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | extra, type.lpszClassName, L"NovaBar isolated scene test",
            WS_POPUP, x, y, w, h, nullptr, nullptr, type.hInstance, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, color); return window;
    };
    HWND lower = create(100, 100, 700, 550, RGB(0, 0, 255));
    HWND middle = create(300, 250, 200, 140, RGB(255, 0, 0), WS_EX_LAYERED);
    HWND panel = create(140, 140, 500, 350, RGB(0, 255, 0));
    SetLayeredWindowAttributes(middle, 0, 128, LWA_ALPHA);
    SetWindowRgn(middle, CreateRoundRectRgn(0, 0, 200, 140, 40, 40), FALSE);
    ShowWindow(lower, SW_SHOWNOACTIVATE); UpdateWindow(lower);
    ShowWindow(middle, SW_SHOWNOACTIVATE); UpdateWindow(middle);
    ShowWindow(panel, SW_SHOWNOACTIVATE); UpdateWindow(panel);
    SetWindowPos(lower, HWND_TOP, 100, 100, 700, 550, SWP_NOACTIVATE);
    SetWindowPos(middle, HWND_TOP, 300, 250, 200, 140, SWP_NOACTIVATE);
    SetWindowPos(panel, HWND_TOP, 140, 140, 500, 350, SWP_NOACTIVATE);
    const auto pump = []
    { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } };
    try
    {
        novabar::SceneBackdropSource source;
        const auto await = [&](novabar::PixelRect bounds, int ms = 2500)
        {
            const auto until = GetTickCount64() + ms;
            novabar::SceneUpdate update;
            do { pump(); update = source.Update(panel, bounds); if (update.Ready) return update;
                 std::this_thread::sleep_for(std::chrono::milliseconds(10)); } while (GetTickCount64() < until);
            printf("pending capture HRESULT=%08lx\n", static_cast<unsigned long>(update.Error)); return update;
        };
        const auto color = [&](int x, int y)
        {
            const auto& frame = source.Frame();
            const size_t at = (static_cast<size_t>(y - frame.Bounds.Top) * frame.Width + x - frame.Bounds.Left) * 4;
            return std::array<unsigned, 4>{frame.Pixels[at], frame.Pixels[at + 1], frame.Pixels[at + 2], frame.Pixels[at + 3]};
        };
        const novabar::PixelRect initialBounds{140, 140, 240, 240};
        check(await(initialBounds).Ready, "WGC captures a real lower window while opaque panel covers it");
        if (!source.Frame().Pixels.empty()) check(color(160, 160) == std::array<unsigned, 4>{255, 0, 0, 255}, "excluded panel green never enters the captured scene");
        // Drain initial frames. Enlarging only the requested crop must work even
        // when the static application's surface produces no further frames.
        for (int i = 0; i < 40; ++i) { pump(); source.Update(panel, initialBounds); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        const novabar::PixelRect large{140, 140, 640, 490};
        const bool larger = await(large).Ready;
        check(larger, "larger crop can reuse retained static source content");
        if (larger)
        {
            const auto mix = color(340, 290);
            printf("layered sample BGRA=%u,%u,%u,%u\n", mix[0], mix[1], mix[2], mix[3]);
            check(mix[0] >= 126 && mix[0] <= 128 && mix[1] == 0 && mix[2] >= 127 && mix[2] <= 129 && mix[3] == 255,
                  "global layered-window alpha is applied once over actual lower pixels");
            check(color(300, 250) == std::array<unsigned, 4>{255, 0, 0, 255}, "rounded window region leaves desktop pixels visible");
        }
        SetWindowPos(middle, HWND_TOP, 360, 250, 200, 140, SWP_NOACTIVATE);
        SetWindowPos(panel, HWND_TOP, 140, 140, 500, 350, SWP_NOACTIVATE);
        const bool moved = await(large).Ready; check(moved, "moved underlying window refreshes valid scene geometry");
        if (moved) check(color(330, 290) == std::array<unsigned, 4>{255, 0, 0, 255}, "old location exposes lower window after movement");
        SetWindowPos(lower, middle, 100, 100, 740, 570, SWP_NOACTIVATE);
        const bool resized = await(large).Ready; check(resized, "resized underlying window receives a newly sized capture surface");
        if (resized) check(color(160, 160) == std::array<unsigned, 4>{255, 0, 0, 255}, "resize retains actual lower content without an opaque replacement");
    }
    catch (winrt::hresult_error const& error)
    { printf("capture exception=%08lx %ls\n", static_cast<unsigned long>(error.code()), error.message().c_str()); ++failures; }
    DestroyWindow(panel); DestroyWindow(middle); DestroyWindow(lower);
    winrt::uninit_apartment();
    printf("scene integration: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
