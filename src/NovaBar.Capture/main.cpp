// Full-trust capture broker. All WGC devices, sessions and scene work stay on
// this single MTA; injected shell code only exchanges requests and BGRA frames.
#include "CaptureFrameWriter.h"
#include "SceneBackdropSource.h"
#include "PanelGlassPixels.h"
#include "GpuPanelPreprocessor.h"
#include "CaptureScheduler.h"
#include "CaptureTiming.h"
#include "GpuInputBroker.h"
#include <map>
#include <memory>
#include <set>
#include <cstdio>

namespace
{
    using namespace novabar;
    namespace ipc = novabar::capture;
    namespace gpu_transport = ipc::gpu;
    constexpr size_t MaxTargets = 12;
    constexpr size_t MaxRequestCandidatesPerRoot = 256;
    constexpr size_t MaxBridgeDirectoriesPerRoot = 64;
    bool WarmingUp(HRESULT error)
    { return error == E_PENDING || error == HRESULT_FROM_WIN32(ERROR_NOT_READY); }
    bool NeedsUpdate(bool visible, bool dirty, uint64_t now, uint64_t lastHiddenUpdate)
    { return visible || dirty || !lastHiddenUpdate || now - lastHiddenUpdate >= 100; }
    std::atomic<bool> running{true};
    BOOL WINAPI ConsoleControl(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
        { running = false; return TRUE; }
        return FALSE;
    }
    bool SameGeometry(ipc::Request const& a, ipc::Request const& b)
    {
        const bool common=a.TopLevelHwnd == b.TopLevelHwnd && a.CoordinateHwnd == b.CoordinateHwnd
            && a.Scale == b.Scale && a.Kind == b.Kind && a.Flags == b.Flags
            && a.OutputWidth == b.OutputWidth && a.OutputHeight == b.OutputHeight;
        if(!common) return false;
        if((a.Flags & ipc::ClientOptics) && (b.Flags & ipc::ClientOptics))
            return scene_detail::SameRect(ipc::detail::CanvasBounds(a),ipc::detail::CanvasBounds(b));
        return scene_detail::SameRect(a.Bounds,b.Bounds) && a.Reserved2 == b.Reserved2;
    }
    bool SameStyle(ipc::Request const& a, ipc::Request const& b)
    {
        const bool common=a.Flags == b.Flags && a.Material.Noise == b.Material.Noise && a.Material.Tint == b.Material.Tint
            && a.Material.Blur == b.Material.Blur && a.Material.Color == b.Material.Color;
        return common && (((a.Flags & ipc::ClientOptics) && (b.Flags & ipc::ClientOptics))
            || (a.Radius == b.Radius && a.Specular == b.Specular
                && a.Material.Lens == b.Material.Lens && a.Material.Dispersion == b.Material.Dispersion));
    }
    bool NeedsRender(ipc::Request const& a, ipc::Request const& b)
    {
        const bool base=(a.Flags & ipc::ClientOptics) && (b.Flags & ipc::ClientOptics);
        return a.Revision != b.Revision || (!base && a.MotionSequence != b.MotionSequence) || a.Visible != b.Visible
            || !SameGeometry(a,b) || !SameStyle(a,b);
    }
    bool OwnsWindows(ipc::Request const& request)
    {
        const HWND coordinate = reinterpret_cast<HWND>(request.CoordinateHwnd);
        const HWND top = reinterpret_cast<HWND>(request.TopLevelHwnd);
        DWORD coordinatePid = 0, topPid = 0;
        if (!IsWindow(coordinate) || !IsWindow(top) || GetAncestor(top, GA_ROOT) != top
            || GetAncestor(coordinate, GA_ROOT) != top) return false;
        GetWindowThreadProcessId(coordinate, &coordinatePid);
        GetWindowThreadProcessId(top, &topPid);
        return coordinatePid == request.Pid && topPid == request.Pid;
    }

