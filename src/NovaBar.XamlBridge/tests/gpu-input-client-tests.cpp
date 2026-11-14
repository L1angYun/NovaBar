#include "../GpuInputClient.h"
#include <cstdio>
#include <thread>
#include <chrono>

namespace gpu = novabar::capture::gpu;
int checks = 0, failures = 0;
void Check(bool value, char const* label) { ++checks; if (!value) { ++failures; printf("FAIL: %s\n", label); } }
gpu::GpuOffer FixtureOffer() {
    gpu::GpuOffer o; o.TargetId = 72; o.ClientEpoch = 4; o.Generation = 9; o.RequestRevision = 6;
    o.Client = o.ExpectedBroker = gpu::CurrentIdentity(); o.AdapterLow = 11; o.BrokerEpoch = 37;
    o.Width = 16; o.Height = 8; for (int i = 0; i < 3; ++i) o.TextureHandle[i] = 101 + i;
    o.ReadyFenceHandle = 104; o.ConsumedFenceHandle = 105; return o;
}
gpu::GpuExpectedFrame FixtureFrame(uint64_t serial = 1) { return {6, 9, serial, serial, 0, 16, 8, {-8, 2, 8, 10}, 10000}; }
void MetadataTests() {
    gpu::input_detail::MetadataGate gate; gate.Reset(FixtureOffer()); auto first = FixtureFrame();
    Check(gate.Observe(first, 10000) && !gate.Ready(10000), "uncompleted first publication never exposes a private cache");
    Check(gate.Commit(first) && gate.Ready(10000), "cache accepts only the observed publication after GPU copy is queued");
    auto pending = FixtureFrame(3); pending.PublishedTick = 11000;
    Check(gate.Observe(pending, 11000) && gate.Accepted().SourceSerial == 1, "new pending fence leaves the accepted serial unchanged");
    Check(!gate.Ready(11501), "new pending metadata does not renew the old accepted source TTL");
    Check(!gate.Observe(FixtureFrame(2), 11000), "skipped publication cannot move the observed serial backwards");
    Check(gate.Commit(pending) && gate.Ready(11000), "newest completed skipped frame becomes private cache");
    auto heartbeat = pending; heartbeat.PublishedTick = 12000;
    Check(gate.Renew(heartbeat, 12000) && gate.Accepted().PublishedTick == 12000, "exact accepted serial heartbeat renews its own timestamp");
    auto next = FixtureFrame(4); next.PublishedTick = 12500; Check(gate.Observe(next, 12500), "next pending metadata is admitted");
    heartbeat.PublishedTick = 12500;
    Check(!gate.Renew(heartbeat, 12500) && gate.Accepted().PublishedTick == 12000, "backward old heartbeat cannot renew while a newer serial is pending");
    for (int field = 0; field < 12; ++field) {
        auto bad = next;
        switch (field) { case 0: ++bad.Revision; break; case 1: ++bad.Generation; break; case 2: bad.SourceSerial = 2; break;
        case 3: bad.ReadyFenceValue = 0; break; case 4: bad.ReadyFenceValue = UINT64_MAX; break; case 5: bad.TextureSlot = 3; break;
        case 6: ++bad.Width; break; case 7: ++bad.Height; break; case 8: bad.BitmapBounds.Right = bad.BitmapBounds.Left; break;
        case 9: bad.PublishedTick = 12501; break; case 10: bad.PublishedTick = 10000; break; case 11: ++bad.BitmapBounds.Left; break; }
        Check(!gate.Observe(bad, 12500), "wrong identity, serial, ready value, slot, geometry or source freshness rejects before copy");
    }
    auto same = next; ++same.ReadyFenceValue;
    Check(!gate.Observe(same, 12500), "same serial with altered ready value cannot replace an observation");
    gate.Clear(); Check(!gate.Ready(12500) && !gate.Accepted().SourceSerial, "source unavailable withdraws accepted private cache metadata");
    auto changed = FixtureOffer(); changed.ClientEpoch = 5; changed.Generation = 10; gate.Reset(changed);
    Check(!gate.Observe(first, 10000), "reset rejects old generation even when source serial is unchanged");
    first.Generation = 10;
    Check(gate.Observe(first, 10000) && gate.Commit(first), "new epoch/generation can reconnect with an unchanged source serial");
}

