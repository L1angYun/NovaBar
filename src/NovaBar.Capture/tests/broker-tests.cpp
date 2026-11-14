#include "../CaptureFrameReader.h"
#include "../CaptureFrameWriter.h"
#include <cstdio>
#include <thread>

namespace
{
    namespace ipc = novabar::capture;
    int failures = 0;
    void Check(bool value, char const* label)
    { printf("%s: %s\n", value ? "PASS" : "FAIL", label); if (!value) ++failures; }
    ipc::Request Request(uint64_t revision = 1)
    {
        ipc::Request r;
        r.Pid = GetCurrentProcessId(); r.TargetId = 42; r.Revision = revision;
        r.HeartbeatTick = GetTickCount64(); r.CoordinateHwnd = 1; r.TopLevelHwnd = 1;
        r.Bounds = {0, 0, 16, 8}; r.Material = {0, 0, 0, 0, 0, 0}; r.Radius = 0; r.Specular = 0;
        return r;
    }
    LRESULT CALLBACK PaintWindow(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            const bool foreground = GetWindowLongPtrW(window, GWLP_USERDATA) == 1;
            HBRUSH brush = CreateSolidBrush(foreground ? RGB(255, 0, 255) : RGB(220, 20, 10));
            FillRect(dc, &paint.rcPaint, brush); DeleteObject(brush); EndPaint(window, &paint); return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    void Pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    void TestRealBroker(std::wstring const& dir, std::wstring const& executable)
    {
        WNDCLASSW wc{}; wc.lpfnWndProc = PaintWindow; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NovaBar.Capture.ControlledFixture";
        RegisterClassW(&wc);
        RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        // At 150% DPI this lies beyond a DPI-unaware broker's virtualized
        // desktop width, so success also verifies production physical pixels.
        const int x = std::max<int>(work.left + 32, work.right - 272), y = work.top + 32;
        printf("INFO: physical desktop=%dx%d fixture-x=%d\n", GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), x);
        HWND below = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName,
            L"NovaBar capture source fixture", WS_POPUP, x, y, 240, 160, nullptr, nullptr, wc.hInstance, nullptr);
        HWND target = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName,
            L"NovaBar capture excluded fixture", WS_POPUP, x + 40, y + 40, 80, 60, nullptr, nullptr, wc.hInstance, nullptr);
        Check(below && target, "create only controlled source and excluded fixture windows");
        if (!below || !target) { if (below) DestroyWindow(below); if (target) DestroyWindow(target); return; }
        SetWindowLongPtrW(target, GWLP_USERDATA, 1);
        ShowWindow(below, SW_SHOWNOACTIVATE); UpdateWindow(below);
        ShowWindow(target, SW_SHOWNOACTIVATE); UpdateWindow(target);
        Pump();
        auto request = Request(); request.TargetId = 99;
        request.CoordinateHwnd = request.TopLevelHwnd = reinterpret_cast<uint64_t>(target);
        request.Bounds = {x + 40, y + 40, x + 120, y + 100};
        Check(ipc::WriteRequest(dir, request), "write live controlled-window request");
        std::wstring command = L"\"" + executable + L"\" --once --parent " + std::to_wstring(GetCurrentProcessId())
            + L" --watch-dir \"" + dir + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        const bool launched = !!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
        Check(launched, "launch --once broker fixture with parent monitoring");
        bool received = false, unavailable = false;
        HRESULT lastError = S_OK;
        if (launched)
        {
            CloseHandle(child.hThread);
            ipc::FrameReader reader;
            const uint64_t start = GetTickCount64();
            while (GetTickCount64() - start < 2500 && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT)
            {
                Pump(); request.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, request);
                ipc::Frame frame;
                if (reader.ReadLatest(dir, request.Pid, request.TargetId, request.Revision, frame))
                {
                    const size_t at = (static_cast<size_t>(frame.Height / 2) * frame.Width + frame.Width / 2) * 4;
                    const bool correct = frame.Pixels[at + 2] > 180 && frame.Pixels[at + 1] < 60 && frame.Pixels[at] < 60;
                    printf("INFO: broker center BGRA=%u,%u,%u,%u\n", frame.Pixels[at], frame.Pixels[at+1], frame.Pixels[at+2], frame.Pixels[at+3]);
                    Check(correct, "genuine captured lower window excludes magenta target pixels");
                    received = true; break;
                }
                unavailable = unavailable || frame.Status == ipc::Status::Unavailable; lastError = frame.Error;
                Sleep(15);
            }
            if (!received)
                printf("INFO: capture readiness absent; explicit unavailable=%d, HRESULT=0x%08lX\n", unavailable, static_cast<unsigned long>(lastError));
            Check(received, "controlled-window broker produces genuine current-revision pixels");
            reader.Close();
            Check(WaitForSingleObject(child.hProcess, 3000) == WAIT_OBJECT_0, "--once broker exits without persistent worker");
            DWORD exit = 1; GetExitCodeProcess(child.hProcess, &exit);
            Check(exit == 0, "--once broker exits successfully");
            CloseHandle(child.hProcess);
        }
        ipc::CancelRequest(dir, request.Pid, request.TargetId);
        DestroyWindow(target); DestroyWindow(below);
    }
}
int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) return 2;
    const std::wstring dir = argv[1];
    std::filesystem::create_directories(dir);
    auto request = Request();
    Check(ipc::ProtocolVersion == 2 && sizeof(ipc::Request) == 176,
        "motion transport has an isolated version and explicit output geometry");
    auto warmRequest = request;
    warmRequest.Visible = 0; warmRequest.Flags = 2;
    Check(ipc::detail::ValidRequest(warmRequest, false),
        "registered hidden panel can keep its capture source warm");
    warmRequest.Kind = ipc::Kind::Taskbar;
    Check(!ipc::detail::ValidRequest(warmRequest, false),
        "hidden panel warming cannot be requested by a taskbar client");
    auto canvasRequest = Request();
    canvasRequest.Flags = 2 | 4; canvasRequest.OutputWidth = 32; canvasRequest.OutputHeight = 16;
    canvasRequest.Bounds = {100, 200, 140, 220}; canvasRequest.Scale = 1.5;
    canvasRequest.Reserved2 = static_cast<uint16_t>(-10) | (static_cast<uint32_t>(static_cast<uint16_t>(-5)) << 16);
    Check(ipc::detail::ValidRequest(canvasRequest, false),
        "panel can register stable world canvas independently of animated material bounds");
    const auto canvasBounds = ipc::detail::CanvasBounds(canvasRequest);
    Check(canvasBounds.Left == 90 && canvasBounds.Top == 195 && canvasBounds.Right == 138 && canvasBounds.Bottom == 219,
        "signed canvas offset and explicit allocation preserve physical world coordinates");
    {
        auto optics = Request(84); optics.TargetId = 181;
        optics.Flags = ipc::HostClipsCorners | ipc::WorldCanvas | 8;
        optics.MotionSequence = 91;
        Check(ipc::detail::SetCanvasBounds(optics, {-8, -8, 24, 16})
            && ipc::detail::ValidRequest(optics, false),
            "current client optics explicitly negotiates a clipped panel world canvas");
        for (const auto missing : {ipc::HostClipsCorners, ipc::WorldCanvas})
        {
            auto invalid = optics; invalid.Flags &= ~missing;
            Check(!ipc::detail::ValidRequest(invalid, false),
                "client optics cannot omit its native clip or world canvas contract");
        }
        auto invalid = optics; invalid.Kind = ipc::Kind::Taskbar;
        Check(!ipc::detail::ValidRequest(invalid, false), "taskbar cannot request client panel optics");
        const auto canvas = ipc::detail::CanvasBounds(optics);
        uint32_t width = 0, height = 0; ipc::detail::Dimensions(optics, width, height);
        std::vector<uint8_t> base(static_cast<size_t>(width) * height * 4, 79);
        ipc::detail::FrameWriter baseWriter;
        ipc::FrameReader baseReader;
        ipc::Frame baseFrame;
        ipc::detail::WriteSmall(ipc::detail::Path(dir, optics.Pid, optics.TargetId, L".request"), &optics, sizeof(optics));
        Check(baseWriter.Ensure(dir, optics.Pid, optics.TargetId, base.size())
            && baseWriter.Publish(84, ipc::Status::Ready, S_OK, width, height, base, true, {}, 0, canvas),
            "prepare a source canvas with no broker lens pose");
        auto* baseView = baseWriter.ViewForTest();
        auto& baseSlot = baseView->Slots[baseView->ActiveSlot];
        // Offset 28 is the negotiated payload discriminator, formerly padding.
        auto* discriminator = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&baseSlot) + 28);
        *discriminator = 1;
        Check(baseReader.ReadLatest(dir, optics.Pid, optics.TargetId, 84, baseFrame)
            && baseFrame.MotionSequence == 0 && baseFrame.RenderedBounds.Right == 0 && baseFrame.Pixels == base,
            "base payload carries a genuine canvas without claiming broker-rendered lens geometry");
        const auto baseSerial = baseFrame.Serial;
        Check(baseWriter.Publish(84, ipc::Status::Ready, S_OK, width, height, base, false, {}, 0, canvas, ipc::BaseCanvasPayload)
            && !baseReader.ReadLatest(dir, optics.Pid, optics.TargetId, 84, baseFrame)
            && baseFrame.Status == ipc::Status::Ready && baseFrame.PayloadFlags == ipc::BaseCanvasPayload
            && baseFrame.Serial == baseSerial && baseFrame.Pixels.empty(),
            "base heartbeat retains its discriminator without duplicating immutable source pixels");
        baseReader.Close();
        Check(baseReader.ReadLatest(dir, optics.Pid, optics.TargetId, 84, baseFrame) && baseFrame.Pixels == base,
            "a withdrawn base presentation can rehydrate the same source serial");
        Check(!baseWriter.Publish(84, ipc::Status::Ready, S_OK, width, height, base, true,
            optics.Bounds, 0, canvas, ipc::BaseCanvasPayload)
            && !baseWriter.Publish(84, ipc::Status::Ready, S_OK, width, height, base, true,
            {}, 91, canvas, ipc::BaseCanvasPayload),
            "base writer cannot invent a rendered material rectangle or lens motion");
        baseReader.Close(); *discriminator = 0;
        Check(!baseReader.ReadLatest(dir, optics.Pid, optics.TargetId, 84, baseFrame)
            && baseFrame.Status == ipc::Status::Unavailable,
            "new client refuses an unacknowledged baked canvas from an older broker");
        auto legacy = optics; legacy.Flags &= ~8u; legacy.MotionSequence = 0;
        ipc::WriteRequest(dir, legacy); *discriminator = 1;
        Check(!baseReader.ReadLatest(dir, legacy.Pid, legacy.TargetId, 84, baseFrame)
            && baseFrame.Status == ipc::Status::Unavailable,
            "legacy client cannot treat a base payload as a finished refractive image");
        *discriminator = 2;
        Check(!baseReader.ReadLatest(dir, legacy.Pid, legacy.TargetId, 84, baseFrame)
            && baseFrame.Status == ipc::Status::Unavailable,
            "unknown frame payload types cannot enter presentation");
        Check(baseWriter.Publish(84, ipc::Status::Unavailable, E_ACCESSDENIED, 0, 0, {}, true,
            {}, 0, {}, ipc::BaseCanvasPayload)
            && !baseReader.ReadLatest(dir, legacy.Pid, legacy.TargetId, 84, baseFrame)
            && baseFrame.Status == ipc::Status::Unavailable && baseFrame.Error == E_ACCESSDENIED
            && baseFrame.PayloadFlags == 0,
            "source failure carries no payload and remains immediate in both modes");
        baseReader.Close(); baseWriter.Close(); ipc::CancelRequest(dir, optics.Pid, optics.TargetId);
    }
    {
        canvasRequest.TargetId = 177; canvasRequest.Revision = 23;
        canvasRequest.MotionSequence = 1; canvasRequest.Visible = 0;
        std::vector<uint8_t> canvasPixels(32 * 16 * 4, 91);
        ipc::detail::FrameWriter canvasWriter;
        ipc::FrameReader canvasReader;
        ipc::Frame canvasFrame;
        Check(ipc::WriteRequest(dir, canvasRequest)
            && canvasWriter.Ensure(dir, canvasRequest.Pid, canvasRequest.TargetId, canvasPixels.size())
            && canvasWriter.Publish(23, ipc::Status::Ready, S_OK, 32, 16, canvasPixels,
                true, canvasRequest.Bounds, 1, canvasBounds)
            && canvasReader.ReadLatest(dir, canvasRequest.Pid, canvasRequest.TargetId, 23, canvasFrame)
            && canvasFrame.BitmapBounds.Left == 90 && canvasFrame.BitmapBounds.Right == 138
            && canvasFrame.RenderedBounds.Left == 100 && canvasFrame.Pixels == canvasPixels,
            "hidden prepared canvas transports full background separately from the refractive material rectangle");
        canvasRequest.Bounds.Left += 5; canvasRequest.Bounds.Right += 5; canvasRequest.MotionSequence = 2;
        Check(ipc::detail::SetCanvasBounds(canvasRequest, canvasBounds)
            && ipc::detail::CanvasBounds(canvasRequest).Left == 90
            && ipc::detail::CanvasBounds(canvasRequest).Right == 138,
            "animation updates material position without moving the world canvas");
        canvasRequest.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, canvasRequest);
        canvasReader.Close();
        Check(canvasReader.ReadLatest(dir, canvasRequest.Pid, canvasRequest.TargetId, 23, canvasFrame)
            && canvasFrame.MotionSequence == 1 && canvasFrame.BitmapBounds.Left == 90,
            "compatible older motion keeps its explicit stationary bitmap coverage");
        Check(canvasWriter.Publish(23, ipc::Status::Ready, S_OK, 32, 16, canvasPixels,
                true, canvasRequest.Bounds, 2, {91, 195, 139, 219})
            && !canvasReader.ReadLatest(dir, canvasRequest.Pid, canvasRequest.TargetId, 23, canvasFrame),
            "canvas pixels from another screen region cannot pass allocation-only matching");
        Check(!ipc::detail::SetCanvasBounds(canvasRequest, {-40000, 0, -39900, 100}),
            "unrepresentable world offset is rejected instead of wrapping to another monitor");
        canvasWriter.Close(); canvasReader.Close(); ipc::CancelRequest(dir, canvasRequest.Pid, canvasRequest.TargetId);
    }
    {
        auto moving = Request(9);
        moving.TargetId = 144; moving.Flags = 2; moving.Visible = 0;
        moving.OutputWidth = 16; moving.OutputHeight = 8;
        moving.Bounds = {100, 200, 140, 220}; moving.MotionSequence = 10;
        std::vector<uint8_t> movingPixels(16 * 8 * 4, 71);
        ipc::detail::FrameWriter movingWriter;
        ipc::FrameReader movingReader;
        ipc::Frame movingFrame;
        Check(ipc::WriteRequest(dir, moving)
            && movingWriter.Ensure(dir, moving.Pid, moving.TargetId, movingPixels.size())
            && movingWriter.Publish(9, ipc::Status::Ready, S_OK, 16, 8, movingPixels,
                true, moving.Bounds, moving.MotionSequence)
            && movingReader.ReadLatest(dir, moving.Pid, moving.TargetId, 9, movingFrame)
            && movingFrame.MotionSequence == 10 && movingFrame.RenderedBounds.Left == 100
            && movingFrame.RenderedBounds.Right == 140 && movingFrame.Pixels == movingPixels,
            "hidden warm panel transports real pixels with exact rendered motion geometry");
        moving.Bounds = {112, 200, 144, 216}; moving.MotionSequence = 11;
        moving.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, moving);
        movingReader.Close();
        Check(movingReader.ReadLatest(dir, moving.Pid, moving.TargetId, 9, movingFrame)
            && movingFrame.Width == 16 && movingFrame.Height == 8
            && movingFrame.MotionSequence == 10 && movingFrame.RenderedBounds.Left == 100,
            "moving screen bounds retain stable layout allocation and identify bounded prior motion");
        Check(movingWriter.Publish(9, ipc::Status::Ready, S_OK, 16, 8, movingPixels,
                true, moving.Bounds, 12)
            && !movingReader.ReadLatest(dir, moving.Pid, moving.TargetId, 9, movingFrame),
            "reader rejects a motion result newer than the registered request");
        Check(movingWriter.Publish(9, ipc::Status::Ready, S_OK, 16, 8, movingPixels,
                true, {}, 11)
            && !movingReader.ReadLatest(dir, moving.Pid, moving.TargetId, 9, movingFrame),
            "motion frame without a physical sample rectangle is rejected");
        moving.Revision = 10; moving.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, moving);
        Check(movingWriter.Publish(9, ipc::Status::Ready, S_OK, 16, 8, movingPixels,
                true, moving.Bounds, 11)
            && !movingReader.ReadLatest(dir, moving.Pid, moving.TargetId, 10, movingFrame),
            "motion compatibility never admits a different material epoch");
        moving.OutputHeight = 0;
        Check(!ipc::detail::ValidRequest(moving, false),
            "explicit material allocation requires both bounded dimensions");
        moving.OutputWidth = 0; moving.OutputHeight = 0; moving.Bounds = {0, 0, 16, 8};
        moving.Flags = 0; moving.Visible = 0; moving.Revision = 9;
        moving.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, moving);
        Check(!movingReader.ReadLatest(dir, moving.Pid, moving.TargetId, 9, movingFrame),
            "ordinary hidden clients cannot consume prepared panel frames");
        movingWriter.Close(); movingReader.Close(); ipc::CancelRequest(dir, moving.Pid, moving.TargetId);
    }
    Check(ipc::WriteRequest(dir, request), "atomic binary request write");
    ipc::Request loaded{};
    Check(ipc::detail::ReadSmall(ipc::detail::Path(dir, request.Pid, request.TargetId, L".request"), &loaded, sizeof(loaded))
        && loaded.Revision == request.Revision, "read complete request record");
    ipc::detail::FrameWriter writer;
    Check(writer.Ensure(dir, request.Pid, request.TargetId, 16 * 8 * 4), "temporary delete-on-close unnamed file mapping");
    const std::wstring original = writer.MappingNameForTest();
    const std::wstring backing = (std::filesystem::path(dir) / original).wstring();
    std::vector<uint8_t> pixels(16 * 8 * 4, 123);
    Check(writer.Publish(1, ipc::Status::Ready, S_OK, 16, 8, pixels), "publish guarded double-buffer frame");
    ipc::FrameReader reader;
    ipc::Frame frame;
    Check(reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Width == 16 && frame.Height == 8 && frame.Stride == 64 && frame.Pixels == pixels,
        "readonly reader copies stable current geometry");
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Status == ipc::Status::Ready && frame.Serial != 0 && frame.Pixels.empty(),
        "repeated serial returns metadata without claiming new pixels");
    const uint64_t staticSerial = frame.Serial;
    Check(writer.Publish(1, ipc::Status::Ready, S_OK, 16, 8, pixels, false)
        && !reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Status == ipc::Status::Ready && frame.Serial == staticSerial && ipc::detail::Fresh(frame.PublishedTick),
        "unchanged static scene renews guarded publication heartbeat with same serial");
    writer.ViewForTest()->Slots[writer.ViewForTest()->ActiveSlot].PublishedTick = GetTickCount64() - 3000;
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Pixels.empty() && frame.Error == HRESULT_FROM_WIN32(WAIT_TIMEOUT),
        "paused static source expires without exposing retained stale pixels");
    Check(writer.Publish(1, ipc::Status::Ready, S_OK, 16, 8, pixels, false)
        && !reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Status == ipc::Status::Ready && frame.Serial == staticSerial && frame.Pixels.empty(),
        "resumed static heartbeat does not invent a changed scene serial");
    reader.Close();
    Check(reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Serial == staticSerial && frame.Pixels == pixels,
        "discarding an expired presentation replays same static serial after resume");
    Check(writer.Publish(1, ipc::Status::Unavailable, E_ACCESSDENIED, 0, 0, {}), "publish unavailable status without pixels");
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 1, frame)
        && frame.Status == ipc::Status::Unavailable && frame.Error == E_ACCESSDENIED && frame.Pixels.empty(),
        "reader observes unavailable status change without new pixels");
    request.Revision = 2; request.Bounds.Right = 8; request.HeartbeatTick = GetTickCount64();
    Check(ipc::WriteRequest(dir, request), "publish resized geometry revision");
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame) && frame.Pixels.empty(),
        "reader never returns old revision geometry");
    Check(writer.Ensure(dir, request.Pid, request.TargetId, 8 * 8 * 4) && writer.MappingNameForTest() == original,
        "geometry shrink and revision reuse immutable mapping capacity");
    pixels.assign(8 * 8 * 4, 57);
    Check(writer.Publish(2, ipc::Status::Ready, S_OK, 8, 8, pixels)
        && reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame) && frame.Pixels == pixels,
        "new revision publishes new geometry in existing mapping");
    auto* view = writer.ViewForTest();
    auto& slot = view->Slots[view->ActiveSlot];
    InterlockedIncrement64(&slot.Sequence);
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame) && frame.Pixels.empty(), "odd sequence rejects in-progress frame");
    InterlockedIncrement64(&slot.Sequence);
    slot.Stride = UINT32_MAX;
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame), "oversized stride cannot read outside mapped slot");
    slot.Stride = 32;
    slot.PublishedTick = GetTickCount64() - 2000;
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame), "expired published heartbeat rejects stale pixels");
    request.HeartbeatTick = GetTickCount64() - 2000;
    ipc::detail::WriteSmall(ipc::detail::Path(dir, request.Pid, request.TargetId, L".request"), &request, sizeof(request));
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame), "expired owner heartbeat rejects retained mapping");
    request.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, request);
    Check(writer.Ensure(dir, request.Pid, request.TargetId, 100000) && writer.MappingNameForTest() != original,
        "capacity growth alone creates a new mapping generation");
    Check(writer.Publish(2, ipc::Status::Ready, S_OK, 8, 8, pixels)
        && reader.ReadLatest(dir, request.Pid, request.TargetId, 2, frame), "reader discovers changed mapping generation");
    request.Revision = 3; request.Scale = 2; request.Bounds = {0, 0, 16, 16};
    request.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(dir, request);
    Check(writer.Publish(3, ipc::Status::Ready, S_OK, 8, 8, pixels)
        && reader.ReadLatest(dir, request.Pid, request.TargetId, 3, frame) && frame.Width == 8 && frame.Height == 8,
        "physical bounds with 2x scale produce correct output pixel geometry");
    Check(GetFileAttributesW(backing.c_str()) == INVALID_FILE_ATTRIBUTES, "old frame file disappears after all mapping handles close");
    ipc::detail::Response unsafe{}; unsafe.CapacityBytes = 65536;
    wcscpy_s(unsafe.FileName, L"..\\capture.elsewhere.frames");
    ipc::detail::WriteSmall(ipc::detail::Path(dir, request.Pid, request.TargetId, L".response"), &unsafe, sizeof(unsafe));
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 3, frame), "unsafe response filename is rejected");
    const auto responsePath = ipc::detail::Path(dir, request.Pid, request.TargetId, L".response");
    HANDLE locked = CreateFileW(responsePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE && !reader.ReadLatest(dir, request.Pid, request.TargetId, 3, frame)
        && frame.Status == ipc::Status::Unavailable && frame.Error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
        "concrete response access failure preserves unavailable HRESULT");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    DeleteFileW(responsePath.c_str());
    Check(!reader.ReadLatest(dir, request.Pid, request.TargetId, 3, frame)
        && frame.Status == ipc::Status::Waiting && frame.Error == S_OK,
        "response not published yet remains waiting without fabricated access error");
    request.Bounds.Right = 1000000;
    Check(!ipc::WriteRequest(dir, request), "maximum pixel and dimension bounds enforced");
    request = Request(); request.Scale = std::nan("");
    SetLastError(ERROR_ACCESS_DENIED);
    Check(!ipc::WriteRequest(dir, request) && GetLastError() == ERROR_INVALID_DATA,
        "invalid request reports its own error rather than an unrelated previous I/O failure");
    {
        auto retry = Request(71); retry.TargetId = 988;
        Check(ipc::WriteRequest(dir, retry), "create complete request before transient replace lock");
        const auto requestPath = ipc::detail::Path(dir, retry.Pid, retry.TargetId, L".request");
        HANDLE held = CreateFileW(requestPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        retry.Revision = 72; retry.HeartbeatTick = GetTickCount64();
        const bool blocked = !ipc::WriteRequest(dir, retry);
        const DWORD replaceError = GetLastError();
        ipc::Request previous{};
        Check(held != INVALID_HANDLE_VALUE && blocked
            && (replaceError == ERROR_ACCESS_DENIED || replaceError == ERROR_SHARING_VIOLATION)
            && ipc::detail::ReadSmall(requestPath, &previous, sizeof(previous)) && previous.Revision == 71,
            "transient replacement denial leaves the last successful request complete and readable");
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        Check(ipc::WriteRequest(dir, retry)
            && ipc::detail::ReadSmall(requestPath, &previous, sizeof(previous)) && previous.Revision == 72,
            "next publication retries the same surface after the external read lock is released");
        ipc::CancelRequest(dir, retry.Pid, retry.TargetId);
    }
    {
        auto cached = Request(501); cached.TargetId = 765;
        cached.Flags = ipc::HostClipsCorners | ipc::KeepWarm | ipc::ClientOptics;
        ipc::detail::SetCanvasBounds(cached, cached.Bounds);
        const auto requestPath = ipc::detail::Path(dir, cached.Pid, cached.TargetId, L".request");
        const auto cachedResponsePath = ipc::detail::Path(dir, cached.Pid, cached.TargetId, L".response");
        std::vector<uint8_t> source(16 * 8 * 4, 67);
        ipc::detail::FrameWriter cachedWriter; ipc::FrameReader cachedReader, strictReader; ipc::Frame result;
        const auto publish = [&]
        {
            return cachedWriter.Publish(cached.Revision, ipc::Status::Ready, S_OK,
                cached.OutputWidth, cached.OutputHeight, source, true, {}, 0,
                ipc::detail::CanvasBounds(cached), ipc::BaseCanvasPayload);
        };
        Check(ipc::WriteRequest(dir, cached) && cachedWriter.Ensure(dir, cached.Pid, cached.TargetId, source.size())
            && publish() && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100),
            "cache fixture reads validated client-optics request response and source pixels");
        HANDLE heldRequest = CreateFileW(requestPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE heldResponse = CreateFileW(cachedResponsePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(heldRequest != INVALID_HANDLE_VALUE && heldResponse != INVALID_HANDLE_VALUE
            && !cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Status == ipc::Status::Ready && result.Error == S_OK,
            "fresh opt-in metadata cache reads mapped source despite exclusive request and response file locks");
        Check(!strictReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result)
            && result.Status == ipc::Status::Unavailable && result.Error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            "default strict readers still observe metadata access failures immediately");
        auto* initialView = cachedWriter.ViewForTest();
        auto& initialSlot = initialView->Slots[initialView->ActiveSlot];
        InterlockedIncrement64(&initialSlot.Sequence);
        Check(!cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Status == ipc::Status::Waiting && result.Pixels.empty(),
            "cached metadata still rejects an in-progress mapped slot sequence");
        InterlockedIncrement64(&initialSlot.Sequence);
        Check(cachedWriter.Publish(cached.Revision, ipc::Status::Unavailable, E_ACCESSDENIED, 0, 0, {}, true)
            && !cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Status == ipc::Status::Unavailable && result.Error == E_ACCESSDENIED,
            "cached metadata never masks immediate mapped source failure");
        Check(cachedWriter.Publish(cached.Revision, ipc::Status::Unavailable, E_FAIL, 0, 0, {}, true)
            && !cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Error == E_FAIL, "cached source error changes remain immediate without new metadata");
        Check(publish() && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100),
            "cached mapped source can recover without reopening locked metadata files");
        auto* mapped = cachedWriter.ViewForTest(); auto& current = mapped->Slots[mapped->ActiveSlot];
        current.PublishedTick = GetTickCount64() - 3000;
        Check(!cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Error == HRESULT_FROM_WIN32(WAIT_TIMEOUT), "cached source publication expiry is checked on every read");
        current.PublishedTick = GetTickCount64();
        Sleep(110);
        Check(!cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 1000)
            && result.Error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            "metadata refresh is mandatory after 100 milliseconds even when a caller requests a longer interval");
        if (heldRequest != INVALID_HANDLE_VALUE) CloseHandle(heldRequest);
        if (heldResponse != INVALID_HANDLE_VALUE) CloseHandle(heldResponse);
        cached.HeartbeatTick = GetTickCount64(); Check(ipc::WriteRequest(dir, cached) && publish()
            && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100), "prime layout cache");
        cached.Bounds.Right = 8; ipc::detail::SetCanvasBounds(cached, cached.Bounds); source.resize(8 * 8 * 4);
        Check(ipc::WriteRequest(dir, cached) && publish()
            && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Width == 8 && result.Height == 8, "changed mapped layout forces metadata refresh within the cache interval");
        heldRequest = CreateFileW(requestPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(heldRequest != INVALID_HANDLE_VALUE
            && !cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision + 1, result, 100)
            && result.Error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "requested epoch changes invalidate the metadata cache immediately");
        if (heldRequest != INVALID_HANDLE_VALUE) CloseHandle(heldRequest);
        cachedReader.Close(); cached.HeartbeatTick = GetTickCount64() - 1460;
        Check(ipc::WriteRequest(dir, cached) && publish()
            && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100), "prime nearly expired owner heartbeat cache");
        Sleep(60);
        Check(!cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Status == ipc::Status::Waiting && result.Error == S_OK,
            "cached owner heartbeat expiration is not extended by the metadata interval");
        cached.HeartbeatTick = GetTickCount64();
        Check(ipc::WriteRequest(dir, cached) && publish()
            && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100), "prime mapped payload validation cache");
        mapped->Slots[mapped->ActiveSlot].PayloadFlags = 2;
        Check(!cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Status == ipc::Status::Unavailable && result.Error == HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "cached metadata does not bypass mapped payload validation");
        Check(publish() && cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100),
            "prime cached old mapping before backing allocation grows");
        cached.Bounds = {0,0,16,16}; ipc::detail::SetCanvasBounds(cached, cached.Bounds); source.resize(16 * 16 * 4);
        Check(ipc::WriteRequest(dir, cached) && cachedWriter.Ensure(dir, cached.Pid, cached.TargetId, source.size()) && publish(),
            "publish a larger source mapping without changing the requested epoch");
        Sleep(110);
        Check(cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId, cached.Revision, result, 100)
            && result.Width == 16 && result.Height == 16 && result.Pixels == source,
            "a new mapping generation is discovered at the next bounded metadata refresh");
        Check(!cachedReader.ReadLatest(dir, cached.Pid, cached.TargetId + 1, cached.Revision, result, 100)
            && result.Status == ipc::Status::Waiting, "target identity changes cannot reuse another target metadata cache");
        cachedWriter.Close(); cachedReader.Close(); ipc::CancelRequest(dir, cached.Pid, cached.TargetId);
    }
    writer.Close(); reader.Close(); ipc::CancelRequest(dir, request.Pid, request.TargetId);
    Check(!std::filesystem::exists(ipc::detail::Path(dir, request.Pid, request.TargetId, L".request")), "cancel removes request");
    if (argc > 2) TestRealBroker(dir, argv[2]);
    printf("Broker transport tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