    // Ownership advertisements use the exact existing request/control directory.
    // Only the watch roots and live target directories are retained; no unrelated
    // package tree or unbounded set of historical bridge directories is created.
    class GpuOwnerDirectories
    {
        struct Entry
        {
            gpu_transport::GpuOwnerWriter Writer;
            uint64_t Renewed = 0;
        };
        std::map<std::wstring, std::unique_ptr<Entry>> m_entries;
        LUID m_adapter{};
    public:
        explicit GpuOwnerDirectories(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& shared)
        {
            if (!shared || !shared->Device || !shared->Context) return;
            Microsoft::WRL::ComPtr<ID3D11Device5> device5;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (FAILED(shared->Device.As(&device5)) || FAILED(shared->Context.As(&context4))
                || FAILED(shared->Device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))
                || FAILED(adapter->GetDesc(&desc))) return;
            m_adapter = desc.AdapterLuid;
        }
        bool Ensure(std::wstring const& directory)
        {
            if (!m_adapter.LowPart && !m_adapter.HighPart) return false;
            if (m_entries.contains(directory)) return true;
            if (m_entries.size() >= MaxTargets + 8) return false;
            const DWORD attributes = GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
                || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            auto entry = std::make_unique<Entry>();
            if (!entry->Writer.Publish(directory, gpu_transport::CurrentIdentity(), gpu_transport::RandomNonce(), m_adapter))
                return false;
            entry->Renewed = GetTickCount64();
            m_entries.emplace(directory, std::move(entry)); return true;
        }
        void Refresh(uint64_t now)
        {
            for (auto& [directory, entry] : m_entries)
            {
                (void)directory;
                if (now >= entry->Renewed && now - entry->Renewed < 250) continue;
                // Failure never authorizes replacing a different live writer.
                if (entry->Writer.Refresh(now)) entry->Renewed = now;
            }
        }
        void Keep(std::set<std::wstring> const& directories)
        { std::erase_if(m_entries, [&](auto const& entry) { return !directories.contains(entry.first); }); }
        void Withdraw() { m_entries.clear(); }
    };
    template<class SceneSource = SceneBackdropSource, class GpuBackend = GpuPanelPreprocessor,
        class SceneGpuBackend = GpuSceneCompositor>
    struct BasicTarget
    {
        std::wstring Directory;
        ipc::Request Request{};
        ipc::Request RenderedRequest{};
        PixelRect RenderedBitmapBounds{};
        ScenePlacement RequestedPlacement = ScenePlacement::Actual;
        HANDLE Owner = nullptr;
        SceneSource Scene;
        GpuBackend Gpu;
        SceneGpuBackend GpuScene;
        std::shared_ptr<GpuPanelPreprocessor::SharedDevice> SharedGpu;
        std::unique_ptr<gpu_transport::BrokerGpuInputTarget> GpuInput;
        std::optional<gpu_transport::GpuExpectedFrame> GpuFrame;
        HANDLE GpuClientProcess = nullptr;
        bool GpuScenePending = true;
        ipc::detail::FrameWriter Writer;
        std::vector<uint8_t> Pixels;
        uint32_t Width = 0, Height = 0;
        uint64_t Published = 0;
        uint64_t LastHiddenUpdate = 0;
        uint64_t GpuFrames = 0, CpuFrames = 0, GpuFailures = 0;
        uint64_t GpuSceneFrames = 0, CpuSceneFrames = 0, GpuSceneFailures = 0;
        HRESULT LastGpuSceneError = S_OK;
        bool LastSceneUsedGpu = false;
        HRESULT LastGpuError = S_OK;
        bool LastFrameUsedGpu = false;
        bool PerformanceEnabled = false;
        CapturePhaseTiming SceneTiming, RendererTiming, GpuTiming, CpuTiming, PublishTiming, UpdateTiming;
        CapturePhaseTiming DiscoveryTiming, ReadTiming, ComposeTiming, AllocationTiming, CoverageTiming, CompareTiming;
        SceneProfile LastSceneProfile;
        ipc::Status PublishedStatus = ipc::Status::Waiting;
        HRESULT PublishedError = S_OK;
        ipc::Status CurrentStatus = ipc::Status::Waiting;
        HRESULT CurrentError = E_PENDING;
        bool Dirty = true;
        explicit BasicTarget(std::shared_ptr<SceneCapturePool> pool = {},
            std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& gpu = {})
            : Scene(std::move(pool)), Gpu(gpu), GpuScene(gpu), SharedGpu(gpu) {}
        ~BasicTarget()
        {
            RetireGpuInput();
            if (GpuClientProcess) CloseHandle(GpuClientProcess);
            if (Owner) CloseHandle(Owner);
        }
        void RetireGpuInput()
        {
            if (GpuInput) { GpuInput->RequestRetire(); GpuInput->PollRetirement(); }
            GpuFrame.reset(); GpuScenePending = true;
        }
        void PollGpuRetirement()
        { if (GpuInput) GpuInput->PollRetirement(); }
        bool EnsureWriter()
        {
            const uint64_t capacity = (Request.Flags & ipc::GpuBase) ? 4 : static_cast<uint64_t>(Width) * Height * 4;
            return Writer.Ensure(Directory, Request.Pid, Request.TargetId, capacity);
        }
        void Publish(ipc::Status status, HRESULT error, bool changed)
        {
            if (status != ipc::Status::Ready) RetireGpuInput();
            CurrentStatus = status; CurrentError = error;
            const uint64_t now = GetTickCount64();
            if (!changed && status == PublishedStatus && error == PublishedError && now - Published < 100) return;
            const bool base=status == ipc::Status::Ready && (RenderedRequest.Flags & ipc::ClientOptics);
            DWORD publishError = ERROR_SUCCESS;
            const bool published = CaptureMeasure(PerformanceEnabled, PublishTiming, [&] {
                const bool result = Writer.Publish(Request.Revision, status, error, Width, Height, Pixels, changed
                || status != PublishedStatus || error != PublishedError,
                base ? PixelRect{} : RenderedRequest.Bounds, base ? 0 : RenderedRequest.MotionSequence,
                    RenderedBitmapBounds, base ? ipc::BaseCanvasPayload : 0);
                if (!result) publishError = GetLastError(); return result;
            });
            if (published)
            { Published = now; PublishedStatus = status; PublishedError = error; }
            else { CurrentStatus = ipc::Status::Unavailable; CurrentError = HRESULT_FROM_WIN32(publishError); }
        }
        // Called only after Update has validated HWND ownership and completed a
        // genuine GPU scene observation. The seam permits headless producer tests
        // without substituting transport, filters, fencing or frame publication.
        void UpdateGpuBase(SceneUpdate const& scene, PanelGlassOptions const& options,
            PixelRect bitmapBounds, uint64_t sourceValidatedTick)
        {
            GpuScenePending = GpuScenePending || Dirty || scene.Changed;
            if (!scene.Ready || !ipc::detail::Fresh(sourceValidatedTick))
            {
                const HRESULT error = !scene.Ready && FAILED(scene.Error) ? scene.Error : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                Publish(WarmingUp(error) ? ipc::Status::Waiting : ipc::Status::Unavailable, error, true); return;
            }
            if (!SharedGpu || !SharedGpu->Device || !SharedGpu->Context)
            { Publish(ipc::Status::Unavailable, E_NOINTERFACE, true); return; }
            if (!GpuClientProcess)
                GpuClientProcess = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                    FALSE, Request.Pid);
            if (!GpuClientProcess)
            { Publish(ipc::Status::Unavailable, HRESULT_FROM_WIN32(GetLastError()), true); return; }
            if (!GpuInput)
            {
                gpu_transport::GpuOwnerRecord owner{};
                if (!gpu_transport::TryReadFreshGpuOwner(Directory, GetTickCount64(), owner))
                { Publish(ipc::Status::Waiting, E_PENDING, false); return; }
                auto input = std::make_unique<gpu_transport::BrokerGpuInputTarget>();
                if (!input->Initialize(SharedGpu->Device.Get(), SharedGpu->Context.Get(), owner))
                { Publish(ipc::Status::Unavailable, input->LastError(), true); return; }
                GpuInput = std::move(input);
            }
            const auto imported = GpuInput->TryImport(Directory, Request, GpuClientProcess);
            if (imported != gpu_transport::BrokerImportResult::Ready)
            {
                const HRESULT error = GpuInput->LastError();
                Publish(imported == gpu_transport::BrokerImportResult::Pending ? ipc::Status::Waiting : ipc::Status::Unavailable,
                    FAILED(error) ? error : E_PENDING, false); return;
            }
            const bool sameFrame = GpuFrame && GpuFrame->Generation == GpuInput->ActiveGeneration()
                && GpuFrame->Revision == Request.Revision && PublishedStatus == ipc::Status::Ready;
            if (!GpuScenePending && sameFrame)
            {
                CurrentStatus = ipc::Status::Ready; CurrentError = S_OK;
                if (sourceValidatedTick >= Published && sourceValidatedTick - Published < 100) return;
                auto renewed = *GpuFrame; renewed.PublishedTick = sourceValidatedTick;
                if (!Writer.RenewGpu(renewed))
                { const auto error = HRESULT_FROM_WIN32(GetLastError()); Publish(ipc::Status::Unavailable, error, true); return; }
                GpuFrame = renewed; Published = GetTickCount64(); PublishedStatus = ipc::Status::Ready; PublishedError = S_OK;
                Dirty = false; return;
            }
            auto lease = GpuInput->TryAcquireWritableSlot(sameFrame ? &*GpuFrame : nullptr);
            if (!lease)
            {
                // Keep the advertised tuple readable while its client copy
                // completes. New scene changes stay pending without renewing it.
                if (GpuInput->LastError() == E_PENDING && sameFrame
                    && ipc::detail::Fresh(GpuFrame->PublishedTick))
                { CurrentStatus = ipc::Status::Ready; CurrentError = S_OK; return; }
                const HRESULT error = GpuInput->LastError();
                Publish(WarmingUp(error) ? ipc::Status::Waiting : ipc::Status::Unavailable,
                    FAILED(error) ? error : E_PENDING, true); return;
            }
            const auto serial = Writer.ReserveGpuSerial();
            if (!serial)
            { GpuInput->CancelDraw(std::move(*lease)); Publish(ipc::Status::Unavailable, E_UNEXPECTED, true); return; }
            bool rendered = false;
            if constexpr (requires { Gpu.RenderTextureToTarget(GpuScene.View(), GpuScene.Geometry(), Request.Bounds,
                Width, Height, options, lease->RenderTarget()); })
            {
                rendered = CaptureMeasure(PerformanceEnabled, GpuTiming, [&] {
                    return Gpu.RenderTextureToTarget(GpuScene.View(), GpuScene.Geometry(), Request.Bounds,
                        Width, Height, options, lease->RenderTarget()); });
            }
            if (!rendered)
            {
                LastGpuError = Gpu.LastError(); ++GpuFailures;
                GpuInput->CancelDraw(std::move(*lease));
                Publish(ipc::Status::Unavailable, FAILED(LastGpuError) ? LastGpuError : E_NOINTERFACE, true); return;
            }
            auto frame = GpuInput->Publish(std::move(*lease), serial, bitmapBounds, sourceValidatedTick);
            if (!frame || !Writer.PublishGpu(*frame))
            {
                const HRESULT error = frame ? HRESULT_FROM_WIN32(GetLastError()) : GpuInput->LastError();
                Publish(ipc::Status::Unavailable, FAILED(error) ? error : E_FAIL, true); return;
            }
            GpuFrame = *frame; GpuScenePending = false; Dirty = false;
            RenderedRequest = Request; RenderedBitmapBounds = bitmapBounds;
            LastSceneUsedGpu = LastFrameUsedGpu = true; LastGpuError = LastGpuSceneError = S_OK;
            ++GpuFrames; ++GpuSceneFrames;
            CurrentStatus = PublishedStatus = ipc::Status::Ready; CurrentError = PublishedError = S_OK;
            Published = GetTickCount64();
        }
        void Update()
        {
            CapturePhaseScope updateTiming(PerformanceEnabled, UpdateTiming);
            if (!ipc::detail::Dimensions(Request, Width, Height))
            { RetireGpuInput(); CurrentStatus = ipc::Status::InvalidRequest; CurrentError = E_INVALIDARG; return; }
            const bool gpuInput = Request.Flags & ipc::GpuBase;
            if (!gpuInput && GpuInput) RetireGpuInput();
            const HWND top = reinterpret_cast<HWND>(Request.TopLevelHwnd);
            const bool worldCanvas = Request.Flags & ipc::WorldCanvas;
            const bool hiddenCanvas = worldCanvas && (Request.Flags & ipc::KeepWarm);
            const auto placement = worldCanvas && !Request.Visible ? ScenePlacement::Promoted : ScenePlacement::Actual;
            if (!OwnsWindows(Request))
            { RetireGpuInput(); CurrentStatus = ipc::Status::Unavailable; CurrentError = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE); return; }
            if (placement != RequestedPlacement) { Dirty = true; RequestedPlacement = placement; }
            if ((!Request.Visible || ((Request.Flags & ipc::KeepWarm) && !IsWindowVisible(top))) && !hiddenCanvas)
            {
                // The shared pool is maintained independently of this hidden
                // HWND's stale Z-order. Keep its registration and material; no
                // hidden request publishes pixels as a visible ready surface.
                RetireGpuInput(); CurrentStatus = ipc::Status::Waiting; CurrentError = E_PENDING; Dirty = true; return;
            }
            if (worldCanvas && !NeedsUpdate(Request.Visible != 0, Dirty, GetTickCount64(), LastHiddenUpdate)) return;
            if (!EnsureWriter())
            {
                const DWORD error = GetLastError();
                CurrentStatus = ipc::Status::Unavailable;
                CurrentError = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
                RetireGpuInput();
                return;
            }
            if (!IsWindowVisible(top) && !hiddenCanvas)
            { Publish(ipc::Status::Unavailable, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE), Dirty); return; }
            const int desktopX = GetSystemMetrics(SM_XVIRTUALSCREEN), desktopY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const PixelRect viewport{desktopX, desktopY, desktopX + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                desktopY + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            PanelGlassOptions options{Request.Material, Request.Radius, Request.Specular,
                !(Request.Flags & ipc::HostClipsCorners), viewport};
            if (worldCanvas) options.Canvas = ipc::detail::CanvasBounds(Request);
            if(Request.Flags & ipc::ClientOptics)
            {
                // The immutable base carries genuine blur/tint/noise only.
                // Current lens geometry is evaluated by the opted-in client.
                options.Material.Lens=options.Material.Dispersion=0;
                options.Radius=options.Specular=0; options.ClipRounded=false;
            }
            const int padding = Request.Kind == ipc::Kind::Panel ? PanelCapturePadding(options)
                : 64 + static_cast<int>(std::clamp(Request.Material.Blur * .25, 0.0, 64.0));
            // Padding is bounded and uses 64-bit arithmetic before narrowing.
            auto expand = [padding](int value, bool add)
            { return static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(value) + (add ? padding : -padding), INT_MIN, INT_MAX)); };
            const PixelRect bitmapBounds = options.Canvas.value_or(Request.Bounds);
            const PixelRect padded{expand(bitmapBounds.Left, false), expand(bitmapBounds.Top, false),
                expand(bitmapBounds.Right, true), expand(bitmapBounds.Bottom, true)};
            try
            {
                if (worldCanvas && !Request.Visible) LastHiddenUpdate = GetTickCount64();
                const auto updateCpuScene = [&] {
                    return CaptureMeasure(PerformanceEnabled, SceneTiming,
                    [&] { if constexpr (requires { Scene.Update(top, padded, placement, PerformanceEnabled); })
                        return Scene.Update(top, padded, placement, PerformanceEnabled);
                    else return Scene.Update(top, padded, placement); }); };
                const auto recordProfile = [&](SceneUpdate const& update)
                {
                    if (!update.Profile.Enabled) return;
                    LastSceneProfile = update.Profile;
                    DiscoveryTiming.Add(update.Profile.DiscoveryMs); ReadTiming.Add(update.Profile.ReadMs);
                    ComposeTiming.Add(update.Profile.ComposeMs); AllocationTiming.Add(update.Profile.AllocationMs);
                    CoverageTiming.Add(update.Profile.CoverageMs); CompareTiming.Add(update.Profile.CompareMs);
                };
                SceneUpdate scene;
                bool gpuScene = false;
                const bool supportsPanelTexture = requires { Gpu.RenderTexture(GpuScene.View(),GpuScene.Geometry(),
                    Request.Bounds,Width,Height,options,Pixels); };
                const bool supportsTaskbarTexture = requires { Gpu.RenderTaskbarTexture(GpuScene.View(),GpuScene.Geometry(),
                    Request.Bounds,Width,Height,Request.Material,Pixels); };
                const bool taskbarPhysicalPixels = static_cast<int64_t>(Request.Bounds.Right)-Request.Bounds.Left == Width
                    && static_cast<int64_t>(Request.Bounds.Bottom)-Request.Bounds.Top == Height;
                if (gpuInput)
                {
                    if constexpr (requires { Scene.UpdateGpu(top,padded,placement,GpuScene,PerformanceEnabled); })
                    {
                        scene = CaptureMeasure(PerformanceEnabled,SceneTiming,
                            [&]{return Scene.UpdateGpu(top,padded,placement,GpuScene,PerformanceEnabled);});
                        gpuScene = scene.Ready; LastGpuSceneError = scene.Error;
                        if (!scene.Ready && !WarmingUp(scene.Error)) ++GpuSceneFailures;
                    }
                    else scene = {false,false,E_NOINTERFACE};
                }
                else if ((Request.Kind == ipc::Kind::Panel && (Request.Flags & ipc::ClientOptics) && supportsPanelTexture)
                    || (Request.Kind == ipc::Kind::Taskbar && supportsTaskbarTexture && taskbarPhysicalPixels))
                {
                    if constexpr (requires { Scene.UpdateGpu(top,padded,placement,GpuScene,PerformanceEnabled); })
                    {
                        scene = CaptureMeasure(PerformanceEnabled,SceneTiming,
                            [&]{return Scene.UpdateGpu(top,padded,placement,GpuScene,PerformanceEnabled);});
                        gpuScene = scene.Ready; LastGpuSceneError = scene.Error;
                        if (!scene.Ready && !WarmingUp(scene.Error)) { ++GpuSceneFailures; scene = updateCpuScene(); }
                    }
                    else scene = updateCpuScene();
                }
                else scene = updateCpuScene();
                recordProfile(scene);
                if (!scene.Ready)
                {
                    const HRESULT error = FAILED(scene.Error) ? scene.Error : HRESULT_FROM_WIN32(ERROR_NOT_READY);
                    Publish(WarmingUp(error) ? ipc::Status::Waiting : ipc::Status::Unavailable, error, Dirty);
                    Dirty = false;
                    return;
                }
                if (gpuInput)
                {
                    UpdateGpuBase(scene, options, bitmapBounds, GetTickCount64());
                    return;
                }
                const bool redraw = Dirty || scene.Changed || LastSceneUsedGpu != gpuScene
                    || Pixels.empty() || PublishedStatus != ipc::Status::Ready;
                if (redraw)
                {
                    bool rendered = false;
                    {
                    CapturePhaseScope rendererTiming(PerformanceEnabled, RendererTiming);
                    bool attemptedGpu = false;
                    if (Request.Kind == ipc::Kind::Panel && (Request.Flags & ipc::ClientOptics))
                    {
                        attemptedGpu = true;
                        if (gpuScene)
                        {
                            if constexpr (requires { Gpu.RenderTexture(GpuScene.View(),GpuScene.Geometry(),Request.Bounds,Width,Height,options,Pixels); })
                                rendered = CaptureMeasure(PerformanceEnabled,GpuTiming,[&] {
                                    return Gpu.RenderTexture(GpuScene.View(),GpuScene.Geometry(),Request.Bounds,Width,Height,options,Pixels); });
                        }
                        else rendered = CaptureMeasure(PerformanceEnabled, GpuTiming,
                            [&] { return Gpu.Render(Scene.Frame(), Request.Bounds, Width, Height, options, Pixels); });
                    }
                    else if (Request.Kind == ipc::Kind::Taskbar && gpuScene)
                    {
                        if constexpr (requires { Gpu.RenderTaskbarTexture(GpuScene.View(),GpuScene.Geometry(),
                            Request.Bounds,Width,Height,Request.Material,Pixels); })
                        {
                            attemptedGpu = true;
                            rendered = CaptureMeasure(PerformanceEnabled,GpuTiming,[&] {
                                return Gpu.RenderTaskbarTexture(GpuScene.View(),GpuScene.Geometry(),
                                    Request.Bounds,Width,Height,Request.Material,Pixels); });
                        }
                    }
                    if (attemptedGpu)
                    {
                        LastGpuError = rendered ? S_OK : Gpu.LastError();
                        if (rendered) { ++GpuFrames; LastFrameUsedGpu = true; }
                        else ++GpuFailures;
                    }
                    if (!rendered)
                    {
                        if (gpuScene)
                        {
                            // A texture/filter failure must reconstruct the same
                            // genuine CPU scene before invoking CPU pixel optics.
                            scene = updateCpuScene(); gpuScene = false; recordProfile(scene);
                            if (!scene.Ready)
                            { const HRESULT error=FAILED(scene.Error)?scene.Error:E_PENDING;
                                Publish(WarmingUp(error)?ipc::Status::Waiting:ipc::Status::Unavailable,error,true);
                                Dirty=false; return; }
                        }
                        rendered = CaptureMeasure(PerformanceEnabled, CpuTiming, [&] {
                            return Request.Kind == ipc::Kind::Panel
                                ? RenderPanelGlass(Scene.Frame(), Request.Bounds, Width, Height, options, Pixels)
                                : RenderGlass(Scene.Frame(), Request.Bounds, Width, Height, Request.Material, Pixels); });
                        if (rendered) { ++CpuFrames; LastFrameUsedGpu = false; }
                    }
                    }
                    if (!rendered)
                    { Publish(ipc::Status::Unavailable, E_FAIL, true); Dirty = false; return; }
                    RenderedRequest = Request;
                    RenderedBitmapBounds = bitmapBounds;
                }
                LastSceneUsedGpu=gpuScene;
                if(gpuScene)++GpuSceneFrames;else++CpuSceneFrames;
                Publish(ipc::Status::Ready, S_OK, redraw);
                Dirty = false;
            }
            catch (winrt::hresult_error const& error)
            { Publish(WarmingUp(error.code()) ? ipc::Status::Waiting : ipc::Status::Unavailable, error.code(), Dirty); Dirty = false; }
            catch (...)
            { Publish(ipc::Status::Unavailable, E_FAIL, Dirty); Dirty = false; }
        }
    };
    using Target = BasicTarget<>;
    using Targets = std::map<std::wstring, std::unique_ptr<Target>>;

    std::string PhasePerformance(CapturePhaseTiming const& timing)
    {
        return std::string("{\"last\":") + std::to_string(timing.Last) + ",\"avg\":" + std::to_string(timing.Average())
            + ",\"max\":" + std::to_string(timing.Max) + ",\"count\":" + std::to_string(timing.Count) + "}";
    }
    std::string SceneSourcesPerformance(SceneProfile const& profile)
    {
        std::string text = "[";
        for (uint32_t index = 0; index < profile.DetailCount; ++index)
        {
            const auto& source = profile.Details[index];
            text += (index ? "," : "") + std::string("{\"width\":") + std::to_string(source.Width)
                + ",\"height\":" + std::to_string(source.Height) + ",\"boundsWidth\":" + std::to_string(source.BoundsWidth)
                + ",\"boundsHeight\":" + std::to_string(source.BoundsHeight) + ",\"opacity\":" + std::to_string(source.Opacity)
                + ",\"direct\":" + (source.Direct ? "true" : "false")
                + ",\"readMs\":" + std::to_string(source.ReadMs) + ",\"composeMs\":" + std::to_string(source.ComposeMs) + "}";
        }
        return text + "]";
    }

    template<class TargetType>
    std::string BackendPerformance(TargetType const& target, uint64_t now)
    {
        return std::string("{\"tick\":") + std::to_string(now)
            + ",\"pid\":" + std::to_string(target.Request.Pid)
            + ",\"targetId\":" + std::to_string(target.Request.TargetId)
            + ",\"visible\":" + (target.Request.Visible ? "true" : "false")
            + ",\"gpuFrames\":" + std::to_string(target.GpuFrames)
            + ",\"cpuFrames\":" + std::to_string(target.CpuFrames)
            + ",\"gpuFailures\":" + std::to_string(target.GpuFailures)
            + ",\"gpuSceneFrames\":" + std::to_string(target.GpuSceneFrames)
            + ",\"cpuSceneFrames\":" + std::to_string(target.CpuSceneFrames)
            + ",\"gpuSceneFailures\":" + std::to_string(target.GpuSceneFailures)
            + ",\"lastGpuSceneError\":" + std::to_string(static_cast<int32_t>(target.LastGpuSceneError))
            + ",\"sceneBackend\":\"" + (target.LastSceneUsedGpu?"gpu":"cpu") + "\""
            + ",\"lastGpuError\":" + std::to_string(static_cast<int32_t>(target.LastGpuError))
            + ",\"profilingEnabled\":" + (target.PerformanceEnabled ? "true" : "false")
            + ",\"sceneMs\":" + PhasePerformance(target.SceneTiming)
            + ",\"sceneDiscoveryMs\":" + PhasePerformance(target.DiscoveryTiming)
            + ",\"sceneReadMs\":" + PhasePerformance(target.ReadTiming)
            + ",\"sceneComposeMs\":" + PhasePerformance(target.ComposeTiming)
            + ",\"sceneAllocationMs\":" + PhasePerformance(target.AllocationTiming)
            + ",\"sceneCoverageMs\":" + PhasePerformance(target.CoverageTiming)
            + ",\"sceneCompareMs\":" + PhasePerformance(target.CompareTiming)
            + ",\"sceneSourceCount\":" + std::to_string(target.LastSceneProfile.Sources)
            + ",\"sceneNonDirectSources\":" + std::to_string(target.LastSceneProfile.NonDirectSources)
            + ",\"sceneSources\":" + SceneSourcesPerformance(target.LastSceneProfile)
            + ",\"rendererMs\":" + PhasePerformance(target.RendererTiming)
            + ",\"gpuMs\":" + PhasePerformance(target.GpuTiming)
            + ",\"cpuMs\":" + PhasePerformance(target.CpuTiming)
            + ",\"publishMs\":" + PhasePerformance(target.PublishTiming)
            + ",\"totalUpdateMs\":" + PhasePerformance(target.UpdateTiming)
            + ",\"backend\":\"" + (!target.GpuFrames && !target.CpuFrames ? "none"
                : target.LastFrameUsedGpu ? "gpu" : "cpu") + "\"}\n";
    }
    void PublishPerformance(std::vector<std::wstring> const& roots, Targets const& targets,
        std::map<std::wstring,uint64_t>& lastChecks)
    {
        const uint64_t now = GetTickCount64();
        for (auto const& [path, target] : targets)
        {
            auto& last = lastChecks[path];
            if (last && now - last < 1000) continue;
            last = now;
            const auto enabledAt = [](std::wstring const& directory)
            { return GetFileAttributesW((std::filesystem::path(directory) / L"performance.enabled").c_str()) != INVALID_FILE_ATTRIBUTES; };
            bool enabled = enabledAt(target->Directory);
            if (!enabled) for (auto const& root : roots)
                if ((target->Directory == root || target->Directory.starts_with(root + L"\\")) && enabledAt(root))
                { enabled = true; break; }
            if (enabled != target->PerformanceEnabled)
            {
                target->PerformanceEnabled = enabled;
                target->SceneTiming = {}; target->RendererTiming = {}; target->GpuTiming = {};
                target->CpuTiming = {}; target->PublishTiming = {}; target->UpdateTiming = {};
                target->DiscoveryTiming = {}; target->ReadTiming = {}; target->ComposeTiming = {};
                target->AllocationTiming = {}; target->CoverageTiming = {}; target->CompareTiming = {};
                target->LastSceneProfile = {};
            }
            if (!enabled) continue;
            const std::string text = BackendPerformance(*target, now);
            ipc::detail::WriteSmall(ipc::detail::Path(target->Directory, target->Request.Pid,
                target->Request.TargetId, L".broker-performance.json"), text.data(), static_cast<DWORD>(text.size()));
        }
        std::erase_if(lastChecks, [&](auto const& entry) { return !targets.contains(entry.first); });
    }

    struct StatusStamp { std::string Text; uint64_t Tick = 0; };
    std::string Summary(std::wstring const& root, Targets const& targets)
    {
        unsigned active = 0, ready = 0, waiting = 0, unavailable = 0;
        HRESULT firstFailure = S_OK, waitError = E_PENDING;
        const std::wstring prefix = root + L"\\";
        for (auto const& item : targets)
        {
            auto const& target = *item.second;
            if (target.Directory != root && !target.Directory.starts_with(prefix)) continue;
            if (!target.Request.Visible) continue;
            ++active;
            if (target.CurrentStatus == ipc::Status::Ready) ++ready;
            else if (target.CurrentStatus == ipc::Status::Waiting || WarmingUp(target.CurrentError))
            { ++waiting; if (FAILED(target.CurrentError)) waitError = target.CurrentError; }
            else { ++unavailable; if (SUCCEEDED(firstFailure)) firstFailure = FAILED(target.CurrentError) ? target.CurrentError : E_FAIL; }
        }
        const char* state = !active ? "idle" : unavailable ? "unavailable" : waiting ? "waiting" : "ready";
        const HRESULT error = unavailable ? firstFailure : waiting ? waitError : S_OK;
        return std::string("state=") + state + "\nactive=" + std::to_string(active) + "\nready=" + std::to_string(ready)
            + "\nerror=" + std::to_string(static_cast<int32_t>(error)) + "\n";
    }
    void PublishSummaries(std::vector<std::wstring> const& roots, Targets const& targets,
        std::map<std::wstring, StatusStamp>& stamps, bool force = false)
    {
        const uint64_t now = GetTickCount64();
        for (auto const& root : roots)
        {
            // State directories are created by the engine/package owner. A
            // diagnostic heartbeat never creates an unrelated package tree.
            const DWORD attributes = GetFileAttributesW(root.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::string text = Summary(root, targets);
            auto& stamp = stamps[root];
            if (!force && text == stamp.Text && now - stamp.Tick < 1000) continue;
            const std::wstring path = (std::filesystem::path(root) / L"capture-status.txt").wstring();
            if (ipc::detail::WriteSmall(path, text.data(), static_cast<DWORD>(text.size())))
            { stamp.Text = text; stamp.Tick = now; }
        }
    }

    bool ShouldRestartCaptureDevice(HRESULT reason) { return FAILED(reason); }
    bool PublishDeviceFailure(HRESULT reason, std::vector<std::wstring> const& roots, Targets& targets,
        std::map<std::wstring,StatusStamp>& stamps)
    {
        if (!ShouldRestartCaptureDevice(reason)) return false;
        for (auto const& [path,target] : targets)
        {
            (void)path;
            target->Publish(ipc::Status::Unavailable,reason,true);
            // Preserve the device diagnosis even when its mapping was already
            // unavailable; publishing failure must not disguise device removal.
            target->CurrentStatus=ipc::Status::Unavailable;target->CurrentError=reason;
            target->LastGpuError=target->LastGpuSceneError=reason;
        }
        PublishSummaries(roots,targets,stamps,true);
        return true;
    }

    void ScanDirectory(std::wstring const& directory, int depth, size_t& examined, size_t& directories,
        Targets& targets, std::set<std::wstring>& seen, std::shared_ptr<SceneCapturePool> const& pool,
        std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& gpu, GpuOwnerDirectories* gpuOwners)
    {
        WIN32_FIND_DATAW data{};
        // Ask the filesystem for request candidates only. Per-client status
        // files, frame files and configuration never consume capture budget.
        const HANDLE find = FindFirstFileW((directory + L"\\capture.*.request").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = data.cFileName;
                if (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (!name.starts_with(L"capture.") || !name.ends_with(L".request")) continue;
                if (examined >= MaxRequestCandidatesPerRoot) break;
                ++examined;
                const std::wstring path = directory + L"\\" + name;
                ipc::Request request{};
                if (!ipc::detail::ReadSmall(path, &request, sizeof(request)) || !ipc::detail::ValidRequest(request, false)
                    || name != ipc::detail::Stem(request.Pid, request.TargetId) + L".request") continue;
                if (!ipc::detail::Fresh(request.HeartbeatTick))
                {
                    // A crashed client cannot cancel its files. Remove only records
                    // whose PID is demonstrably gone; a live client's next atomic
                    // heartbeat must never race a broker cleanup.
                    HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, request.Pid);
                    const bool gone = owner ? WaitForSingleObject(owner, 0) == WAIT_OBJECT_0
                        : GetLastError() == ERROR_INVALID_PARAMETER;
                    if (owner) CloseHandle(owner);
                    if (gone)
                    {
                        DeleteFileW(path.c_str());
                        DeleteFileW(ipc::detail::Path(directory, request.Pid, request.TargetId, L".response").c_str());
                    }
                    continue;
                }
                auto existing = targets.find(path);
                if (existing == targets.end())
                {
                    if (targets.size() >= MaxTargets || !OwnsWindows(request)) continue;
                    auto target = std::make_unique<Target>(pool, gpu);
                    target->Owner = OpenProcess(SYNCHRONIZE, FALSE, request.Pid);
                    if (!target->Owner || WaitForSingleObject(target->Owner, 0) != WAIT_TIMEOUT) continue;
                    target->Directory = directory; target->Request = request;
                    existing = targets.emplace(path, std::move(target)).first;
                }
                Target& target = *existing->second;
                if (WaitForSingleObject(target.Owner, 0) != WAIT_TIMEOUT) continue;
                if (gpuOwners) gpuOwners->Ensure(directory);
                seen.insert(path);
                target.Dirty = target.Dirty || NeedsRender(target.Request,request);
                target.Request = request;
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        if (depth >= 2 || examined >= MaxRequestCandidatesPerRoot || directories >= MaxBridgeDirectoriesPerRoot) return;
        const HANDLE children = FindFirstFileW((directory + L"\\bridge-*").c_str(), &data);
        if (children == INVALID_HANDLE_VALUE) return;
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (directories >= MaxBridgeDirectoriesPerRoot || examined >= MaxRequestCandidatesPerRoot) break;
            ++directories;
            ScanDirectory(directory + L"\\" + data.cFileName, depth + 1, examined, directories, targets, seen, pool, gpu, gpuOwners);
        } while (FindNextFileW(children, &data));
        FindClose(children);
    }
    void Scan(std::vector<std::wstring> const& roots, Targets& targets, std::shared_ptr<SceneCapturePool> const& pool = {},
        std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const& gpu = {}, GpuOwnerDirectories* gpuOwners = nullptr)
    {
        std::set<std::wstring> seen;
        for (auto const& root : roots)
        {
            // A busy user-data root cannot exhaust the package root's budget.
            size_t examined = 0, directories = 0;
            if (gpuOwners) gpuOwners->Ensure(root);
            ScanDirectory(root, 0, examined, directories, targets, seen, pool, gpu, gpuOwners);
        }
        for (auto it = targets.begin(); it != targets.end();)
        {
            if (!seen.contains(it->first) || !ipc::detail::Fresh(it->second->Request.HeartbeatTick)
                || WaitForSingleObject(it->second->Owner, 0) != WAIT_TIMEOUT) it = targets.erase(it);
            else ++it;
        }
        if (gpuOwners)
        {
            std::set<std::wstring> directories(roots.begin(), roots.end());
            for (auto const& [path, target] : targets) { (void)path; directories.insert(target->Directory); }
            gpuOwners->Keep(directories);
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    // All HWND rectangles and the virtual desktop must share the physical
    // coordinate system used by the per-monitor-aware shell clients.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
    std::vector<std::wstring> roots;
    DWORD parentPid = 0;
    bool once = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring argument = argv[i];
        if (argument == L"--watch-dir" && i + 1 < argc && roots.size() < 8)
        {
            std::wstring root = std::filesystem::absolute(argv[++i]).lexically_normal().wstring();
            while (root.size() > 3 && root.back() == L'\\') root.pop_back();
            roots.push_back(std::move(root));
        }
        else if (argument == L"--parent" && i + 1 < argc)
        {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(argv[++i], &end, 10);
            if (!value || !end || *end) return 2;
            parentPid = value;
        }
        else if (argument == L"--once") once = true;
        else { fwprintf(stderr, L"Unknown capture argument: %ls\n", argument.c_str()); return 2; }
    }
    if (roots.empty()) return 2;
    HANDLE parent = parentPid ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr;
    if (parentPid && !parent) return 2;
    SetConsoleCtrlHandler(ConsoleControl, TRUE);
    int result = 0;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        {
            auto gpu = GpuPanelPreprocessor::CreateSharedDevice();
            GpuOwnerDirectories gpuOwners(gpu);
            for (auto const& root : roots) gpuOwners.Ensure(root);
            auto sources = std::make_shared<SceneCapturePool>(gpu?gpu->Device.Get():nullptr,gpu?gpu->Context.Get():nullptr);
            Targets targets;
            std::map<std::wstring, StatusStamp> summaryStamps;
            std::map<std::wstring, uint64_t> performanceChecks;
            double scanTick = 0;
            CaptureScheduler scheduler;
            CapturePacer pacer;
            bool deviceFailed = false;
            const double started = CaptureNowMilliseconds();
            while (running && (!parent || WaitForSingleObject(parent, 0) == WAIT_TIMEOUT))
            {
                const double budgetStart = CaptureNowMilliseconds();
                gpu_transport::BrokerGpuInputTarget::PollDetachedRetirements();
                const HRESULT deviceHealth = gpu && gpu->Device ? gpu->Device->GetDeviceRemovedReason() : S_OK;
                if (PublishDeviceFailure(deviceHealth,roots,targets,summaryStamps))
                { deviceFailed = true; result = 1; break; }
                gpuOwners.Refresh(GetTickCount64());
                if (!scanTick || budgetStart - scanTick >= 16) { Scan(roots, targets, sources, gpu, &gpuOwners); scanTick = budgetStart; }
                std::unordered_set<HWND> excluded;
                for (auto const& [path, target] : targets)
                { (void)path; excluded.insert(reinterpret_cast<HWND>(target->Request.TopLevelHwnd)); }
                std::vector<Target*> work;
                bool visiblePanel = false, visibleGpuPanel = false;
                for (auto const& [path, target] : targets)
                {
                    (void)path;
                    target->PollGpuRetirement();
                    if (!ipc::detail::Fresh(target->Request.HeartbeatTick)
                        || WaitForSingleObject(target->Owner, 0) != WAIT_TIMEOUT)
                    { target->RetireGpuInput(); continue; }
                    work.push_back(target.get());
                    visiblePanel = visiblePanel || (target->Request.Visible && target->Request.Kind == ipc::Kind::Panel);
                    visibleGpuPanel = visibleGpuPanel || (target->Request.Visible
                        && target->Request.Kind == ipc::Kind::Panel && (target->Request.Flags & ipc::GpuBase)
                        && OwnsWindows(target->Request));
                }
                sources->EnableFrameWake(visibleGpuPanel);
                const auto now = [] { return CaptureNowMilliseconds(); };
                scheduler.Run(work.size(), budgetStart, now,
                    [&](size_t index) { return work[index]->Request.Visible != 0; },
                    [&](size_t index) { auto const& target = *work[index];
                        return !target.Request.Visible && NeedsUpdate(false, target.Dirty, GetTickCount64(), target.LastHiddenUpdate); },
                    [&](size_t index) { auto& target = *work[index];
                        if (ipc::detail::Fresh(target.Request.HeartbeatTick)
                            && WaitForSingleObject(target.Owner, 0) == WAIT_TIMEOUT) target.Update(); });
                if (scheduler.ShouldWarm(visiblePanel, now(), budgetStart))
                {
                    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    sources->Warm({x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                        y + GetSystemMetrics(SM_CYVIRTUALSCREEN)}, excluded);
                    scheduler.DidWarm(now());
                }
                PublishSummaries(roots, targets, summaryStamps);
                PublishPerformance(roots, targets, performanceChecks);
                // --once is an isolated diagnostic pass with time for initial WGC frames.
                if (once && (targets.empty() || now() - started >= 1000)) break;
                pacer.Wait(budgetStart, 16, visibleGpuPanel ? sources->FrameWakeHandle() : nullptr);
            }
            sources->EnableFrameWake(false);
            targets.clear();
            gpu_transport::BrokerGpuInputTarget::PollDetachedRetirements();
            gpuOwners.Withdraw();
            if (!deviceFailed) PublishSummaries(roots, targets, summaryStamps, true);
        } // Destroy every WGC session while this MTA is still initialized.
        winrt::uninit_apartment();
    }
    catch (...) { result = 1; }
    SetConsoleCtrlHandler(ConsoleControl, FALSE);
    if (parent) CloseHandle(parent);
    return result;
}