void MetadataAdmissionTests() {
    auto offer=FixtureOffer();offer.Generation=10;auto frame=FixtureFrame();
    using Admission=gpu::GpuMetadataAdmission;
    const auto classify=[&]{return gpu::input_detail::ClassifyMetadata(frame,offer,9,100,102,10000);};
    Check(classify()==Admission::Obsolete,"previous issued generation is obsolete rather than a replacement failure");
    frame.Generation=10;Check(classify()==Admission::Current,"current offer metadata is bound before import");
    frame.Generation=11;Check(classify()==Admission::Invalid,"unissued future generation is invalid");
    frame.Generation=8;Check(classify()==Admission::Invalid,"unissued generation before first publication is invalid");
    frame.Generation=101;Check(classify()==Admission::Obsolete,"known preceding transport namespace survives internal reset");
    ++frame.Revision;Check(classify()==Admission::Invalid,"obsolete generation cannot disguise wrong revision");--frame.Revision;
    ++frame.Width;Check(classify()==Admission::Invalid,"obsolete generation cannot disguise wrong extent");--frame.Width;
    frame.PublishedTick=10001;Check(classify()==Admission::Invalid,"obsolete generation cannot disguise future source timestamp");
    frame.PublishedTick=1;Check(classify()==Admission::Invalid,"obsolete generation cannot disguise expired source timestamp");
    frame.PublishedTick=10000;frame.Generation=103;Check(classify()==Admission::Invalid,"unknown preceding-channel generation is invalid");
}
void OwnershipTests() {
    for (auto phase : {gpu::GpuPhase::Offered, gpu::GpuPhase::Claiming, gpu::GpuPhase::Imported}) {
        gpu::GpuControl c; gpu::InitializeControl(c, gpu::CurrentIdentity(), 72, 4);
        auto& r = c.Records[0]; Check(gpu::PublishOffer(r, FixtureOffer()), "immutable offer published for cancellation test");
        if (phase != gpu::GpuPhase::Offered) Check(gpu::TryClaim(r), "broker claims cancellation test record");
        if (phase == gpu::GpuPhase::Imported) Check(gpu::CompleteImport(r, S_OK), "broker completes cancellation test import");
        gpu::RequestCancel(r);
        Check(gpu::input_detail::Disposable(r, true, false) == (phase == gpu::GpuPhase::Offered), "cancel before claim disposes; claiming/imported original handles remain pinned");
        Check(!gpu::input_detail::Disposable(r, false, false), "uncompleted client GPU read prevents retirement even after broker acknowledgement");
        if (phase == gpu::GpuPhase::Claiming) Check(gpu::CompleteImport(r, E_ABORT), "cancelled importer acknowledges after stopping all duplication");
        if (phase == gpu::GpuPhase::Imported) Check(gpu::CompleteRetirement(r), "imported owner acknowledges only after dropping imports and completing writes");
        Check(gpu::input_detail::Disposable(r, true, false), "terminal broker ack plus completed client reads allows exact local cleanup");
    }
    gpu::GpuControl c; gpu::InitializeControl(c, gpu::CurrentIdentity(), 72, 4);
    for (int i = 0; i < 2; ++i) { auto o = FixtureOffer(); o.Generation += i; gpu::PublishOffer(c.Records[i], o); gpu::TryClaim(c.Records[i]); gpu::RequestCancel(c.Records[i]); }
    Check(gpu::FindEmptyRecord(c) == -1, "resize/stop with two claimed records stops allocating instead of overwriting handles");
    auto& r = c.Records[0]; Check(!gpu::input_detail::Disposable(r, true, true), "device removal cannot bypass unacknowledged broker claim");
    gpu::Transition(r, gpu::GpuPhase::Claiming, gpu::GpuPhase::Abandoned);
    Check(gpu::input_detail::Disposable(r, false, true), "exact dead-owner abandonment plus device removal releases pending GPU reads");
    gpu::PublishConsumed(c.Records[1].Consumed[0], {10, 4, 9});
    Check(!gpu::CanReuseSlot(c.Records[1].Consumed[0], 10, 5, 9), "old serial acknowledgement never releases a later publication in the same slot");
    gpu::input_detail::RetirementRegistry bounded;
    Check(bounded.Reserve(512ull * 1024 * 1024 - 1) && !bounded.Reserve(2) && bounded.Reserve(1) && !bounded.Reserve(1),
        "hard shared-input byte budget rejects overflow before resource creation");
}

