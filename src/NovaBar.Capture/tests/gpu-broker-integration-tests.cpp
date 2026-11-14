#define wmain NovaBarCaptureMain
#include "../main.cpp"
#undef wmain
#include "../CaptureFrameReader.h"
#include <cstdio>

namespace
{
namespace gpu = novabar::capture::gpu;
using Microsoft::WRL::ComPtr;
int checks = 0, failures = 0;
void Check(bool ok, char const *name)
{
    ++checks;
    if (!ok)
        ++failures;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
}
void Hr(HRESULT result)
{
    if (FAILED(result))
        throw result;
}
struct NoCaptureScene
{
    explicit NoCaptureScene(std::shared_ptr<SceneCapturePool>) {}
};
struct PrivateScene
{
    std::shared_ptr<GpuPanelPreprocessor::SharedDevice> Shared;
    PixelFrame Shape;
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11ShaderResourceView> Source;
    explicit PrivateScene(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const &shared) : Shared(shared)
    {
        Shape.Width = 64;
        Shape.Height = 48;
        Shape.Bounds = {0, 0, 64, 48};
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 64;
        d.Height = 48;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Hr(shared->Device->CreateTexture2D(&d, nullptr, &Texture));
        Hr(shared->Device->CreateShaderResourceView(Texture.Get(), nullptr, &Source));
        Color(0, 255, 0);
    }
    void Color(uint8_t b, uint8_t g, uint8_t r)
    {
        std::vector<uint8_t> pixels(64 * 48 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i] = b;
            pixels[i + 1] = g;
            pixels[i + 2] = r;
            pixels[i + 3] = 255;
        }
        Shared->Context->UpdateSubresource(Texture.Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
    }
    auto View() const
    {
        return Source.Get();
    }
    auto const &Geometry() const
    {
        return Shape;
    }
};
using TestTarget = BasicTarget<NoCaptureScene, GpuPanelPreprocessor, PrivateScene>;
struct Input
{
    gpu::GpuControlMapping Mapping;
    gpu::GpuOffer Offer;
    ComPtr<ID3D11Texture2D> Textures[3];
    ComPtr<ID3D11Fence> Ready, Consumed;
    ComPtr<ID3D11DeviceContext4> Context;
    std::vector<HANDLE> Handles;
    Input(std::wstring const &root, ipc::Request const &request, gpu::GpuOwnerRecord const &owner,
          std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const &shared)
    {
        Hr(Mapping.Create(ipc::detail::Path(root, request.Pid, request.TargetId, L".gpu"), gpu::CurrentIdentity(),
                          request.TargetId, 7)
               ? S_OK
               : HRESULT_FROM_WIN32(GetLastError()));
        Offer.TargetId = request.TargetId;
        Offer.ClientEpoch = 7;
        Offer.Generation = 17;
        Offer.RequestRevision = request.Revision;
        Offer.Client = Offer.ExpectedBroker = gpu::CurrentIdentity();
        Offer.BrokerEpoch = owner.Nonce;
        Offer.AdapterLow = owner.AdapterLow;
        Offer.AdapterHigh = owner.AdapterHigh;
        Offer.Width = request.OutputWidth;
        Offer.Height = request.OutputHeight;
        CreateResources(shared);
        Check(gpu::PublishOffer(Mapping.Get()->Records[0], Offer),
              "input fixture offers three textures and two fences");
    }
    void CreateResources(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const &shared)
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = Offer.Width;
            d.Height = Offer.Height;
            d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = 0x802;
            Hr(shared->Device->CreateTexture2D(&d, nullptr, &Textures[i]));
            ComPtr<IDXGIResource1> resource;
            Hr(Textures[i].As(&resource));
            HANDLE h{};
            Hr(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                            &h));
            Handles.push_back(h);
            Offer.TextureHandle[i] = reinterpret_cast<uint64_t>(h);
        }
        ComPtr<ID3D11Device5> device;
        Hr(shared->Device.As(&device));
        Hr(shared->Context.As(&Context));
        Hr(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&Ready)));
        Hr(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&Consumed)));
        for (auto *fence : {Ready.Get(), Consumed.Get()})
        {
            HANDLE h{};
            Hr(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h));
            Handles.push_back(h);
        }
        Offer.ReadyFenceHandle = reinterpret_cast<uint64_t>(Handles[Handles.size() - 2]);
        Offer.ConsumedFenceHandle = reinterpret_cast<uint64_t>(Handles.back());
    }
    ~Input()
    {
        for (auto h : Handles)
            CloseHandle(h);
    }
};
bool Complete(ID3D11Fence *fence, uint64_t value)
{
    const auto deadline = GetTickCount64() + 3000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline)
        Sleep(1);
    return fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value;
}
std::vector<uint8_t> Read(std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const &shared, ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Hr(shared->Device->CreateTexture2D(&d, nullptr, &staging));
    shared->Context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE m{};
    Hr(shared->Context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
    std::vector<uint8_t> result(size_t(d.Width) * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y)
        memcpy(result.data() + size_t(y) * d.Width * 4, static_cast<uint8_t *>(m.pData) + size_t(y) * m.RowPitch,
               d.Width * 4);
    shared->Context->Unmap(staging.Get(), 0);
    return result;
}
ipc::detail::Slot const &Latest(TestTarget &target)
{
    auto *view = target.Writer.ViewForTest();
    return view->Slots[view->ActiveSlot];
}
void MappingPolicy(std::wstring const &root, std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const &shared)
{
    TestTarget target({}, shared);
    target.Directory = root;
    target.Request.Pid = GetCurrentProcessId();
    target.Request.TargetId = 705;
    target.Request.Revision = 1;
    target.Request.HeartbeatTick = GetTickCount64();
    target.Request.CoordinateHwnd = target.Request.TopLevelHwnd = 1;
    target.Request.Flags = ipc::HostClipsCorners | ipc::WorldCanvas | ipc::ClientOptics | ipc::GpuBase;
    target.Request.Bounds = {0, 0, 1024, 512};
    target.Width = target.Request.OutputWidth = 1024;
    target.Height = target.Request.OutputHeight = 512;
    Check(target.EnsureWriter() && target.Writer.ViewForTest()->CapacityBytes == 65536,
          "GPU metadata mapping reserves only minimum envelope rather than a full CPU image");
    target.Request.Flags = 0;
    Check(target.EnsureWriter() && target.Writer.ViewForTest()->CapacityBytes >= 2097152,
          "explicit CPU protocol resize restores enough space for the complete pixel payload");
    target.Pixels.assign(2097152, 255);
    target.RenderedRequest = target.Request;
    target.RenderedBitmapBounds = target.Request.Bounds;
    Check(ipc::WriteRequest(root, target.Request), "CPU resize fixture publishes its explicit legacy request");
    target.Publish(ipc::Status::Ready, S_OK, true);
    ipc::FrameReader reader;
    ipc::Frame frame;
    Check(reader.ReadLatest(root, target.Request.Pid, target.Request.TargetId, 1, frame) &&
              frame.Pixels == target.Pixels && frame.Stride == 4096 && frame.PayloadFlags == 0,
          "legacy CPU main publication retains complete exact pixels after GPU-to-CPU mapping resize");
}
void Run(std::wstring const &root, std::shared_ptr<GpuPanelPreprocessor::SharedDevice> const &shared)
{
    std::filesystem::create_directories(root);
    MappingPolicy(root, shared);
    GpuOwnerDirectories owners(shared);
    Check(owners.Ensure(root), "main publishes fresh owner in exact existing client directory");
    gpu::GpuOwnerRecord owner{};
    Check(gpu::TryReadFreshGpuOwner(root, GetTickCount64(), owner),
          "published owner advertises actual current adapter");
    TestTarget target({}, shared);
    target.Directory = root;
    target.Request.Pid = GetCurrentProcessId();
    target.Request.TargetId = 704;
    target.Request.Revision = 3;
    target.Request.HeartbeatTick = GetTickCount64();
    target.Request.CoordinateHwnd = target.Request.TopLevelHwnd = 1;
    target.Request.Bounds = {8, 8, 40, 32};
    target.Request.OutputWidth = 32;
    target.Request.OutputHeight = 24;
    target.Request.Flags = ipc::HostClipsCorners | ipc::WorldCanvas | ipc::ClientOptics | ipc::GpuBase;
    target.Width = 32;
    target.Height = 24;
    target.Owner = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    Hr(target.Owner ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    Check(ipc::WriteRequest(root, target.Request),
          "headless seam writes explicit GPU request metadata without creating a window");
    Check(target.EnsureWriter(), "main frame writer maps unchanged protocol2 envelope");
    Input channel(root, target.Request, owner, shared);
    PanelGlassOptions options;
    options.Canvas = target.Request.Bounds;
    options.Viewport = {0, 0, 64, 48};
    options.ClipRounded = false;
    options.Radius = options.Specular = options.Material.Lens = options.Material.Dispersion = 0;
    options.Material.Blur = 4;
    options.Material.Noise = .02;
    options.Material.Tint = .2;
    const auto tick = GetTickCount64();
    target.UpdateGpuBase({true, true, S_OK}, options, *options.Canvas, tick);
    auto first = Latest(target);
    Check(first.Status == ipc::Status::Ready && first.PayloadFlags == (ipc::BaseCanvasPayload | ipc::GpuBasePayload) &&
              first.Stride == 0,
          "main publishes only metadata for GPU opt-in");
    Check(first.Serial && first.Generation == 17 && first.ReadyFenceValue && first.PublishedTick == tick,
          "main publishes exact reserved serial generation ready fence and validated source tick");
    Check(target.Pixels.empty() && target.GpuFrames == 1 && target.CpuFrames == 0,
          "GPU base producer never creates a CPU pixel payload");
    Check(Complete(channel.Ready.Get(), first.ReadyFenceValue),
          "metadata publication is ordered behind actual target rendering");
    std::vector<uint8_t> oracle;
    Check(target.Gpu.RenderTexture(target.GpuScene.View(), target.GpuScene.Geometry(), target.Request.Bounds, 32, 24,
                                   options, oracle),
          "test establishes existing genuine blur tint noise oracle");
    Check(Read(shared, channel.Textures[first.TextureSlot].Get()) == oracle,
          "integrated GPU base exactly matches existing filtered pixels");
    ipc::FrameReader defaultReader, gpuReader;
    ipc::Frame frame;
    Check(!defaultReader.ReadLatest(root, target.Request.Pid, target.Request.TargetId, 3, frame),
          "legacy reader continues rejecting GPU metadata by default");
    Check(gpuReader.ReadLatest(root, target.Request.Pid, target.Request.TargetId, 3, frame, 0, true) &&
              frame.Pixels.empty(),
          "explicit GPU reader accepts metadata with no pixel vector");
    target.Published = GetTickCount64() - 101;
    const auto renewed = GetTickCount64();
    target.UpdateGpuBase({true, false, S_OK}, options, *options.Canvas, renewed);
    auto same = Latest(target);
    Check(same.Serial == first.Serial && same.ReadyFenceValue == first.ReadyFenceValue &&
              same.PublishedTick == renewed && target.GpuFrames == 1,
          "unchanged revalidated scene renews only exact tuple without redrawing or reserving serial");
    auto advertised = same;
    for (uint64_t consumed = 1; consumed <= gpu::TextureSlotCount + 2; ++consumed)
    {
        // The reader is paused. Only active metadata will be available when
        // it returns; it cannot acknowledge a superseded predecessor.
        target.GpuScene.Color(255, 0, 0);
        target.UpdateGpuBase({true, true, S_OK}, options, *options.Canvas, GetTickCount64());
        target.GpuScene.Color(0, static_cast<uint8_t>(20 * consumed), 255);
        target.UpdateGpuBase({true, true, S_OK}, options, *options.Canvas, GetTickCount64());
        gpuReader.ReadLatest(root, target.Request.Pid, target.Request.TargetId, 3, frame, 0, true);
        Check(frame.Status == ipc::Status::Ready && frame.Generation == advertised.Generation &&
                  frame.Serial == advertised.Serial && frame.TextureSlot == advertised.TextureSlot &&
                  frame.ReadyFenceValue == advertised.ReadyFenceValue && frame.PublishedTick == advertised.PublishedTick,
              "paused reader can still see the exact advertised tuple after two changed scenes");
        Check(target.GpuScenePending, "newest scene stays pending until the advertised slot completes");
        Check(Complete(channel.Ready.Get(), frame.ReadyFenceValue), "advertised texture is ready for the late client");
        // Model an exact client acknowledgement whose queued signal has not
        // completed yet. The still-old per-slot ack on reuse was also pending.
        Check(gpu::PublishConsumed(channel.Mapping.Get()->Records[0].Consumed[frame.TextureSlot],
                                   {frame.Generation, frame.Serial, consumed}),
              "late client acknowledges only the tuple returned by the active reader");
        target.UpdateGpuBase({true, false, S_OK}, options, *options.Canvas, GetTickCount64());
        Check(Latest(target).Serial == frame.Serial && Latest(target).PublishedTick == frame.PublishedTick,
              "exact acknowledgement cannot advance publication until its fence completes");
        Hr(channel.Context->Signal(channel.Consumed.Get(), consumed));
        shared->Context->Flush();
        Check(Complete(channel.Consumed.Get(), consumed), "client consumed fence actually completes");
        const auto catchupTick = GetTickCount64();
        target.UpdateGpuBase({true, false, S_OK}, options, *options.Canvas, catchupTick);
        advertised = Latest(target);
        Check(advertised.Serial == frame.Serial + 1 && advertised.Generation == first.Generation &&
                  advertised.PublishedTick == catchupTick && target.GpuFrames == consumed + 1,
              "catch-up publishes exactly one newly validated scene without generation churn");
        Check(Complete(channel.Ready.Get(), advertised.ReadyFenceValue) &&
                  target.Gpu.RenderTexture(target.GpuScene.View(), target.GpuScene.Geometry(), target.Request.Bounds,
                                           32, 24, options, oracle) &&
                  Read(shared, channel.Textures[advertised.TextureSlot].Get()) == oracle,
              "catch-up texture contains the newest scene rather than the intermediate change");
    }
    target.GpuFrame->PublishedTick = GetTickCount64() - ipc::HeartbeatTimeoutMs - 1;
    target.GpuScene.Color(255, 0, 255);
    target.UpdateGpuBase({true, true, S_OK}, options, *options.Canvas, GetTickCount64());
    Check(Latest(target).Status != ipc::Status::Ready && gpu::Cancelled(channel.Mapping.Get()->Records[0]),
          "TTL-stuck advertised publication retires truthfully to permit safe reoffer");
    Check(Complete(channel.Ready.Get(), advertised.ReadyFenceValue), "stuck generation's submitted writes complete");
    target.PollGpuRetirement();
    Check(gpu::Phase(channel.Mapping.Get()->Records[0]) == gpu::GpuPhase::Retired,
          "TTL retirement releases the old generation only after writes quiesce");
    Check(gpu::ResetTerminal(channel.Mapping.Get()->Records[0], true), "client can reclaim the terminal generation");
    ++channel.Offer.Generation;
    channel.CreateResources(shared);
    Check(gpu::PublishOffer(channel.Mapping.Get()->Records[0], channel.Offer),
          "client safely reoffers fresh input objects");
    target.UpdateGpuBase({true, false, S_OK}, options, *options.Canvas, GetTickCount64());
    auto recovered = Latest(target);
    Check(recovered.Status == ipc::Status::Ready && recovered.Generation == 18 && recovered.Serial > advertised.Serial,
          "new input generation forces genuine render even when the source observation is unchanged");
    target.UpdateGpuBase({false, false, E_ACCESSDENIED}, options, *options.Canvas, GetTickCount64());
    Check(Latest(target).Status == ipc::Status::Unavailable && target.CurrentError == E_ACCESSDENIED &&
              !target.GpuFrame,
          "source failure retires GPU transport and publishes truthful unavailable status");
    const auto oldNonce = owner.Nonce;
    Sleep(260);
    owners.Refresh(GetTickCount64());
    Check(gpu::TryReadFreshGpuOwner(root, GetTickCount64(), owner) && owner.Nonce == oldNonce &&
              owner.HeartbeatTick > tick,
          "owner heartbeat refresh preserves the exact broker identity and nonce");
    const auto absent = (std::filesystem::path(root) / L"absent-package-tree").wstring();
    Check(!owners.Ensure(absent) && !std::filesystem::exists(absent),
          "owner publication never creates an unrelated directory");
    gpu::BrokerGpuInputTarget::PollDetachedRetirements();
}
} // namespace
int main()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto shared = GpuPanelPreprocessor::CreateSharedDevice();
        Check(bool(shared), "headless integration uses genuine hardware preprocessor");
        if (!shared)
            return 1;
        auto root =
            (std::filesystem::temp_directory_path() / (L"novabar-gpu-main-" + std::to_wstring(GetCurrentProcessId())))
                .wstring();
        Run(root, shared);
        gpu::BrokerGpuInputTarget::PollDetachedRetirements();
        std::error_code error;
        std::filesystem::remove_all(root, error);
        winrt::uninit_apartment();
    }
    catch (HRESULT error)
    {
        printf("HRESULT=%08lx\n", static_cast<unsigned long>(error));
        return 2;
    }
    catch (winrt::hresult_error const &error)
    {
        printf("WinRT=%08lx\n", static_cast<unsigned long>(error.code().value));
        return 2;
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
