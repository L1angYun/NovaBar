#define NOMINMAX
#include "../GpuInputBroker.h"
#include <cstdio>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using namespace novabar;
namespace gpu = novabar::capture::gpu;
namespace ipc = novabar::capture;
using Microsoft::WRL::ComPtr;
int checks = 0, failures = 0;
void Check(bool value, char const *name)
{
    ++checks;
    if (!value)
        ++failures;
    printf("%s: %s\n", value ? "PASS" : "FAIL", name);
}
void Hr(HRESULT value)
{
    if (FAILED(value))
        throw value;
}
struct Device
{
    ComPtr<ID3D11Device> Base;
    ComPtr<ID3D11Device5> Device5;
    ComPtr<ID3D11DeviceContext> Context;
    ComPtr<ID3D11DeviceContext4> Context4;
    LUID Adapter{};
    Device()
    {
        D3D_FEATURE_LEVEL level{};
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                             D3D11_SDK_VERSION, &Base, &level, &Context));
        Hr(Base.As(&Device5));
        Hr(Context.As(&Context4));
        ComPtr<IDXGIDevice> dx;
        Hr(Base.As(&dx));
        ComPtr<IDXGIAdapter> a;
        Hr(dx->GetAdapter(&a));
        DXGI_ADAPTER_DESC desc{};
        Hr(a->GetDesc(&desc));
        Adapter = desc.AdapterLuid;
    }
};
struct Channel
{
    std::wstring Root;
    gpu::GpuOwnerWriter Owner;
    gpu::GpuControlMapping Map;
    ipc::Request Request{};
    gpu::GpuOffer Offer{};
    ComPtr<ID3D11Texture2D> Textures[3];
    ComPtr<ID3D11Fence> Ready, Consumed;
    std::vector<HANDLE> Handles;
    HANDLE Process = nullptr;
    Channel(Device &client, Device &broker, wchar_t const *suffix = L"default",
            UINT bindings = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, UINT misc = 0x802)
    {
        Process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                              GetCurrentProcessId());
        Hr(Process ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        Root = (std::filesystem::temp_directory_path() /
                (L"novabar-gpu-broker-" + std::to_wstring(GetCurrentProcessId()) + L"-" + suffix))
                   .wstring();
        std::filesystem::create_directories(Root);
        Hr(Owner.Publish(Root, gpu::CurrentIdentity(), gpu::RandomNonce(), broker.Adapter)
               ? S_OK
               : HRESULT_FROM_WIN32(GetLastError()));
        Request.Pid = GetCurrentProcessId();
        Request.TargetId = 73;
        Request.Revision = 9;
        Request.HeartbeatTick = GetTickCount64();
        Request.CoordinateHwnd = 1;
        Request.TopLevelHwnd = 1;
        Request.Bounds = {0, 0, 16, 8};
        Request.OutputWidth = 16;
        Request.OutputHeight = 8;
        Request.Flags = ipc::HostClipsCorners | ipc::WorldCanvas | ipc::ClientOptics | ipc::GpuBase;
        Hr(Map.Create(ipc::detail::Path(Root, Request.Pid, Request.TargetId, L".gpu"), gpu::CurrentIdentity(), 73, 4)
               ? S_OK
               : HRESULT_FROM_WIN32(GetLastError()));
        Offer.TargetId = 73;
        Offer.ClientEpoch = 4;
        Offer.Generation = 11;
        Offer.RequestRevision = 9;
        Offer.Client = Offer.ExpectedBroker = gpu::CurrentIdentity();
        Offer.BrokerEpoch = Owner.Record().Nonce;
        Offer.AdapterLow = broker.Adapter.LowPart;
        Offer.AdapterHigh = broker.Adapter.HighPart;
        Offer.Width = 16;
        Offer.Height = 8;
        for (unsigned i = 0; i < 3; ++i)
        {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 16;
            d.Height = 8;
            d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = bindings;
            d.MiscFlags = misc;
            Hr(client.Base->CreateTexture2D(&d, nullptr, &Textures[i]));
            ComPtr<IDXGIResource1> resource;
            Hr(Textures[i].As(&resource));
            HANDLE handle{};
            Hr(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                            &handle));
            Handles.push_back(handle);
            Offer.TextureHandle[i] = reinterpret_cast<uint64_t>(handle);
        }
        Hr(client.Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                       reinterpret_cast<void **>(Ready.GetAddressOf())));
        Hr(client.Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                       reinterpret_cast<void **>(Consumed.GetAddressOf())));
        for (auto *f : {Ready.Get(), Consumed.Get()})
        {
            HANDLE handle{};
            Hr(f->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle));
            Handles.push_back(handle);
        }
        Offer.ReadyFenceHandle = reinterpret_cast<uint64_t>(Handles[3]);
        Offer.ConsumedFenceHandle = reinterpret_cast<uint64_t>(Handles[4]);
    }
    ~Channel()
    {
        CloseHandle(Process);
        Map.Close();
        Owner.Withdraw();
        for (auto handle : Handles)
            CloseHandle(handle);
        std::error_code error;
        std::filesystem::remove_all(Root, error);
    }
    bool Publish()
    {
        return gpu::PublishOffer(Map.Get()->Records[0], Offer);
    }
};
bool Wait(ID3D11Fence *fence, uint64_t value)
{
    const auto deadline = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline)
        Sleep(1);
    return fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value;
}
void Drain(gpu::BrokerGpuInputTarget &broker)
{
    broker.RequestRetire();
    const auto deadline = GetTickCount64() + 5000;
    while (!broker.Idle() && GetTickCount64() < deadline)
    {
        broker.PollRetirement();
        Sleep(1);
    }
    Check(broker.Idle(), "retirement completes without GPU queue wait");
}
void Basic(Device &client, Device &device)
{
    Channel channel(client, device);
    Check(channel.Publish(), "client publishes pinned five-handle offer");
    gpu::BrokerGpuInputTarget broker;
    Check(broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record()),
          "broker initializes current owner on matching device");

    Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Ready,
          "broker imports client-owned resources through local handle pulls");
    Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Imported,
          "import acknowledgement exposes no unpinned duplicate gap");
    std::optional<gpu::WritableGpuSlot> slots[3];
    for (unsigned i = 0; i < 3; ++i)
    {
        slots[i] = broker.TryAcquireWritableSlot();
        Check(slots[i] && slots[i]->Index == i, "fresh slot has bounded index");
        if (!slots[i])
            return;
        const float color[] = {0, 1, 0, 1};
        device.Context->ClearRenderTargetView(slots[i]->RenderTarget(), color);
        auto frame = broker.Publish(std::move(*slots[i]), 100 + i, {0, 0, 16, 8}, GetTickCount64());
        Check(frame && frame->SourceSerial == 100 + i && frame->Revision == 9 && frame->Generation == 11,
              "publication preserves exact reserved serial and offered identity");
        Check(frame && Wait(channel.Ready.Get(), frame->ReadyFenceValue),
              "ready signal orders broker render on client device");
    }
    Check(!broker.TryAcquireWritableSlot(), "three unconsumed publications exhaust bounded ring without waiting");
    auto &ack = channel.Map.Get()->Records[0].Consumed[0];
    Check(gpu::PublishConsumed(ack, {11, 99, 1}), "client can publish stale serial acknowledgement");
    Hr(client.Context4->Signal(channel.Consumed.Get(), 1));
    client.Context->Flush();
    Check(Wait(channel.Consumed.Get(), 1), "client consumed fence reaches stale acknowledgement");
    Check(!broker.TryAcquireWritableSlot(), "stale source serial cannot unlock input");
    Check(gpu::PublishConsumed(ack, {11, 100, 2}), "exact source serial acknowledgement accepted");
    Check(!broker.TryAcquireWritableSlot(), "acknowledgement cannot unlock incomplete consumed fence");
    Hr(client.Context4->Signal(channel.Consumed.Get(), 2));
    client.Context->Flush();
    Check(Wait(channel.Consumed.Get(), 2), "consumed signal completes");
    auto reused = broker.TryAcquireWritableSlot();
    Check(reused && reused->Index == 0, "exact serial plus completed consumed fence unlocks only consumed slot");
    if (reused)
        broker.CancelDraw(std::move(*reused));
    Drain(broker);
    Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Retired,
          "imports drop before cancellation terminal acknowledgement");
    for (auto handle : channel.Handles)
    {
        DWORD flags{};
        Check(!!GetHandleInformation(handle, &flags), "broker never closes original opaque client handle");
    }
    // Reading pixels is test-only; broker never maps inputs.
    D3D11_TEXTURE2D_DESC desc{};
    channel.Textures[1]->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Hr(client.Base->CreateTexture2D(&desc, nullptr, &staging));
    client.Context->CopyResource(staging.Get(), channel.Textures[1].Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Hr(client.Context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    auto *pixels = static_cast<uint8_t *>(mapped.pData);
    Check(pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255,
          "two-device import preserves exact broker BGRA pixels");
    client.Context->Unmap(staging.Get(), 0);
}
void Rejection(Device &client, Device &device)
{
    for (unsigned bad = 0; bad < 5; ++bad)
    {
        const auto name = L"bad-handle-" + std::to_wstring(bad);
        Channel channel(client, device, name.c_str());
        uint64_t *handles[] = {&channel.Offer.TextureHandle[0], &channel.Offer.TextureHandle[1],
                               &channel.Offer.TextureHandle[2], &channel.Offer.ReadyFenceHandle,
                               &channel.Offer.ConsumedFenceHandle};
        *handles[bad] = 0x7ffffff0ull + bad * 4;
        Check(channel.Publish(), "offer with unreachable opaque handle remains structurally valid");
        gpu::BrokerGpuInputTarget broker;
        Check(broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record()),
              "failure fixture initializes");
        DWORD before{}, after{};
        GetProcessHandleCount(GetCurrentProcess(), &before);
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "failed pull at each of five handles rejects synchronously");
        GetProcessHandleCount(GetCurrentProcess(), &after);
        Check(before == after, "partial pull failure closes every broker-local duplicate exactly");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Rejected,
              "failed import releases Claiming pin");
        Check(broker.Idle(), "failure leaves no imported generation");
        for (auto h : channel.Handles)
        {
            DWORD flags{};
            Check(!!GetHandleInformation(h, &flags), "partial failure preserves every foreign original");
        }
    }
    {
        Channel channel(client, device, L"bad-bind", D3D11_BIND_SHADER_RESOURCE);
        Check(channel.Publish(), "wrong actual bindings offered");
        gpu::BrokerGpuInputTarget broker;
        Check(broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record()),
              "descriptor fixture initializes");
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "actual descriptor without render target binding rejected");
    }
    {
        Channel channel(client, device, L"revision");
        Check(channel.Publish(), "revision fixture offered");
        channel.Request.Revision = 10;
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "current revision mismatch rejects before GPU import");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Rejected,
              "revision rejection unpins handles");
    }
    {
        Channel channel(client, device, L"wrong-birth");
        channel.Offer.Client.CreationFileTime++;
        Check(channel.Publish(), "structurally valid wrong birth offered");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "opened actual process birth rejects PID reuse identity");
    }
    {
        Channel channel(client, device, L"lost-owner");
        Check(channel.Publish(), "owner fixture offered");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        channel.Owner.Withdraw();
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Unavailable,
              "missing current owner file prohibits claims");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Offered,
              "owner failure never pins client handles");
    }
    {
        Channel channel(client, device, L"cancel");
        Check(channel.Publish(), "cancel fixture offered");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Ready,
              "cancel fixture imports");
        {
            auto lease = broker.TryAcquireWritableSlot();
            Check(!!lease, "cancel fixture leases target");
            if (lease)
            {
                const float c[] = {1, 0, 0, 1};
                device.Context->ClearRenderTargetView(lease->RenderTarget(), c);
            }
        }
        Check(channel.Ready->GetCompletedValue() != UINT64_MAX, "lease destructor seals submitted GPU commands");
        Drain(broker);
    }
}
void AliasesAndOwnership(Device &client, Device &device)
{
    for (unsigned which : {0u, 3u})
    {
        const auto name = L"alias-" + std::to_wstring(which);
        Channel channel(client, device, name.c_str());
        HANDLE alias{};
        Hr(DuplicateHandle(GetCurrentProcess(), channel.Handles[which], GetCurrentProcess(), &alias, 0, FALSE,
                           DUPLICATE_SAME_ACCESS)
               ? S_OK
               : HRESULT_FROM_WIN32(GetLastError()));
        channel.Handles.push_back(alias);
        if (which == 0)
            channel.Offer.TextureHandle[1] = reinterpret_cast<uint64_t>(alias);
        else
            channel.Offer.ConsumedFenceHandle = reinterpret_cast<uint64_t>(alias);
        Check(channel.Publish(), "different numeric handles can alias same underlying object");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "resource or fence alias cannot break independent slot ownership");
        Drain(broker);
    }
    {
        Channel channel(client, device, L"mutable-lease");
        Check(channel.Publish(), "mutable lease fixture offered");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        broker.TryImport(channel.Root, channel.Request, channel.Process);
        auto lease = broker.TryAcquireWritableSlot();
        if (lease)
        {
            lease->Index = 99;
            Check(!broker.Publish(std::move(*lease), 200, {0, 0, 16, 8}, GetTickCount64()),
                  "modified lease descriptor cannot publish invalid index");
            broker.CancelDraw(std::move(*lease));
        }
        broker.RequestRetire();
        for (unsigned i = 0; i < 100 && !broker.Idle(); ++i)
        {
            broker.PollRetirement();
            Sleep(1);
        }
        Check(broker.Idle(), "modified public metadata cannot pin private lease reservation");
    }
    {
        Channel channel(client, device, L"owner-replaced");
        Check(channel.Publish(), "replacement owner fixture offered");
        auto replacement = channel.Owner.Record();
        replacement.Nonce++;
        Hr(ipc::detail::WriteSmall(gpu::OwnerPath(channel.Root), &replacement, sizeof(replacement)) ? S_OK : E_FAIL);
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Unavailable,
              "fresh replacement nonce blocks stale importer");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Offered,
              "stale importer never claims replacement owner's offer");
    }
}
void RecoveryAndAdmission(Device &client, Device &device)
{
    for (unsigned mode = 0; mode < 8; ++mode)
    {
        const auto name = L"admission-" + std::to_wstring(mode);
        Channel channel(client, device, name.c_str());
        switch (mode)
        {
        case 0:
            channel.Offer.ExpectedBroker.CreationFileTime++;
            break;
        case 1:
            channel.Offer.BrokerEpoch++;
            break;
        case 2:
            channel.Offer.AdapterLow++;
            break;
        case 3:
            channel.Offer.Width++;
            break;
        case 4:
            channel.Offer.MiscFlags = 0;
            break;
        case 5:
            channel.Offer.DxgiFormat = 28;
            break;
        case 6:
            channel.Offer.ConsumedFenceHandle = channel.Offer.TextureHandle[0];
            break;
        case 7:
            channel.Offer.ClientEpoch++;
            break;
        }
        // Direct publication models malformed/hostile fixed wire bytes.
        channel.Map.Get()->Records[0].Offer = channel.Offer;
        InterlockedExchange64(&channel.Map.Get()->Records[0].Life.Phase, 1);
        gpu::BrokerGpuInputTarget broker;
        Check(broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record()),
              "admission fixture initializes");
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "identity LUID extent format flags handles and epoch mismatches reject");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Rejected,
              "malformed claimed offer releases ownership pin");
    }
    for (auto phase : {gpu::GpuPhase::Claiming, gpu::GpuPhase::Imported})
    {
        Channel channel(client, device, phase == gpu::GpuPhase::Claiming ? L"dead-claim" : L"dead-import");
        channel.Offer.ExpectedBroker.CreationFileTime++;
        Check(channel.Publish(), "PID reuse fixture remains structurally valid");
        Check(gpu::TryClaim(channel.Map.Get()->Records[0]), "old importer claims original handles");
        if (phase == gpu::GpuPhase::Imported)
            Check(gpu::CompleteImport(channel.Map.Get()->Records[0], S_OK), "old importer publishes Imported");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Pending,
              "recovery does not reinterpret abandoned resources as ready pixels");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Abandoned,
              "fresh current broker abandons verified mismatched PID birth owner");
    }
    {
        Channel channel(client, device, L"live-claim");
        Check(channel.Publish(), "live claimant fixture offered");
        Check(gpu::TryClaim(channel.Map.Get()->Records[0]), "live exact broker owns Claiming pin");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Pending,
              "live claimant cannot be stolen based on polling or elapsed time");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Claiming,
              "fresh same-process importer preserves exact live Claiming pin");
        gpu::CompleteImport(channel.Map.Get()->Records[0], E_ABORT);
    }
    {
        Channel channel(client, device, L"cancel-offered");
        Check(channel.Publish(), "preclaim cancellation fixture offered");
        gpu::RequestCancel(channel.Map.Get()->Records[0]);
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Pending,
              "canceled offered handles never imported");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Retired,
              "preclaim cancellation keeps terminal ownership");
    }
    {
        Channel channel(client, device, L"freshness");
        Check(channel.Publish(), "source freshness fixture offered");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        broker.TryImport(channel.Root, channel.Request, channel.Process);
        auto lease = broker.TryAcquireWritableSlot();
        Check(!!lease, "source freshness fixture leases");
        if (lease)
            Check(!broker.Publish(std::move(*lease), 100, {0, 0, 16, 8}, GetTickCount64() - 1501),
                  "expired original source tick cannot gain new freshness through GPU publication");
        Drain(broker);
    }
    {
        Channel channel(client, device, L"revision-retire");
        Check(channel.Publish(), "revision retirement fixture offered");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        broker.TryImport(channel.Root, channel.Request, channel.Process);
        auto lease = broker.TryAcquireWritableSlot();
        channel.Request.Revision++;
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Pending,
              "revision change withdraws old active generation");
        if (lease)
        {
            Check(!broker.Publish(std::move(*lease), 100, {0, 0, 16, 8}, GetTickCount64()),
                  "old revision in-flight draw cannot publish as new revision");
        }
        Drain(broker);
    }
}
void ConcurrentCancel(Device &client, Device &device)
{
    Channel channel(client, device, L"concurrent-cancel");
    gpu::BrokerGpuInputTarget broker;
    broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
    for (unsigned attempt = 0; attempt < 24; ++attempt)
    {
        if (attempt)
        {
            Check(gpu::ResetTerminal(channel.Map.Get()->Records[0], true),
                  "terminal cancellation can recycle record after safe acknowledgement");
            ++channel.Offer.Generation;
        }
        Check(channel.Publish(), "concurrent cancellation generation offered");
        std::thread cancel(
            [&]
            {
                const auto deadline = GetTickCount64() + 1000;
                while (gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Offered &&
                       GetTickCount64() < deadline)
                    std::this_thread::yield();
                gpu::RequestCancel(channel.Map.Get()->Records[0]);
            });
        const auto imported = broker.TryImport(channel.Root, channel.Request, channel.Process);
        cancel.join();
        Check(imported == gpu::BrokerImportResult::Ready || imported == gpu::BrokerImportResult::Rejected ||
                  imported == gpu::BrokerImportResult::Pending,
              "cancel races claim or import with bounded result");
        const auto deadline = GetTickCount64() + 1000;
        while (!broker.Idle() && GetTickCount64() < deadline)
        {
            broker.PollRetirement();
            Sleep(1);
        }
        Check(broker.Idle() && gpu::Terminal(gpu::Phase(channel.Map.Get()->Records[0])),
              "cancel at asynchronous claim/import boundary leaves no live Claiming pin");
    }
}
void Capacity(Device &device)
{
    auto owner = gpu::GpuOwnerRecord{};
    const auto self = gpu::CurrentIdentity();
    owner.Pid = self.Pid;
    owner.CreationFileTime = self.CreationFileTime;
    owner.Nonce = 1;
    owner.HeartbeatTick = GetTickCount64();
    owner.AdapterLow = device.Adapter.LowPart;
    owner.AdapterHigh = device.Adapter.HighPart;
    std::vector<std::unique_ptr<gpu::BrokerGpuInputTarget>> owners;
    for (unsigned i = 0; i < 64; ++i)
    {
        auto broker = std::make_unique<gpu::BrokerGpuInputTarget>();
        Check(broker->Initialize(device.Base.Get(), device.Context.Get(), owner),
              "bounded retirement owner capacity reserves before claims");
        owners.push_back(std::move(broker));
    }
    gpu::BrokerGpuInputTarget extra;
    Check(!extra.Initialize(device.Base.Get(), device.Context.Get(), owner),
          "retirement capacity exhaustion rejects allocation before any client claim");
    owners.clear();
    Check(extra.Initialize(device.Base.Get(), device.Context.Get(), owner),
          "destroyed idle owners return bounded retirement capacity");
}
void Detached(Device &client, Device &device)
{
    Channel channel(client, device, L"detached");
    Check(channel.Publish(), "detached fixture offered");
    std::optional<gpu::WritableGpuSlot> lease;
    {
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Ready,
              "detached fixture imports");
        lease = broker.TryAcquireWritableSlot();
        Check(!!lease, "lease may remain live through broker destruction");
        if (lease)
        {
            const float c[] = {0, 1, 0, 1};
            device.Context->ClearRenderTargetView(lease->RenderTarget(), c);
        }
    }
    Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Imported,
          "broker destruction preserves pending lease ownership without blocking");
    lease.reset();
    const auto deadline = GetTickCount64() + 5000;
    while (gpu::Phase(channel.Map.Get()->Records[0]) != gpu::GpuPhase::Retired && GetTickCount64() < deadline)
    {
        gpu::BrokerGpuInputTarget::PollDetachedRetirements();
        Sleep(1);
    }
    Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Retired,
          "detached owner finishes submitted writes before releasing terminal pin");
}
void WrongTypes(Device &client, Device &device)
{
    for (unsigned bad = 0; bad < 5; ++bad)
    {
        const auto name = L"wrong-type-" + std::to_wstring(bad);
        Channel channel(client, device, name.c_str());
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Hr(event ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        channel.Handles.push_back(event);
        uint64_t *handles[] = {&channel.Offer.TextureHandle[0], &channel.Offer.TextureHandle[1],
                               &channel.Offer.TextureHandle[2], &channel.Offer.ReadyFenceHandle,
                               &channel.Offer.ConsumedFenceHandle};
        *handles[bad] = reinterpret_cast<uint64_t>(event);
        Check(channel.Publish(), "non-resource kernel handle is a structurally valid opaque number");
        gpu::BrokerGpuInputTarget broker;
        broker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record());
        Check(broker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected,
              "actual OpenSharedResource or OpenSharedFence type mismatch rejects at each position");
        Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Rejected && broker.Idle(),
              "partial open failure drops prior COM imports before terminal acknowledgement");
        DWORD flags{};
        Check(!!GetHandleInformation(event, &flags), "wrong-type original client handle remains owned by client");
    }
}
// Break caught: importing a new offer while an old lease still names its
// resources lets the client reuse all five objects across independent epochs.
// This regression never draws: a held lease and zero fences expose the overlap.
void DeferredGenerationHandoff(Device &client, Device &device)
{
    for (unsigned mode : {1u, 2u, 0u})
    {
        const bool reuse = mode != 0;
        Channel first(client, device, reuse ? L"handoff-reused-a" : L"handoff-fresh-a"),
            fresh(client, device, reuse ? L"handoff-reused-b" : L"handoff-fresh-b");
        Check(first.Publish(), "old handoff generation offered");
        gpu::BrokerGpuInputTarget broker;
        Check(broker.Initialize(device.Base.Get(), device.Context.Get(), first.Owner.Record()),
              "handoff broker initializes");
        Check(broker.TryImport(first.Root, first.Request, first.Process) == gpu::BrokerImportResult::Ready,
              "old handoff generation imports");
        auto oldLease = broker.TryAcquireWritableSlot();
        Check(!!oldLease, "old generation keeps a write reservation without drawing");
        auto *oldTarget = oldLease ? oldLease->RenderTarget() : nullptr;
        auto next = reuse ? first.Offer : fresh.Offer;
        next.Generation = 12;
        next.RequestRevision = 10;
        next.BrokerEpoch = first.Owner.Record().Nonce;
        if (mode == 2)
        {
            uint64_t *values[] = {&next.TextureHandle[0], &next.TextureHandle[1], &next.TextureHandle[2],
                                  &next.ReadyFenceHandle, &next.ConsumedFenceHandle};
            for (unsigned i = 0; i < 5; ++i)
            {
                HANDLE alias{};
                Hr(DuplicateHandle(GetCurrentProcess(), first.Handles[i], GetCurrentProcess(), &alias, 0, FALSE,
                                   DUPLICATE_SAME_ACCESS)
                       ? S_OK
                       : HRESULT_FROM_WIN32(GetLastError()));
                first.Handles.push_back(alias);
                *values[i] = reinterpret_cast<uint64_t>(alias);
            }
        }
        Check(gpu::PublishOffer(first.Map.Get()->Records[1], next),
              "new offer is structurally valid before old generation drains");
        first.Request.Revision = 10;
        Check(first.Ready->GetCompletedValue() == 0 && first.Consumed->GetCompletedValue() == 0,
              "old held reservation has never signaled either shared fence");
        Check(broker.TryImport(first.Root, first.Request, first.Process) == gpu::BrokerImportResult::Pending,
              "fresh or reused offer waits while any old lease or import remains resident");
        Check(gpu::Phase(first.Map.Get()->Records[1]) == gpu::GpuPhase::Offered,
              "deferred handoff never claims or acknowledges new source handles");
        Check(gpu::Phase(first.Map.Get()->Records[0]) == gpu::GpuPhase::Imported && broker.ActiveGeneration() == 11 &&
                  broker.ActiveRevision() == 9,
              "deferred offer preserves old imported generation and metadata");
        Check(oldLease && oldLease->RenderTarget() == oldTarget && oldLease->Generation == 11 &&
                  oldLease->Revision == 9,
              "deferred handoff preserves the old outstanding lease");
        Check(!broker.TryAcquireWritableSlot(), "old retiring generation cannot issue another reservation");
        if (oldLease)
            broker.CancelDraw(std::move(*oldLease));
        Check(Wait(first.Ready.Get(), 1), "cancel seals old reservation without submitting a draw");
        const auto deadline = GetTickCount64() + 5000;
        while (!broker.Idle() && GetTickCount64() < deadline)
        {
            broker.PollRetirement();
            Sleep(1);
        }
        Check(broker.Idle() && gpu::Phase(first.Map.Get()->Records[0]) == gpu::GpuPhase::Retired,
              "old imports drain fully before new claim is permitted");
        if (reuse)
        {
            Check(broker.TryImport(first.Root, first.Request, first.Process) == gpu::BrokerImportResult::Rejected,
                  "previously used fence objects fail fresh zero-fence generation admission");
            Check(gpu::Phase(first.Map.Get()->Records[1]) == gpu::GpuPhase::Rejected && broker.Idle() &&
                      broker.ActiveGeneration() == 0 && first.Map.Get()->Records[0].Offer.Generation == 11 &&
                      first.Map.Get()->Records[0].Offer.RequestRevision == 9,
                  "reused-resource rejection cannot overwrite retired old generation metadata");
            Check(gpu::ResetTerminal(first.Map.Get()->Records[1], true),
                  "client resets rejected candidate after terminal acknowledgement");
            next = fresh.Offer;
            next.Generation = 13;
            next.RequestRevision = 10;
            next.BrokerEpoch = first.Owner.Record().Nonce;
            Check(gpu::PublishOffer(first.Map.Get()->Records[1], next),
                  "client offers genuinely fresh resources after rejected reuse");
        }
        Check(broker.TryImport(first.Root, first.Request, first.Process) == gpu::BrokerImportResult::Ready,
              "fresh zero-fence resources import after old generation fully releases");
        Check(broker.ActiveGeneration() == (reuse ? 13ull : 12ull) && broker.ActiveRevision() == 10,
              "successful handoff exposes only the accepted new generation metadata");
        Drain(broker);
    }
}
// A replacement target must obey the same handoff rule while its predecessor
// is owned by the bounded detached-retirement registry. No draw is submitted.
void DetachedGenerationHandoff(Device &client, Device &device)
{
    Channel channel(client, device, L"detached-generation-handoff");
    Check(channel.Publish(), "detached handoff old generation offered");
    std::optional<gpu::WritableGpuSlot> oldLease;
    {
        gpu::BrokerGpuInputTarget oldBroker;
        Check(oldBroker.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record()),
              "detached handoff old broker initializes");
        Check(oldBroker.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Ready,
              "detached handoff old generation imports");
        oldLease = oldBroker.TryAcquireWritableSlot();
        Check(!!oldLease, "detached old owner retains one reservation without drawing");
    }
    auto next = channel.Offer;
    next.Generation = 12;
    next.RequestRevision = 10;
    Check(gpu::PublishOffer(channel.Map.Get()->Records[1], next),
          "replacement target offers same five objects while predecessor remains pinned");
    channel.Request.Revision = 10;
    gpu::BrokerGpuInputTarget replacement;
    Check(replacement.Initialize(device.Base.Get(), device.Context.Get(), channel.Owner.Record()),
          "replacement broker reserves its own bounded retirement capacity");
    Check(replacement.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Pending,
          "replacement broker cannot bypass a detached predecessor's imported pin");
    Check(gpu::Phase(channel.Map.Get()->Records[1]) == gpu::GpuPhase::Offered &&
              gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Imported &&
              channel.Map.Get()->Records[0].Offer.Generation == 11 &&
              channel.Map.Get()->Records[0].Offer.RequestRevision == 9 && replacement.ActiveGeneration() == 0 &&
              oldLease && oldLease->RenderTarget(),
          "deferred replacement preserves old lease and both record identities");
    oldLease.reset();
    const auto deadline = GetTickCount64() + 5000;
    while (gpu::Phase(channel.Map.Get()->Records[0]) != gpu::GpuPhase::Retired && GetTickCount64() < deadline)
    {
        gpu::BrokerGpuInputTarget::PollDetachedRetirements();
        Sleep(1);
    }
    Check(gpu::Phase(channel.Map.Get()->Records[0]) == gpu::GpuPhase::Retired,
          "detached predecessor releases its pin after cancellation seal completes");
    Check(replacement.TryImport(channel.Root, channel.Request, channel.Process) == gpu::BrokerImportResult::Rejected &&
              gpu::Phase(channel.Map.Get()->Records[1]) == gpu::GpuPhase::Rejected,
          "replacement rejects reused nonzero fence after detached predecessor drains");
    Drain(replacement);
}
int main()
{
    try
    {
        Device client, broker;
        Check(client.Adapter.LowPart == broker.Adapter.LowPart && client.Adapter.HighPart == broker.Adapter.HighPart,
              "two real devices use same adapter");
        Basic(client, broker);
        Rejection(client, broker);
        AliasesAndOwnership(client, broker);
        RecoveryAndAdmission(client, broker);
        Detached(client, broker);
        ConcurrentCancel(client, broker);
        WrongTypes(client, broker);
        DeferredGenerationHandoff(client, broker);
        DetachedGenerationHandoff(client, broker);
        Capacity(broker);
    }
    catch (HRESULT e)
    {
        printf("unexpected HRESULT 0x%08lx\n", static_cast<unsigned long>(e));
        return 2;
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