template<class Predicate> bool Until(Predicate predicate) {
    for (int i = 0; i < 500; ++i) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    return false;
}
bool HandleAlive(uint64_t value) { DWORD flags = 0; return !!GetHandleInformation(reinterpret_cast<HANDLE>(value), &flags); }
struct Devices {
    winrt::com_ptr<ID3D11Device> Client, Broker;
    winrt::com_ptr<ID3D11DeviceContext> ClientContext, BrokerContext;
    winrt::com_ptr<ID3D11Device5> Client5, Broker5;
    winrt::com_ptr<ID3D11DeviceContext4> Client4, Broker4;
    gpu::GpuOwnerRecord Owner;
    Devices() {
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, Client.put(), nullptr, ClientContext.put()));
        auto dxgi = Client.as<IDXGIDevice>(); winrt::com_ptr<IDXGIAdapter> adapter; winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, Broker.put(), nullptr, BrokerContext.put()));
        Client5 = Client.as<ID3D11Device5>(); Broker5 = Broker.as<ID3D11Device5>();
        Client4 = ClientContext.as<ID3D11DeviceContext4>(); Broker4 = BrokerContext.as<ID3D11DeviceContext4>();
        const auto id = gpu::CurrentIdentity(); const auto luid = gpu::input_detail::DeviceAdapter(Client.get());
        Owner.Pid = id.Pid; Owner.CreationFileTime = id.CreationFileTime; Owner.Nonce = 37; Owner.HeartbeatTick = GetTickCount64();
        Owner.AdapterLow = luid.LowPart; Owner.AdapterHigh = luid.HighPart;
    }
    void Refresh() { Owner.HeartbeatTick = GetTickCount64(); }
};
struct Imports {
    std::array<winrt::com_ptr<ID3D11Texture2D>, 3> Texture;
    std::array<winrt::com_ptr<ID3D11RenderTargetView>, 3> Target;
    winrt::com_ptr<ID3D11Fence> Ready, Consumed;
    Imports(Devices& d, gpu::GpuOffer const& offer) {
        gpu::input_detail::Handle verifiedClient;
        *verifiedClient.Put() = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, GetCurrentProcessId());
        winrt::check_bool(verifiedClient.Get());
        for (int i = 0; i < 5; ++i) {
            gpu::LocalHandle duplicate;
            const uint64_t value = i < 3 ? offer.TextureHandle[i] : i == 3 ? offer.ReadyFenceHandle : offer.ConsumedFenceHandle;
            winrt::check_bool(duplicate.Pull(verifiedClient.Get(), value));
            if (i < 3) {
                winrt::check_hresult(d.Broker5->OpenSharedResource1(duplicate.Get(), __uuidof(ID3D11Texture2D), Texture[i].put_void()));
                winrt::check_hresult(d.Broker->CreateRenderTargetView(Texture[i].get(), nullptr, Target[i].put()));
            } else winrt::check_hresult(d.Broker5->OpenSharedFence(duplicate.Get(), __uuidof(ID3D11Fence), i == 3 ? Ready.put_void() : Consumed.put_void()));
        }
    }
    void Paint(Devices& d, int slot, float r, float g, float b, uint64_t ready) {
        float color[]{r, g, b, 1}; d.BrokerContext->ClearRenderTargetView(Target[slot].get(), color);
        if (ready) winrt::check_hresult(d.Broker4->Signal(Ready.get(), ready)); d.BrokerContext->Flush();
    }
};
bool Pixel(Devices& d, gpu::GpuPrivateBaseCache const& cache, uint32_t expected) {
    if (!cache.Texture()) return false;
    D3D11_TEXTURE2D_DESC desc{}; cache.Texture()->GetDesc(&desc); desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging; winrt::check_hresult(d.Client->CreateTexture2D(&desc, nullptr, staging.put()));
    d.ClientContext->CopyResource(staging.get(), cache.Texture()); D3D11_MAPPED_SUBRESOURCE map{};
    winrt::check_hresult(d.ClientContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map));
    bool matches = true;
    for (uint32_t y = 0; y < desc.Height; ++y) for (uint32_t x = 0; x < desc.Width; ++x)
        if (reinterpret_cast<uint32_t const*>(static_cast<uint8_t const*>(map.pData) + y * map.RowPitch)[x] != expected) matches = false;
    d.ClientContext->Unmap(staging.get(), 0); return matches;
}
void GpuCopyTests(Devices& d, std::wstring const& directory) {
    gpu::ClientGpuInputChannel channel; gpu::GpuPrivateBaseCache cache; d.Refresh();
    Check(!channel.Initialize(directory, 72, 4, d.Client.get(), d.BrokerContext.get()), "foreign-device immediate context is rejected before mapping creation");
    Check(channel.Initialize(directory, 72, 4, d.Client.get(), d.ClientContext.get()), "channel accepts caller-owned matching device and immediate context");
    bool foreignThreadOffered = false;
    std::thread wrongThread([&] { foreignThreadOffered = channel.Offer(d.Owner, 6, 16, 8); }); wrongThread.join();
    Check(!foreignThreadOffered, "shared immediate-context work is rejected off the channel owner thread");
    Check(channel.Offer(d.Owner, 6, 16, 8), "client exports three shared BGRA inputs and two NT shared fences");
    gpu::GpuControlMapping mapping; Check(mapping.Open(channel.ControlPath()), "normal importer opens existing-directory RW control mapping");
    auto& control = *mapping.Get(); int index = gpu::FindEmptyRecord(control) == 1 ? 0 : 1; auto& record = control.Records[index];
    auto offer = record.Offer;
    auto candidate=FixtureFrame();candidate.Generation=offer.Generation;candidate.PublishedTick=GetTickCount64();
    Check(channel.AdmitMetadata(candidate,candidate.PublishedTick)==gpu::GpuMetadataAdmission::Current,"metadata binds to actual active offer while Offered");
    Check(gpu::TryClaim(record), "broker wins offer claim before importing original handles");
    Check(channel.AdmitMetadata(candidate,candidate.PublishedTick)==gpu::GpuMetadataAdmission::Current,"metadata binds to actual active offer while Claiming");
    auto future=candidate;++future.Generation;
    Check(channel.AdmitMetadata(future,candidate.PublishedTick)==gpu::GpuMetadataAdmission::Invalid&&!gpu::Cancelled(record),"unissued generation rejects truthfully without mutating channel lifecycle");
    Imports imports(d, offer); Check(gpu::CompleteImport(record, S_OK), "all broker-local shared resource imports complete");
    const uint64_t tick = GetTickCount64(); auto frame = FixtureFrame(); frame.Generation = offer.Generation; frame.PublishedTick = tick;
    Check(channel.TryCopyLatest(frame, cache, tick) == gpu::GpuInputReadResult::Pending && !cache.Ready(tick), "unsignalled ready fence never exposes uninitialized private texture");
    imports.Paint(d, 0, 1, 0, 0, 1); Check(Until([&] { return imports.Ready->GetCompletedValue() >= 1; }), "producer GPU completes ready signal");
    Check(channel.TryCopyLatest(frame, cache, tick) == gpu::GpuInputReadResult::Copied && cache.Ready(tick), "completed source copies and accepts private cache on supplied immediate context");
    D3D11_TEXTURE2D_DESC privateDesc{}; cache.Texture()->GetDesc(&privateDesc);
    Check(privateDesc.MiscFlags == 0 && privateDesc.BindFlags == D3D11_BIND_SHADER_RESOURCE && privateDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
        "optics cache is a private BGRA SRV without shared alias flags");
    Check(Pixel(d, cache, 0xffff0000), "every GPU-copied private pixel equals producer red without transport readback");
    gpu::GpuConsumedAck ack{}; Check(gpu::ReadConsumed(record.Consumed[0], ack) && ack.Generation == offer.Generation && ack.SourceSerial == 1 && ack.FenceValue == 1,
        "consumed acknowledgement names exact generation, publication serial and queued fence value");
    Check(Until([&] { return imports.Consumed->GetCompletedValue() >= ack.FenceValue; })
        && gpu::CanReuseSlot(record.Consumed[0], offer.Generation, 1, imports.Consumed->GetCompletedValue()), "consumed GPU completion permits broker input reuse");
    imports.Paint(d, 0, 0, 1, 0, 0);
    Check(channel.TryCopyLatest(frame, cache, tick) == gpu::GpuInputReadResult::Unchanged && Pixel(d, cache, 0xffff0000),
        "same serial renders retained private red after broker overwrites released shared input green");
    auto next = frame; next.SourceSerial = next.ReadyFenceValue = 3; next.TextureSlot = 1; next.PublishedTick = tick + 1000;
    Check(channel.TryCopyLatest(next, cache, tick + 1000) == gpu::GpuInputReadResult::Pending && !cache.Ready(tick + 1501)
        && cache.Metadata().PublishedTick == tick, "new pending GPU publication cannot renew old red cache freshness");
    auto old = frame; old.PublishedTick = tick + 1000;
    Check(!channel.RenewAcceptedMetadata(old, cache, tick + 1000), "older accepted heartbeat cannot bypass newer pending serial floor");
    imports.Paint(d, 1, 0, 0, 1, 3); Check(Until([&] { return imports.Ready->GetCompletedValue() >= 3; }), "skipped source publication completes producer ready fence");
    Check(channel.TryCopyLatest(next, cache, tick + 1000) == gpu::GpuInputReadResult::Copied && Pixel(d, cache, 0xff0000ff), "newest skipped serial copies exact blue into retained private texture");
    auto heartbeat = next; heartbeat.PublishedTick = tick + 2000;
    Check(channel.RenewAcceptedMetadata(heartbeat, cache, tick + 2000) && cache.Metadata().PublishedTick == tick + 2000,
        "validated exact accepted metadata heartbeat renews without re-reading shared input");
    auto backward = frame; backward.SourceSerial = backward.ReadyFenceValue = 2; backward.PublishedTick = tick + 2000;
    Check(channel.TryCopyLatest(backward, cache, tick + 2000) == gpu::GpuInputReadResult::Unavailable && !cache.Texture(), "backwards serial withdraws private cache before any GPU read");
    Check(channel.TryCopyLatest(heartbeat, cache, tick + 2000) == gpu::GpuInputReadResult::Unavailable && !cache.Texture(), "cleared cache never resamples previously released same-serial input");
    auto recovery = heartbeat; recovery.SourceSerial = recovery.ReadyFenceValue = 4; recovery.TextureSlot = 2;
    imports.Paint(d, 2, 0, 1, 0, 4); Check(Until([&] { return imports.Ready->GetCompletedValue() >= 4; }), "recovery publication becomes GPU ready");
    winrt::com_ptr<ID3D11Predicate> predicate; D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    winrt::check_hresult(d.Client->CreatePredicate(&query, predicate.put()));
    d.ClientContext->Begin(predicate.get()); d.ClientContext->End(predicate.get()); d.ClientContext->Flush(); BOOL visible = TRUE;
    Check(Until([&] { return d.ClientContext->GetData(predicate.get(), &visible, sizeof(visible), 0) == S_OK; }) && !visible,
        "test predicate resolves false before testing unconditional cache copy");
    d.ClientContext->SetPredication(predicate.get(), FALSE);
    const auto recovered = channel.TryCopyLatest(recovery, cache, tick + 2000);
    winrt::com_ptr<ID3D11Predicate> restored; BOOL predicateValue = TRUE; d.ClientContext->GetPredication(restored.put(), &predicateValue);
    d.ClientContext->SetPredication(nullptr, FALSE);
    Check(recovered == gpu::GpuInputReadResult::Copied && Pixel(d, cache, 0xff00ff00), "new source serial copies exact green even when inherited false predication would skip CopyResource");
    Check(restored.get() == predicate.get() && !predicateValue, "private cache copy restores inherited caller predication state");
    ++record.Offer.BrokerEpoch;
    Check(channel.TryCopyLatest(recovery, cache, tick + 2000) == gpu::GpuInputReadResult::Unavailable && !cache.Ready(tick + 2000), "mutated owner nonce payload rejects and clears presentation cache");
    record.Offer.BrokerEpoch = offer.BrokerEpoch;
    channel.RequestRetire(); Check(gpu::Cancelled(record), "client stop sets immutable generation cancellation before detach");
    Check(gpu::CompleteRetirement(record), "normal broker acknowledges cancelled imported generation");
    channel.PollRetirement(); channel.Reset(); mapping.Close();
    Check(Until([] { return gpu::ClientGpuInputChannel::RetirementBytes() == 0; }), "normal GPU copy retirement returns every reserved shared byte");
    Check(Until([] { return gpu::ClientGpuInputChannel::RetirementChannels() == 0; }),"old channel mapping drains before same-path namespace replacement");
    d.Refresh();Check(channel.Initialize(directory,72,5,d.Client.get(),d.ClientContext.get())&&channel.Offer(d.Owner,6,16,8),"same client object can replace its input transport namespace");
    auto priorNamespace=frame;priorNamespace.PublishedTick=GetTickCount64();
    Check(channel.AdmitMetadata(priorNamespace,priorNamespace.PublishedTick)==gpu::GpuMetadataAdmission::Obsolete,
        "known previous channel metadata cannot cancel a replacement offered after internal reset");
    channel.Reset();
}

