#define wmain NovaBarCaptureMain
#include "../main.cpp"
#undef wmain
#include <fstream>
#include <iterator>

namespace
{
    // Only replace asynchronous WGC discovery. The actual target lifecycle,
    // rendering, frame mapping and publication remain production behavior.
    struct ControlledScene
    {
        bool Available = true;
        bool Changed = true;
        uint8_t Blue = 11;
        int Updates = 0;
        novabar::PixelFrame Pixels;
        explicit ControlledScene(std::shared_ptr<novabar::SceneCapturePool>) {}
        novabar::SceneUpdate Update(HWND, novabar::PixelRect bounds,
            novabar::ScenePlacement = novabar::ScenePlacement::Actual)
        {
            ++Updates;
            if (!Available) return {false, false, E_PENDING};
            Pixels.Bounds = bounds; Pixels.Width = bounds.Right-bounds.Left; Pixels.Height = bounds.Bottom-bounds.Top;
            Pixels.Pixels.resize(static_cast<size_t>(Pixels.Width)*Pixels.Height*4);
            for(size_t i=0;i<Pixels.Pixels.size();i+=4)
            { Pixels.Pixels[i]=Blue; Pixels.Pixels[i+1]=22; Pixels.Pixels[i+2]=33; Pixels.Pixels[i+3]=255; }
            return {true, Changed, S_OK};
        }
        auto const& Frame() const { return Pixels; }
    };
    struct ControlledGpu
    {
        bool Succeeds = true, Flat = false;
        int Calls = 0;
        explicit ControlledGpu(std::shared_ptr<novabar::GpuPanelPreprocessor::SharedDevice> const&) {}
        bool Render(novabar::PixelFrame const& frame, novabar::PixelRect, int width, int height,
            novabar::PanelGlassOptions const& options, std::vector<uint8_t>& out)
        {
            ++Calls; Flat = options.Canvas && !options.ClipRounded && options.Radius == 0
                && options.Specular == 0 && options.Material.Lens == 0 && options.Material.Dispersion == 0;
            if (!Succeeds) { out.clear(); return false; }
            out.resize(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < out.size(); i += 4) std::memcpy(out.data() + i, frame.Pixels.data(), 4);
            return true;
        }
        HRESULT LastError() const { return Succeeds ? S_OK : E_FAIL; }
    };
    struct ControlledGpuScene
    {
        novabar::PixelFrame Pixels;
        explicit ControlledGpuScene(std::shared_ptr<novabar::GpuPanelPreprocessor::SharedDevice> const&) {}
        ID3D11ShaderResourceView* View() const { return nullptr; }
        auto const& Geometry() const { return Pixels; }
    };
    struct ControlledGpuTexture : ControlledGpu
    {
        int TextureCalls=0, TaskbarCalls=0;
        using ControlledGpu::ControlledGpu;
        bool RenderTexture(ID3D11ShaderResourceView*,novabar::PixelFrame const& pixels,novabar::PixelRect target,
            int width,int height,novabar::PanelGlassOptions const& options,std::vector<uint8_t>& out)
        { ++TextureCalls;return Render(pixels,target,width,height,options,out); }
        bool RenderTaskbarTexture(ID3D11ShaderResourceView*,novabar::PixelFrame const& pixels,novabar::PixelRect target,
            int width,int height,novabar::GlassOptions const& material,std::vector<uint8_t>& out)
        { ++TaskbarCalls;novabar::PanelGlassOptions options;options.Material=material;
            return Render(pixels,target,width,height,options,out); }
    };
    struct ControlledGpuSource : ControlledScene
    {
        HRESULT GpuError=S_OK;
        int GpuUpdates=0;
        uint8_t GpuBlue=99;
        bool GpuChanged=true;
        using ControlledScene::ControlledScene;
        novabar::SceneUpdate UpdateGpu(HWND,novabar::PixelRect bounds,novabar::ScenePlacement,
            ControlledGpuScene& compositor,bool)
        {
            ++GpuUpdates;if(FAILED(GpuError))return {false,false,GpuError};
            auto& frame=compositor.Pixels;frame.Bounds=bounds;frame.Width=bounds.Right-bounds.Left;frame.Height=bounds.Bottom-bounds.Top;
            frame.Pixels.resize(static_cast<size_t>(frame.Width)*frame.Height*4);
            for(size_t i=0;i<frame.Pixels.size();i+=4){frame.Pixels[i]=GpuBlue;frame.Pixels[i+1]=22;frame.Pixels[i+2]=33;frame.Pixels[i+3]=255;}
            return {true,GpuChanged,S_OK};
        }
    };
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    int failures = 0;
    const auto check = [&](bool ok, char const* name)
    { printf("%s: %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    const std::wstring root = argv[1];
    check(!ShouldRestartCaptureDevice(S_OK) && !ShouldRestartCaptureDevice(S_FALSE),
        "healthy shared GPU device does not restart the broker");
    check(ShouldRestartCaptureDevice(DXGI_ERROR_DEVICE_REMOVED)
        && ShouldRestartCaptureDevice(DXGI_ERROR_DEVICE_RESET) && ShouldRestartCaptureDevice(DXGI_ERROR_DEVICE_HUNG),
        "removed reset and hung shared devices request a bounded broker restart");
    CapturePhaseTiming measured;
    check(CaptureMeasure(false,measured,[]{return 17;})==17 && measured.Count==0,
        "disabled phase profiling preserves results without recording timings");
    check(CaptureMeasure(true,measured,[]{Sleep(1);return 23;})==23
        && measured.Count==1 && measured.Last>0 && measured.Max==measured.Last,
        "enabled phase profiling records QPC elapsed duration and preserves results");
    check(NeedsUpdate(false, false, 100, 0), "hidden canvas initial update is immediate");
    check(!NeedsUpdate(false, false, 199, 100) && NeedsUpdate(false, false, 200, 100),
        "unchanged hidden canvas becomes eligible exactly at its hundred millisecond deadline");
    check(NeedsUpdate(true, false, 101, 100) && NeedsUpdate(false, true, 101, 100),
        "visible or dirty canvas always bypasses hidden rendering throttle");
    Targets targets;
    check(Summary(root, targets) == "state=idle\nactive=0\nready=0\nerror=0\n", "no active captures produces idle summary");
    auto target = std::make_unique<Target>(); target->Directory = root + L"\\bridge-fixture";
    auto* view = target.get(); targets.emplace(L"target", std::move(target));
    check(Summary(root, targets).starts_with("state=waiting\nactive=1\nready=0\n"), "initial WGC warmup remains waiting");
    view->CurrentStatus = ipc::Status::Unavailable; view->CurrentError = HRESULT_FROM_WIN32(ERROR_NOT_READY);
    check(Summary(root, targets).starts_with("state=waiting\n"), "ERROR_NOT_READY is classified as warmup");
    view->CurrentStatus = ipc::Status::Ready; view->CurrentError = S_OK;
    check(Summary(root, targets) == "state=ready\nactive=1\nready=1\nerror=0\n", "ready counts include bridge child directories");
    {
        // Break caught: a permanently registered hidden panel overrides the
        // ready taskbar and leaves the UI reporting capture warmup forever.
        auto hiddenWarm = std::make_unique<Target>(); hiddenWarm->Directory = root + L"\\bridge-hidden";
        hiddenWarm->Request.Visible = 0; hiddenWarm->Request.Flags = ipc::KeepWarm;
        hiddenWarm->CurrentStatus = ipc::Status::Waiting; hiddenWarm->CurrentError = E_PENDING;
        targets.emplace(L"hidden-warm", std::move(hiddenWarm));
        check(Summary(root, targets) == "state=ready\nactive=1\nready=1\nerror=0\n",
            "one visible ready target plus hidden warm registration remains ready with one active target");
        view->Request.Visible = 0;
        check(Summary(root, targets) == "state=idle\nactive=0\nready=0\nerror=0\n",
            "only hidden registrations produce idle with no visible active captures");
        view->Request.Visible = 1;
        targets.erase(L"hidden-warm");
    }
    view->CurrentStatus = ipc::Status::Unavailable; view->CurrentError = E_ACCESSDENIED;
    check(Summary(root, targets) == "state=unavailable\nactive=1\nready=0\nerror=-2147024891\n", "unavailable summary retains signed decimal HRESULT");
    check(Summary(root + L"-unrelated", targets).starts_with("state=idle\n"), "summary never includes unrelated watch root");
    std::map<std::wstring, StatusStamp> stamps;
    PublishSummaries({root}, targets, stamps);
    check(!std::filesystem::exists(root), "status producer never creates missing package directory");
    std::filesystem::create_directories(root);
    PublishSummaries({root}, targets, stamps);
    std::ifstream input(std::filesystem::path(root) / L"capture-status.txt", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    input.close();
    check(text == Summary(root, targets), "atomic status file contains exact UTF-8 keyline schema");
    const uint64_t first = stamps[root].Tick;
    Sleep(15); PublishSummaries({root}, targets, stamps);
    check(stamps[root].Tick == first, "unchanged summary avoids writes between heartbeat deadlines");
    stamps[root].Tick = GetTickCount64() - 1001; PublishSummaries({root}, targets, stamps);
    check(stamps[root].Tick > first, "unchanged summary renews one-second heartbeat");
    {
        std::filesystem::create_directories(view->Directory);
        const auto path = ipc::detail::Path(view->Directory, view->Request.Pid,
            view->Request.TargetId, L".broker-performance.json");
        std::map<std::wstring,uint64_t> checks;
        PublishPerformance({root}, targets, checks);
        check(!std::filesystem::exists(path), "broker backend diagnostics remain opt-in");
        std::ofstream(std::filesystem::path(root) / L"performance.enabled") << "enabled\n";
        view->GpuFrames=7; view->CpuFrames=2; view->GpuFailures=1; view->LastGpuError=E_ACCESSDENIED;
        view->LastFrameUsedGpu=true; checks.clear(); PublishPerformance({root}, targets, checks);
        const auto read = [&] { std::ifstream file(path,std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(file)),{}); };
        const std::string firstPerformance=read();
        check(firstPerformance.find("\"gpuFrames\":7") != std::string::npos
            && firstPerformance.find("\"cpuFrames\":2") != std::string::npos
            && firstPerformance.find("\"gpuFailures\":1") != std::string::npos
            && firstPerformance.find("\"lastGpuError\":-2147024891") != std::string::npos
            && firstPerformance.find("\"backend\":\"gpu\"") != std::string::npos,
            "watch-root performance marker publishes per-target backend counts and signed HRESULT");
        view->GpuFrames=8; PublishPerformance({root},targets,checks);
        check(read()==firstPerformance, "backend counters do not cause per-frame diagnostic file writes");
        checks.begin()->second=GetTickCount64()-1001; PublishPerformance({root},targets,checks);
        check(read().find("\"gpuFrames\":8") != std::string::npos,
            "backend diagnostic file renews at the one-second deadline");
        std::filesystem::remove(std::filesystem::path(root) / L"performance.enabled");
    }
    {
        view->CurrentStatus=ipc::Status::Ready;view->CurrentError=S_OK;
        check(!PublishDeviceFailure(S_OK,{root},targets,stamps) && view->CurrentStatus==ipc::Status::Ready,
            "healthy injected device health leaves ready targets unchanged");
        check(PublishDeviceFailure(DXGI_ERROR_DEVICE_REMOVED,{root},targets,stamps)
            && view->CurrentStatus==ipc::Status::Unavailable && view->CurrentError==DXGI_ERROR_DEVICE_REMOVED,
            "injected device removal reports unavailable and asks the worker to exit immediately");
        std::ifstream failure(std::filesystem::path(root)/L"capture-status.txt",std::ios::binary);
        const std::string failureText((std::istreambuf_iterator<char>(failure)),{});
        check(failureText.starts_with("state=unavailable\n")
            && failureText.find("error="+std::to_string(static_cast<int32_t>(DXGI_ERROR_DEVICE_REMOVED)))!=std::string::npos,
            "device removal status preserves its signed HRESULT before supervised restart");
    }
    {
        // Old per-client diagnostics must not exhaust a shared enumeration
        // budget before a fresh request in the notification watch root.
        const std::wstring firstRoot = root + L"\\scan-first", secondRoot = root + L"\\scan-second";
        std::filesystem::create_directories(firstRoot); std::filesystem::create_directories(secondRoot);
        for (unsigned i = 0; i < 2048; ++i)
            std::ofstream(std::filesystem::path(firstRoot) / (L"capture.1000." + std::to_wstring(i) + L".client-status.txt")) << "hidden\n";
        HWND hidden = CreateWindowExW(0, L"STATIC", L"Invisible broker scan fixture", WS_POPUP,
            0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(hidden != nullptr, "create hidden scan fixture without foreground or capture changes");
        if (hidden)
        {
            ipc::Request request;
            request.Pid = GetCurrentProcessId(); request.TargetId = 73; request.Revision = 1;
            request.HeartbeatTick = GetTickCount64(); request.Bounds = {0, 0, 16, 16};
            request.CoordinateHwnd = request.TopLevelHwnd = reinterpret_cast<uint64_t>(hidden);
            check(ipc::WriteRequest(secondRoot, request), "publish valid request in second watch root");
            Targets scanned;
            Scan({firstRoot, secondRoot}, scanned);
            check(scanned.size() == 1, "2048 stale diagnostic files cannot starve later watch root");
            // Break caught: dropping a hidden registration destroys the warm
            // source sessions on every close, making each reopen cold again.
            const auto* registered = scanned.begin()->second.get();
            request.Visible = 0; request.Flags |= ipc::KeepWarm; request.HeartbeatTick = GetTickCount64();
            check(ipc::WriteRequest(secondRoot, request), "publish hidden registration heartbeat");
            Scan({firstRoot, secondRoot}, scanned);
            check(scanned.size() == 1 && scanned.begin()->second.get() == registered,
                "hidden registration retains the same capture target across scans");
            check(Summary(secondRoot, scanned).starts_with("state=idle\nactive=0\nready=0\n"),
                "hidden warm registrations do not claim visible capture readiness");
            if (!scanned.empty())
            {
                scanned.begin()->second->Update();
                check(!std::filesystem::exists(ipc::detail::Path(secondRoot, request.Pid, request.TargetId, L".response")),
                    "hidden warm registration does not publish visible pixel mapping");
            }
            {
                BasicTarget<ControlledScene> canvas;
                canvas.Directory = secondRoot; canvas.Request = request; canvas.Request.TargetId = 74;
                canvas.Request.Flags = ipc::KeepWarm | ipc::HostClipsCorners | ipc::WorldCanvas;
                canvas.Request.Bounds = {32,32,48,48};
                canvas.Request.OutputWidth = canvas.Request.OutputHeight = 16;
                canvas.Request.Reserved2 = static_cast<uint16_t>(-32)
                    | (static_cast<uint32_t>(static_cast<uint16_t>(-32)) << 16);
                canvas.Request.Material = {0,0,0,0,0,0}; canvas.Request.Radius = canvas.Request.Specular = 0;
                canvas.Request.MotionSequence = 1;
                canvas.Update();
                check(canvas.CurrentStatus == ipc::Status::Ready && canvas.Pixels.size()==16*16*4
                    && canvas.Pixels[0]==11 && canvas.Pixels[1]==22 && canvas.Pixels[2]==33 && canvas.Pixels[3]==255,
                    "hidden world canvas publishes complete real scene pixels before the material appears");
                auto const* published = canvas.Writer.ViewForTest();
                check(published && published->Slots[published->ActiveSlot].BitmapBounds.Left==0
                    && published->Slots[published->ActiveSlot].BitmapBounds.Right==16
                    && published->Slots[published->ActiveSlot].RenderedBounds.Left==32
                    && published->Slots[published->ActiveSlot].MotionSequence==1,
                    "hidden canvas publication preserves separate bitmap and material geometry");
                const auto firstSerial = published ? published->Slots[published->ActiveSlot].Serial : 0;
                canvas.Update();
                check(canvas.Scene.Updates==1 && canvas.CurrentStatus==ipc::Status::Ready
                    && published && published->Slots[published->ActiveSlot].Serial==firstSerial,
                    "unchanged hidden canvas skips scene rendering and keeps its ready publication between deadlines");
                canvas.Dirty = true; canvas.Update();
                check(canvas.Scene.Updates==2, "dirty hidden canvas updates immediately before its normal deadline");
                canvas.Request.Visible = 1; canvas.Update();
                check(canvas.Scene.Updates==3, "visible canvas bypasses the hidden rendering deadline");
                canvas.Request.Visible = 0;
                canvas.Scene.Available = false; canvas.Dirty = true; canvas.Update();
                check(canvas.CurrentStatus == ipc::Status::Waiting,
                    "hidden world canvas stays waiting when its actual source scene is incomplete");
                const int waitingUpdates = canvas.Scene.Updates;
                canvas.Update();
                check(canvas.Scene.Updates==waitingUpdates && canvas.CurrentStatus==ipc::Status::Waiting,
                    "skipping an unchanged hidden canvas preserves waiting instead of inventing a ready frame");
                canvas.LastHiddenUpdate = GetTickCount64() - 100;
                canvas.Scene.Available = true; canvas.Update();
                check(canvas.Scene.Updates==waitingUpdates+1 && canvas.CurrentStatus==ipc::Status::Ready,
                    "hidden deadline performs a real scene update before restoring ready publication");
            }
            {
                BasicTarget<ControlledScene> base;
                base.Directory=secondRoot; base.Request=request; base.Request.TargetId=75;
                base.Request.Flags=ipc::KeepWarm|ipc::HostClipsCorners|ipc::WorldCanvas|ipc::ClientOptics;
                base.Request.Visible=1; base.Request.Scale=1; base.Request.Bounds={2,2,14,14};
                base.Request.OutputWidth=base.Request.OutputHeight=16;
                base.Request.Reserved2=static_cast<uint16_t>(-2)
                    | (static_cast<uint32_t>(static_cast<uint16_t>(-2))<<16);
                base.Request.Material={.55,.18,0,0,0,0}; base.Request.Radius=4; base.Request.Specular=.6;
                base.Request.MotionSequence=1; base.Scene.Changed=false; base.Update();
                const auto* mapping=base.Writer.ViewForTest();
                const auto firstBase=mapping ? mapping->Slots[mapping->ActiveSlot] : ipc::detail::Slot{};
                check(base.CurrentStatus==ipc::Status::Ready && firstBase.PayloadFlags==ipc::BaseCanvasPayload
                    && firstBase.RenderedBounds.Left==0 && firstBase.RenderedBounds.Right==0 && firstBase.MotionSequence==0,
                    "client-optics producer acknowledges a real base payload without claiming a rendered lens pose");
                check(base.CpuFrames == 1 && base.GpuFrames == 0,
                    "default unavailable GPU preserves the genuine CPU base fallback");
                BasicTarget<ControlledScene,ControlledGpu> accelerated;
                accelerated.Directory=secondRoot; accelerated.Request=base.Request; accelerated.Request.TargetId=76;
                accelerated.PerformanceEnabled=true;
                accelerated.Update();
                check(accelerated.Gpu.Calls==1 && accelerated.Gpu.Flat && accelerated.GpuFrames==1
                    && accelerated.CpuFrames==0 && accelerated.CurrentStatus==ipc::Status::Ready,
                    "client-optics base takes the shared GPU backend with flat options");
                check(accelerated.SceneTiming.Count==1 && accelerated.RendererTiming.Count==1
                    && accelerated.GpuTiming.Count==1 && accelerated.CpuTiming.Count==0
                    && accelerated.PublishTiming.Count==1 && accelerated.UpdateTiming.Count==1,
                    "GPU base records separate scene renderer publication and total-update phases");
                accelerated.Gpu.Succeeds=false; accelerated.Scene.Blue=44; accelerated.Dirty=true; accelerated.Update();
                check(accelerated.Gpu.Calls==2 && accelerated.GpuFrames==1 && accelerated.CpuFrames==1
                    && accelerated.GpuFailures==1 && accelerated.LastGpuError==E_FAIL
                    && accelerated.Pixels[0]==44 && accelerated.CurrentStatus==ipc::Status::Ready,
                    "GPU failure falls back to the same current genuine scene and records its HRESULT");
                check(accelerated.GpuTiming.Count==2 && accelerated.CpuTiming.Count==1
                    && accelerated.RendererTiming.Count==2 && accelerated.UpdateTiming.Count==2
                    && BackendPerformance(accelerated,GetTickCount64()).find("\"sceneMs\":{\"last\":")!=std::string::npos,
                    "fallback phase diagnostics retain both GPU attempt and genuine CPU rendering time");
                accelerated.Request.Flags &= ~ipc::ClientOptics; accelerated.Dirty=true; accelerated.Update();
                check(accelerated.Gpu.Calls==2 && accelerated.CpuFrames==2,
                    "legacy finished optics retain the existing CPU renderer without GPU base processing");
                BasicTarget<ControlledGpuSource,ControlledGpuTexture,ControlledGpuScene> texture;
                texture.Directory=secondRoot;texture.Request=base.Request;texture.Request.TargetId=77;
                texture.Scene.GpuError=E_PENDING;texture.Update();
                check(texture.Scene.GpuUpdates==1 && texture.Scene.Updates==0 && texture.Gpu.TextureCalls==0
                    && texture.CurrentStatus==ipc::Status::Waiting,
                    "unknown GPU contributor stays strictly waiting without starting a CPU scene fallback");
                texture.Scene.GpuError=S_OK;texture.Dirty=true;texture.Update();
                check(texture.Gpu.TextureCalls==1 && texture.Scene.Updates==0 && texture.GpuSceneFrames==1
                    && texture.CpuSceneFrames==0 && texture.LastSceneUsedGpu && texture.Pixels[0]==99,
                    "ready GPU scene feeds its texture directly into base processing without CPU pixels");
                texture.Scene.GpuError=E_FAIL;texture.Scene.Blue=55;texture.Dirty=true;texture.Update();
                check(texture.Scene.Updates==1 && texture.GpuSceneFailures==1 && texture.CpuSceneFrames==1
                    && !texture.LastSceneUsedGpu && texture.Pixels[0]==55 && texture.CurrentStatus==ipc::Status::Ready,
                    "hard GPU scene failure falls back to a complete current genuine CPU scene");
                texture.Scene.GpuError=S_OK;texture.Gpu.Succeeds=false;texture.Scene.Blue=66;texture.Dirty=true;texture.Update();
                check(texture.Gpu.TextureCalls==2 && texture.Scene.Updates==2 && texture.CpuFrames==1
                    && !texture.LastSceneUsedGpu && texture.Pixels[0]==66 && texture.CurrentStatus==ipc::Status::Ready,
                    "texture filter failure reconstructs CPU pixels before CPU rendering instead of reusing stale data");
                BasicTarget<ControlledGpuSource,ControlledGpuTexture,ControlledGpuScene> switching;
                switching.Directory=secondRoot;switching.Request=base.Request;switching.Request.TargetId=78;
                switching.Scene.GpuError=E_FAIL;switching.Scene.Blue=44;switching.Update();
                switching.Scene.GpuError=S_OK;switching.Update();
                switching.Scene.GpuError=E_FAIL;switching.Scene.Changed=false;switching.Update();
                check(switching.Pixels[0]==44 && !switching.LastSceneUsedGpu,
                    "GPU-to-CPU scene transition redraws when only the CPU backend cache is unchanged");
                BasicTarget<ControlledGpuSource,ControlledGpuTexture,ControlledGpuScene> switchingBack;
                switchingBack.Directory=secondRoot;switchingBack.Request=base.Request;switchingBack.Request.TargetId=79;
                switchingBack.Scene.Blue=44;switchingBack.Update();
                switchingBack.Scene.GpuError=E_FAIL;switchingBack.Update();
                switchingBack.Scene.GpuError=S_OK;switchingBack.Scene.GpuChanged=false;switchingBack.Update();
                check(switchingBack.Pixels[0]==99 && switchingBack.LastSceneUsedGpu,
                    "CPU-to-GPU scene transition redraws when only the GPU backend cache is unchanged");
                const HWND taskbarWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Offscreen taskbar fixture",
                    WS_POPUP|WS_VISIBLE,-32000,-32000,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
                check(taskbarWindow!=nullptr,"create offscreen non-activating taskbar fixture");
                if(taskbarWindow)
                {
                    BasicTarget<ControlledGpuSource,ControlledGpuTexture,ControlledGpuScene> strip;
                    strip.Directory=secondRoot;strip.Request=base.Request;strip.Request.TargetId=80;
                    strip.Request.Kind=ipc::Kind::Taskbar;strip.Request.Flags=0;strip.Request.Bounds={0,0,16,16};
                    strip.Request.OutputWidth=strip.Request.OutputHeight=16;strip.Request.MotionSequence=15;
                    strip.Request.CoordinateHwnd=strip.Request.TopLevelHwnd=reinterpret_cast<uint64_t>(taskbarWindow);
                    strip.Update();
                    check(strip.CurrentStatus==ipc::Status::Ready && strip.Gpu.TaskbarCalls==1
                        && strip.Gpu.TextureCalls==0 && strip.Scene.Updates==0 && strip.LastSceneUsedGpu && strip.Pixels[0]==99,
                        "taskbar uses its legacy strip texture entry after GPU scene composition");
                    auto const* stripMap=strip.Writer.ViewForTest();
                    check(stripMap && stripMap->Slots[stripMap->ActiveSlot].PayloadFlags==0
                        && stripMap->Slots[stripMap->ActiveSlot].MotionSequence==15
                        && stripMap->Slots[stripMap->ActiveSlot].RenderedBounds.Right==16,
                        "GPU taskbar retains payload-zero rendered geometry and motion acknowledgement");
                    strip.Gpu.Succeeds=false;strip.Scene.Blue=77;strip.Dirty=true;strip.Update();
                    check(strip.CurrentStatus==ipc::Status::Ready && strip.Gpu.TaskbarCalls==2
                        && strip.Scene.Updates==1 && strip.CpuFrames==1 && !strip.LastSceneUsedGpu && strip.Pixels[0]==77,
                        "taskbar strip failure rebuilds the current CPU scene before legacy CPU rendering");
                    strip.Request.OutputWidth=8;strip.Gpu.Succeeds=true;strip.Scene.Blue=88;strip.Dirty=true;strip.Update();
                    check(strip.CurrentStatus==ipc::Status::Ready && strip.Gpu.TaskbarCalls==2
                        && strip.Scene.Updates==2 && !strip.LastSceneUsedGpu && strip.Pixels[0]==88,
                        "scaled taskbar output bypasses unsupported GPU scene work and preserves legacy CPU sampling");
                    DestroyWindow(taskbarWindow);
                }
                check(base.Pixels.size()==16*16*4 && base.Pixels[(2*16+8)*4]==11,
                    "client-optics producer leaves lens and specular work out of the genuine base");
                auto moved=base.Request; moved.Bounds={3,3,15,15}; moved.Radius=3; moved.MotionSequence=2;
                moved.Reserved2=static_cast<uint16_t>(-3)
                    | (static_cast<uint32_t>(static_cast<uint16_t>(-3))<<16);
                base.Dirty=base.Dirty||NeedsRender(base.Request,moved); base.Request=moved; base.Update();
                check(mapping && mapping->Slots[mapping->ActiveSlot].Serial==firstBase.Serial,
                    "mere material pose changes preserve the static producer base serial");
                base.Scene.Changed=true; base.Scene.Blue=44; base.Update();
                check(mapping && mapping->Slots[mapping->ActiveSlot].Serial>firstBase.Serial && base.Pixels[0]==44,
                    "a genuine source change replaces the acknowledged base pixels and serial");
            }
            request.Visible = 1;
            scanned.clear();
            for (unsigned i = 0; i < 384; ++i)
                std::ofstream(std::filesystem::path(firstRoot) / (L"capture.0." + std::to_wstring(i) + L".request")) << "invalid";
            request.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(secondRoot, request);
            Scan({firstRoot, secondRoot}, scanned);
            check(scanned.size() == 1, "candidate request budget is independent for every watch root");
            scanned.clear();
            ipc::CancelRequest(secondRoot, request.Pid, request.TargetId);
            const std::wstring legacyRoot = root + L"\\scan-legacy", bridge = legacyRoot + L"\\bridge-fixture";
            std::filesystem::create_directories(bridge);
            request.HeartbeatTick = GetTickCount64(); ipc::WriteRequest(bridge, request);
            Scan({legacyRoot}, scanned);
            check(scanned.size() == 1 && scanned.begin()->second->Directory == bridge,
                "bridge child requests remain discoverable when watch root has no direct requests");
            scanned.clear();
            ipc::CancelRequest(bridge, request.Pid, request.TargetId);
            DestroyWindow(hidden);
        }
    }
    printf("Capture status tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