void DeviceAdmissionTests(Devices& d, std::wstring const& directory) {
    gpu::ClientGpuInputChannel channel; winrt::com_ptr<ID3D11DeviceContext> deferred;
    winrt::check_hresult(d.Client->CreateDeferredContext(0, deferred.put()));
    Check(!channel.Initialize(directory, 94, 1, d.Client.get(), deferred.get()), "deferred context cannot satisfy the shared immediate-context ordering contract");
    winrt::com_ptr<ID3D11Device> single; winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED, nullptr, 0, D3D11_SDK_VERSION, single.put(), nullptr, context.put()));
    Check(!channel.Initialize(directory, 94, 1, single.get(), context.get()), "single-threaded device is rejected because detached worker must observe completion and release COM resources");
}

void StopAndPressureTests(Devices& d, std::wstring const& directory) {
    Check(Until([] { return gpu::ClientGpuInputChannel::RetirementChannels() == 0; }), "completed channel releases its process registry slot");
    gpu::GpuControlMapping mapping; uint64_t handles[2][5]{};
    {
        auto channel = std::make_unique<gpu::ClientGpuInputChannel>(); d.Refresh();
        Check(channel->Initialize(directory, 73, 4, d.Client.get(), d.ClientContext.get()) && channel->Offer(d.Owner, 6, 16, 8), "first pressure generation allocated within bounded owner");
        Check(mapping.Open(channel->ControlPath()), "pressure importer retains RW mapping across client destruction"); auto& control = *mapping.Get();
        Check(gpu::TryClaim(control.Records[0]), "first pressure offer remains in Claiming");
        Check(channel->Offer(d.Owner, 7, 32, 8) && gpu::TryClaim(control.Records[1]), "resize preserves claimed old record and offers one new generation");
        for (int i = 0; i < 2; ++i) {
            auto const& o = control.Records[i].Offer; for (int j = 0; j < 3; ++j) handles[i][j] = o.TextureHandle[j];
            handles[i][3] = o.ReadyFenceHandle; handles[i][4] = o.ConsumedFenceHandle;
        }
        const auto bytes = gpu::ClientGpuInputChannel::RetirementBytes();
        Check(!channel->Offer(d.Owner, 8, 48, 8) && channel->LastError() == E_PENDING && gpu::ClientGpuInputChannel::RetirementBytes() == bytes,
            "two unacknowledged records reject third resize without allocation or handle reuse");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(130));
    for (auto const& generation : handles) for (auto handle : generation) Check(HandleAlive(handle), "Stop/destructor and retirement timer preserve each claimed original NT handle");
    gpu::ClientGpuInputChannel reconnect;
    Check(!reconnect.Initialize(directory, 73, 5, d.Client.get(), d.ClientContext.get()), "same path reconnect cannot overwrite a detached predecessor's claimed mapping");
    for (auto& record : mapping.Get()->Records) Check(gpu::CompleteImport(record, E_ABORT), "broker importer acknowledges cancelled generation after client object has gone");
    Check(Until([] { return gpu::ClientGpuInputChannel::RetirementBytes() == 0; }), "detached retirement worker reclaims terminal generations without a live client");
    for (auto const& generation : handles) for (auto handle : generation) Check(!HandleAlive(handle), "exact terminal cleanup closes each original client handle once");
    mapping.Close(); Check(Until([] { return gpu::ClientGpuInputChannel::RetirementChannels() == 0; }), "detached worker releases completed mapping and bounded registry record");
    Check(reconnect.Initialize(directory, 73, 5, d.Client.get(), d.ClientContext.get()), "same target reconnect succeeds after terminal predecessor cleanup"); reconnect.Reset();
    Check(Until([] { return gpu::ClientGpuInputChannel::RetirementChannels() == 0; }), "empty reconnect does not retain a mapping");
    std::vector<std::unique_ptr<gpu::ClientGpuInputChannel>> channels;
    for (int i = 0; i < 8; ++i) {
        auto channel = std::make_unique<gpu::ClientGpuInputChannel>();
        Check(channel->Initialize(directory, 80 + i, 1, d.Client.get(), d.ClientContext.get()), "bounded registry reserves channel before offering any resources"); channels.push_back(std::move(channel));
    }
    gpu::ClientGpuInputChannel ninth;
    Check(!ninth.Initialize(directory, 88, 1, d.Client.get(), d.ClientContext.get()) && ninth.LastError() == HRESULT_FROM_WIN32(ERROR_BUSY), "ninth channel fails closed at hard process registry capacity");
    channels.clear(); Check(Until([] { return gpu::ClientGpuInputChannel::RetirementChannels() == 0; }), "bounded registry reclaims all stopped empty channels");
}

void PendingClientReadRetirement(Devices& d, std::wstring const& directory) {
    gpu::ClientGpuInputChannel channel; gpu::GpuPrivateBaseCache cache; d.Refresh();
    Check(channel.Initialize(directory, 93, 1, d.Client.get(), d.ClientContext.get()) && channel.Offer(d.Owner, 6, 16, 8),
        "pending client read fixture creates one bounded offer");
    gpu::GpuControlMapping mapping; Check(mapping.Open(channel.ControlPath()), "pending read fixture opens persistent mapping");
    auto& record = mapping.Get()->Records[0]; auto offer = record.Offer;
    Check(gpu::TryClaim(record), "pending read fixture claims immutable originals"); Imports imports(d, offer);
    Check(gpu::CompleteImport(record, S_OK), "pending read fixture completes all imports");
    imports.Paint(d, 0, 1, 0, 1, 1); Check(Until([&] { return imports.Ready->GetCompletedValue() >= 1; }), "pending read source is fully ready before consumer queue wait");
    // A separate test fence deliberately holds the client queue. Release is
    // always queued before any assertion can throw, avoiding a stuck GPU test.
    winrt::com_ptr<ID3D11Fence> blocker, remoteBlocker;
    winrt::check_hresult(d.Client5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), blocker.put_void()));
    gpu::input_detail::Handle sharedBlocker; winrt::check_hresult(blocker->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, sharedBlocker.Put()));
    winrt::check_hresult(d.Broker5->OpenSharedFence(sharedBlocker.Get(), __uuidof(ID3D11Fence), remoteBlocker.put_void()));
    winrt::check_hresult(d.Client4->Wait(blocker.get(), 1));
    auto frame = FixtureFrame(); frame.Generation = offer.Generation; frame.PublishedTick = GetTickCount64();
    const auto copied = channel.TryCopyLatest(frame, cache);
    channel.RequestRetire(); const bool retired = gpu::CompleteRetirement(record); channel.Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(130));
    const bool originalPinned = HandleAlive(offer.TextureHandle[0]) && HandleAlive(offer.ReadyFenceHandle) && HandleAlive(offer.ConsumedFenceHandle);
    const bool readNotDone = imports.Consumed->GetCompletedValue() == 0;
    // Queue release before recording failures or performing staging Map.
    winrt::check_hresult(d.Broker4->Signal(remoteBlocker.get(), 1)); d.BrokerContext->Flush();
    Check(copied == gpu::GpuInputReadResult::Copied && retired && originalPinned && readNotDone,
        "terminal broker ack plus destructor keeps all originals while final client GPU copy remains pending");
    Check(Pixel(d, cache, 0xffff00ff), "later draw/read on same immediate context sees completed private copy after ordered wait");
    Check(Until([] { return gpu::ClientGpuInputChannel::RetirementBytes() == 0; }), "detached owner releases originals after final client copy fence completes");
    Check(!HandleAlive(offer.TextureHandle[0]) && !HandleAlive(offer.ReadyFenceHandle) && !HandleAlive(offer.ConsumedFenceHandle),
        "final client GPU completion closes the exact original texture and fence handles");
    mapping.Close(); Check(Until([] { return gpu::ClientGpuInputChannel::RetirementChannels() == 0; }), "final GPU read retirement releases mapping and worker module ownership");
}

int main() {
    MetadataTests(); MetadataAdmissionTests(); OwnershipTests();
    const auto directory = (std::filesystem::temp_directory_path() / (L"novabar-gpu-client-" + std::to_wstring(GetCurrentProcessId()))).wstring();
    try {
        std::filesystem::create_directories(directory); Devices devices;
        DeviceAdmissionTests(devices, directory); GpuCopyTests(devices, directory); StopAndPressureTests(devices, directory); PendingClientReadRetirement(devices, directory);
        std::filesystem::remove_all(directory);
    } catch (winrt::hresult_error const& error) { printf("FAIL: GPU input test HRESULT 0x%08x\n", unsigned(error.code().value)); ++failures; }
    catch (...) { printf("FAIL: unexpected GPU input test exception\n"); ++failures; }
    printf("GPU input client: %d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
