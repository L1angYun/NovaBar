#pragma once
#include "../NovaBar.Capture/GpuOwner.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <memory>
#include <mutex>
#include <vector>

namespace novabar::capture::gpu {
enum class GpuMetadataAdmission { Current, Obsolete, Invalid, Unavailable };
namespace input_detail {
// Issued IDs are contiguous: allocation failures do not consume an ID. These
// bounds are metadata only and never authorize a texture read or TTL renewal.
inline GpuMetadataAdmission ClassifyMetadata(GpuExpectedFrame const& frame,GpuOffer const& current,
    uint64_t firstIssued,uint64_t previousFirst,uint64_t previousLast,uint64_t now) noexcept {
    if(!ValidExpectedFrame(frame,now)||frame.Revision!=current.RequestRevision
        ||frame.Width!=current.Width||frame.Height!=current.Height)return GpuMetadataAdmission::Invalid;
    if(frame.Generation==current.Generation)return GpuMetadataAdmission::Current;
    const bool priorCurrent=firstIssued&&frame.Generation>=firstIssued&&frame.Generation<current.Generation;
    const bool priorChannel=previousFirst&&frame.Generation>=previousFirst&&frame.Generation<=previousLast;
    return priorCurrent||priorChannel?GpuMetadataAdmission::Obsolete:GpuMetadataAdmission::Invalid;
}
inline bool SameFrame(GpuExpectedFrame const& a, GpuExpectedFrame const& b) noexcept {
    return a.Revision == b.Revision && a.Generation == b.Generation && a.SourceSerial == b.SourceSerial
        && a.ReadyFenceValue == b.ReadyFenceValue && a.TextureSlot == b.TextureSlot
        && a.Width == b.Width && a.Height == b.Height && a.BitmapBounds.Left == b.BitmapBounds.Left
        && a.BitmapBounds.Top == b.BitmapBounds.Top && a.BitmapBounds.Right == b.BitmapBounds.Right
        && a.BitmapBounds.Bottom == b.BitmapBounds.Bottom;
}
// Observation is deliberately separate from acceptance: an unfinished ready
// fence must not renew the cached pixels' source timestamp.
class MetadataGate {
    GpuOffer m_offer{};
    GpuExpectedFrame m_observed{}, m_accepted{};
public:
    void Reset(GpuOffer const& offer) noexcept { m_offer = offer; Clear(); }
    bool Observe(GpuExpectedFrame const& f, uint64_t now) noexcept {
        if (!ValidExpectedFrame(f, now) || f.Revision != m_offer.RequestRevision || f.Generation != m_offer.Generation
            || f.Width != m_offer.Width || f.Height != m_offer.Height || f.SourceSerial < m_observed.SourceSerial) return false;
        if (f.SourceSerial == m_observed.SourceSerial) {
            if (!SameFrame(f, m_observed) || f.PublishedTick < m_observed.PublishedTick) return false;
        } else if (f.ReadyFenceValue <= m_observed.ReadyFenceValue) return false;
        m_observed = f; return true;
    }
    bool Commit(GpuExpectedFrame const& f) noexcept {
        if (!m_observed.SourceSerial || !SameFrame(f, m_observed) || f.PublishedTick != m_observed.PublishedTick) return false;
        m_accepted = f; return true;
    }
    bool Renew(GpuExpectedFrame const& f, uint64_t now) noexcept {
        return m_accepted.SourceSerial && SameFrame(f, m_accepted) && Observe(f, now) && Commit(f);
    }
    bool Ready(uint64_t now) const noexcept { return ValidExpectedFrame(m_accepted, now); }
    GpuExpectedFrame const& Accepted() const noexcept { return m_accepted; }
    void Clear() noexcept { m_observed = {}; m_accepted = {}; }
};
inline bool Disposable(GpuGenerationRecord const& record, bool readsComplete, bool deviceRemoved) noexcept {
    return Terminal(Phase(record)) && (readsComplete || deviceRemoved);
}
class Handle {
    HANDLE m_value = nullptr;
public:
    Handle() = default;
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    ~Handle() { Reset(); }
    HANDLE* Put() noexcept { Reset(); return &m_value; }
    HANDLE Get() const noexcept { return m_value; }
    void Reset() noexcept { if (m_value) CloseHandle(std::exchange(m_value, nullptr)); }
};
struct Generation {
    GpuOffer Offer{};
    MetadataGate Gate;
    std::array<winrt::com_ptr<ID3D11Texture2D>, 3> Inputs;
    std::array<Handle, 3> Textures;
    Handle ReadyHandle, ConsumedHandle;
    winrt::com_ptr<ID3D11Fence> Ready, Consumed, ReadComplete;
    uint64_t LastRead = 0, NextConsumed = 0, LastCopiedSerial = 0, Bytes = 0;
    bool UnsealedRead = false;
    bool ReadsComplete(bool removed) const noexcept {
        return removed || (!UnsealedRead && (!LastRead || (ReadComplete && ReadComplete->GetCompletedValue() != UINT64_MAX
            && ReadComplete->GetCompletedValue() >= LastRead)));
    }
};
struct ChannelState {
    GpuControlMapping Mapping;
    std::wstring Path;
    winrt::com_ptr<ID3D11Device> Device;
    winrt::com_ptr<ID3D11Device5> Device5;
    winrt::com_ptr<ID3D11DeviceContext> Context;
    winrt::com_ptr<ID3D11DeviceContext4> Context4;
    std::array<std::unique_ptr<Generation>, 2> Generations;
    GpuOwnerRecord Owner{};
    LUID Adapter{};
    uint64_t FirstGeneration = 0, NextGeneration = 0;
    DWORD Thread = 0;
    int Active = -1;
    bool Detached = false;
};
// One fixed process owner, one bounded worker. The worker only observes fences
// and releases resources; all immediate-context commands stay on the UI owner.
// A real module reference covers pending callbacks and is released by Windows
// after the final callback returns. No permanent DLL pin or named kernel object.
class RetirementRegistry {
    PTP_TIMER m_timer = nullptr;
    HMODULE m_module = nullptr;
    static void CALLBACK Callback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER) noexcept {
        auto& registry = *static_cast<RetirementRegistry*>(context);
        std::lock_guard lock(registry.Mutex);
        for (auto const& state : registry.Channels) registry.Poll(*state);
        std::erase_if(registry.Channels, [](auto const& state) {
            return state->Detached && !state->Generations[0] && !state->Generations[1];
        });
        if (registry.Channels.empty()) {
            auto timer = std::exchange(registry.m_timer, nullptr);
            auto module = std::exchange(registry.m_module, nullptr);
            // One-shot scheduling means no other queued callback can outlive
            // this module reference, even while another client initializes.
            CloseThreadpoolTimer(timer);
            FreeLibraryWhenCallbackReturns(instance, module);
        } else registry.Arm();
    }
    void Arm() noexcept {
        LARGE_INTEGER due{}; due.QuadPart = -1000000; // one shot, 100 ms
        FILETIME time{due.LowPart, static_cast<DWORD>(due.HighPart)};
        SetThreadpoolTimer(m_timer, &time, 0, 0);
    }
public:
    static constexpr size_t MaxChannels = 8;
    static constexpr uint64_t MaxBytes = 512ull * 1024 * 1024;
    std::recursive_mutex Mutex;
    std::vector<std::shared_ptr<ChannelState>> Channels;
    uint64_t Bytes = 0;
    HRESULT WorkerError = S_OK;
    static RetirementRegistry& Get() {
        // Fixed process-lifetime metadata; resources themselves are reclaimed
        // on terminal broker acknowledgement and final client GPU completion.
        static auto* value = new RetirementRegistry; return *value;
    }
    bool EnsureWorker() noexcept {
        if (m_timer) return true;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Callback), &module)) { WorkerError = HRESULT_FROM_WIN32(GetLastError()); return false; }
        auto timer = CreateThreadpoolTimer(Callback, this, nullptr);
        if (!timer) { WorkerError = HRESULT_FROM_WIN32(GetLastError()); FreeLibrary(module); return false; }
        m_module = module; m_timer = timer; WorkerError = S_OK; Arm(); return true;
    }
    void Poll(ChannelState& state) noexcept {
        auto* control = state.Mapping.Get(); if (!control) return;
        const bool removed = FAILED(state.Device->GetDeviceRemovedReason());
        for (size_t i = 0; i < state.Generations.size(); ++i) {
            auto& generation = state.Generations[i]; if (!generation) continue;
            auto& record = control->Records[i];
            if (Disposable(record, generation->ReadsComplete(removed), removed)) {
                if (state.Active == static_cast<int>(i)) { state.Active = -1; InterlockedExchange64(&control->Header.ActiveRecord, -1); }
                Bytes -= generation->Bytes;
                generation.reset(); ResetTerminal(record, true);
            }
        }
    }
    bool Reserve(uint64_t bytes) noexcept {
        if (bytes > MaxBytes - Bytes) return false;
        Bytes += bytes; return true;
    }
};
inline LUID DeviceAdapter(ID3D11Device* device) {
    winrt::com_ptr<ID3D11Device> retained; retained.copy_from(device);
    auto dxgi = retained.as<IDXGIDevice>(); winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgi->GetAdapter(adapter.put())); DXGI_ADAPTER_DESC desc{};
    winrt::check_hresult(adapter->GetDesc(&desc)); return desc.AdapterLuid;
}
}

enum class GpuInputReadResult { Unavailable, Pending, Unchanged, Copied };
class ClientGpuInputChannel;
class GpuPrivateBaseCache {
    friend class ClientGpuInputChannel;
    input_detail::MetadataGate m_gate;
    GpuOffer m_offer{};
    winrt::com_ptr<ID3D11Texture2D> m_texture;
    winrt::com_ptr<ID3D11ShaderResourceView> m_view;
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
public:
    GpuPrivateBaseCache() = default;
    GpuPrivateBaseCache(GpuPrivateBaseCache const&) = delete;
    GpuPrivateBaseCache& operator=(GpuPrivateBaseCache const&) = delete;
    void Clear() noexcept { m_gate.Clear(); m_offer = {}; m_view = nullptr; m_texture = nullptr; m_context = nullptr; m_device = nullptr; }
    bool Ready(uint64_t now = GetTickCount64()) const noexcept {
        return m_texture && m_view && m_device && SUCCEEDED(m_device->GetDeviceRemovedReason()) && m_gate.Ready(now);
    }
    ID3D11ShaderResourceView* ShaderResource() const noexcept { return m_view.get(); }
    ID3D11Texture2D* Texture() const noexcept { return m_texture.get(); }
    GpuExpectedFrame const& Metadata() const noexcept { return m_gate.Accepted(); }
};

// All methods, the cache, optics, and presenter use the caller's one immediate
// context on its serialized owner thread. Copied means the private copy has
// been queued safely; subsequent draws on that SAME context see the copy by
// command ordering. A different context/device must never sample this cache.
class ClientGpuInputChannel {
    std::shared_ptr<input_detail::ChannelState> m_state;
    // Preserve the immediately preceding transport namespace across an internal
    // device/adapter Reset with unchanged request revision. No handles are kept.
    uint64_t m_previousFirst = 0, m_previousLast = 0;
    HRESULT m_error = E_PENDING;
    bool Fail(HRESULT error) noexcept { m_error = error; return false; }
    bool Current(input_detail::Generation*& generation, GpuGenerationRecord*& record) noexcept {
        generation = nullptr; record = nullptr;
        if (!m_state || m_state->Detached || m_state->Active < 0) return false;
        auto& s = *m_state; auto* control = s.Mapping.Get(); const auto index = s.Active;
        if (!control || !ValidControl(*control) || !s.Generations[index]) return false;
        generation = s.Generations[index].get(); record = &control->Records[index];
        return ValidLifecycle(record->Life) && Phase(*record) == GpuPhase::Imported && SUCCEEDED(record->Life.Result) && !Cancelled(*record)
            && !memcmp(&record->Offer, &generation->Offer, sizeof(GpuOffer))
            && SameIdentity(control->Header.Client, generation->Offer.Client)
            && control->Header.TargetId == generation->Offer.TargetId && control->Header.ClientEpoch == generation->Offer.ClientEpoch;
    }
    void CancelActive() noexcept {
        if (!m_state || m_state->Active < 0) return;
        auto* control = m_state->Mapping.Get();
        RequestCancel(control->Records[m_state->Active]);
        InterlockedExchange64(&control->Header.ActiveRecord, -1); m_state->Active = -1;
    }
public:
    ClientGpuInputChannel() = default;
    ClientGpuInputChannel(ClientGpuInputChannel const&) = delete;
    ClientGpuInputChannel& operator=(ClientGpuInputChannel const&) = delete;
    ~ClientGpuInputChannel() { Reset(); }
    bool Initialize(std::wstring const& directory, uint64_t targetId, uint64_t clientEpoch,
        ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
        try {
            if (!device || !context || !targetId || !clientEpoch || directory.empty()
                || (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return Fail(E_INVALIDARG);
            winrt::com_ptr<ID3D11Device> owner; context->GetDevice(owner.put());
            if (owner.get() != device) return Fail(E_INVALIDARG);
            winrt::check_hresult(device->GetDeviceRemovedReason());
            Reset(); auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex);
            if (registry.Channels.size() >= registry.MaxChannels) return Fail(HRESULT_FROM_WIN32(ERROR_BUSY));
            if (!registry.EnsureWorker()) return Fail(registry.WorkerError);
            auto state = std::make_shared<input_detail::ChannelState>();
            state->Thread = GetCurrentThreadId();
            state->Device.copy_from(device); state->Context.copy_from(context);
            state->Device5 = state->Device.as<ID3D11Device5>(); state->Context4 = state->Context.as<ID3D11DeviceContext4>();
            state->Adapter = input_detail::DeviceAdapter(device); state->NextGeneration = RandomNonce();
            if (!state->NextGeneration || state->NextGeneration == UINT64_MAX) return Fail(E_UNEXPECTED);
            const auto client = CurrentIdentity(); state->Path = detail::Path(directory, client.Pid, targetId, L".gpu");
            // CREATE_NEW preserves a detached predecessor's original mapping.
            if (!state->Mapping.Create(state->Path, client, targetId, clientEpoch)) return Fail(HRESULT_FROM_WIN32(GetLastError()));
            registry.Channels.push_back(state); m_state = std::move(state); m_error = S_OK; return true;
        } catch (...) { return Fail(winrt::to_hresult()); }
    }
    bool Offer(GpuOwnerRecord const& freshOwner, uint64_t revision, uint32_t width, uint32_t height) noexcept {
        try {
            auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex);
            if (!m_state || m_state->Detached) return Fail(E_NOINTERFACE);
            if (m_state->Thread != GetCurrentThreadId()) return Fail(RPC_E_WRONG_THREAD);
            auto& s = *m_state; registry.Poll(s);
            if (!ValidOwner(freshOwner, GetTickCount64()) || !revision || !width || !height || width > 16384 || height > 16384
                || uint64_t(width) * height > MaxPixels || freshOwner.AdapterLow != s.Adapter.LowPart
                || freshOwner.AdapterHigh != s.Adapter.HighPart) { CancelActive(); return Fail(E_INVALIDARG); }
            winrt::check_hresult(s.Device->GetDeviceRemovedReason());
            if (s.Active >= 0) {
                auto const& active = *s.Generations[s.Active]; auto const& offer = active.Offer;
                auto const& record = s.Mapping.Get()->Records[s.Active];
                if (SameOwner(freshOwner, s.Owner) && offer.RequestRevision == revision && offer.Width == width
                    && offer.Height == height && !Cancelled(record) && !Terminal(Phase(record))) { m_error = S_OK; return true; }
            }
            CancelActive(); registry.Poll(s);
            const int index = FindEmptyRecord(*s.Mapping.Get());
            if (index < 0) return Fail(E_PENDING);
            const uint64_t bytes = uint64_t(width) * height * 4 * TextureSlotCount;
            if (!registry.Reserve(bytes)) return Fail(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY));
            // Reserve before creating any texture; the local rollback returns
            // that reservation on every pre-publication failure.
            struct Reservation { input_detail::RetirementRegistry& Registry; uint64_t Bytes; bool Committed = false;
                ~Reservation() { if (!Committed) Registry.Bytes -= Bytes; } } reservation{registry, bytes};
            auto generation = std::make_unique<input_detail::Generation>(); generation->Bytes = bytes;
            auto& offer = generation->Offer; const auto& header = s.Mapping.Get()->Header;
            if (s.NextGeneration >= UINT64_MAX - 1) return Fail(E_UNEXPECTED);
            offer.TargetId = header.TargetId; offer.ClientEpoch = header.ClientEpoch; offer.Generation = s.NextGeneration + 1;
            offer.RequestRevision = revision; offer.Client = header.Client;
            offer.ExpectedBroker = {freshOwner.Pid, 0, freshOwner.CreationFileTime}; offer.BrokerEpoch = freshOwner.Nonce;
            offer.AdapterLow = s.Adapter.LowPart; offer.AdapterHigh = s.Adapter.HighPart; offer.Width = width; offer.Height = height;
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = SharedTextureMiscFlags;
            for (uint32_t i = 0; i < TextureSlotCount; ++i) {
                winrt::check_hresult(s.Device->CreateTexture2D(&desc, nullptr, generation->Inputs[i].put()));
                auto resource = generation->Inputs[i].as<IDXGIResource1>();
                winrt::check_hresult(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr, generation->Textures[i].Put()));
                offer.TextureHandle[i] = reinterpret_cast<uintptr_t>(generation->Textures[i].Get());
            }
            winrt::check_hresult(s.Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), generation->Ready.put_void()));
            winrt::check_hresult(s.Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), generation->Consumed.put_void()));
            winrt::check_hresult(s.Device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), generation->ReadComplete.put_void()));
            winrt::check_hresult(generation->Ready->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, generation->ReadyHandle.Put()));
            winrt::check_hresult(generation->Consumed->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, generation->ConsumedHandle.Put()));
            offer.ReadyFenceHandle = reinterpret_cast<uintptr_t>(generation->ReadyHandle.Get());
            offer.ConsumedFenceHandle = reinterpret_cast<uintptr_t>(generation->ConsumedHandle.Get());
            generation->Gate.Reset(offer);
            // Store the local owner before publication: the broker can win its
            // CAS immediately and every published numeric handle is pinned.
            s.Generations[index] = std::move(generation);
            if (!PublishOffer(s.Mapping.Get()->Records[index], s.Generations[index]->Offer)) {
                s.Generations[index].reset(); return Fail(E_UNEXPECTED);
            }
            s.NextGeneration = offer.Generation;
            if(!s.FirstGeneration)s.FirstGeneration=offer.Generation;
            reservation.Committed = true; s.Active = index; s.Owner = freshOwner;
            m_error = S_OK; return true;
        } catch (...) { const auto error = winrt::to_hresult(); RequestRetire(); return Fail(error); }
    }
    // Validate against the current OFFER, including Offered/Claiming, before
    // retaining pending metadata. This does not observe/accept a source frame.
    GpuMetadataAdmission AdmitMetadata(GpuExpectedFrame const& expected,uint64_t now=GetTickCount64()) noexcept {
        auto& registry=input_detail::RetirementRegistry::Get();std::lock_guard lock(registry.Mutex);
        const auto reject=[&](HRESULT error,GpuMetadataAdmission result){m_error=error;return result;};
        if(!m_state||m_state->Detached||m_state->Active<0)return reject(E_PENDING,GpuMetadataAdmission::Unavailable);
        auto& s=*m_state;
        if(s.Thread!=GetCurrentThreadId())return reject(RPC_E_WRONG_THREAD,GpuMetadataAdmission::Unavailable);
        const auto removed=s.Device->GetDeviceRemovedReason();
        if(FAILED(removed))return reject(removed,GpuMetadataAdmission::Unavailable);
        auto* control=s.Mapping.Get();const auto index=s.Active;
        if(!control||!ValidControl(*control)||!s.Generations[index])
            return reject(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),GpuMetadataAdmission::Unavailable);
        auto const& record=control->Records[index];auto const& offer=s.Generations[index]->Offer;const auto phase=Phase(record);
        if(!ValidLifecycle(record.Life)||Cancelled(record)||(phase!=GpuPhase::Offered&&phase!=GpuPhase::Claiming&&phase!=GpuPhase::Imported)
            ||memcmp(&record.Offer,&offer,sizeof(offer))||!SameIdentity(control->Header.Client,offer.Client)
            ||control->Header.TargetId!=offer.TargetId||control->Header.ClientEpoch!=offer.ClientEpoch)
            return reject(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),GpuMetadataAdmission::Unavailable);
        if(phase==GpuPhase::Imported&&FAILED(record.Life.Result))return reject(record.Life.Result,GpuMetadataAdmission::Unavailable);
        const auto result=input_detail::ClassifyMetadata(expected,offer,s.FirstGeneration,m_previousFirst,m_previousLast,now);
        m_error=result==GpuMetadataAdmission::Current?S_OK:result==GpuMetadataAdmission::Obsolete?E_PENDING:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return result;
    }
    GpuInputReadResult TryCopyLatest(GpuExpectedFrame const& expected, GpuPrivateBaseCache& cache,
        uint64_t now = GetTickCount64()) noexcept {
        try {
            auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex);
            if (m_state && m_state->Thread != GetCurrentThreadId()) { m_error = RPC_E_WRONG_THREAD; return GpuInputReadResult::Unavailable; }
            input_detail::Generation* generation = nullptr; GpuGenerationRecord* record = nullptr;
            if (!Current(generation, record)) {
                cache.Clear(); m_error = E_PENDING;
                if (m_state && m_state->Active >= 0) {
                    auto const& current = m_state->Mapping.Get()->Records[m_state->Active]; const auto phase = Phase(current);
                    if (!Cancelled(current) && (phase == GpuPhase::Offered || phase == GpuPhase::Claiming)) return GpuInputReadResult::Pending;
                    m_error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
                return GpuInputReadResult::Unavailable;
            }
            auto& s = *m_state; winrt::check_hresult(s.Device->GetDeviceRemovedReason());
            if (memcmp(&cache.m_offer, &generation->Offer, sizeof(GpuOffer)) || cache.m_device.get() != s.Device.get()
                || cache.m_context.get() != s.Context.get()) {
                cache.Clear(); cache.m_offer = generation->Offer; cache.m_gate.Reset(generation->Offer);
                cache.m_device = s.Device; cache.m_context = s.Context;
            }
            if (!generation->Gate.Observe(expected, now) || !cache.m_gate.Observe(expected, now)) {
                cache.Clear(); m_error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return GpuInputReadResult::Unavailable;
            }
            if (cache.m_texture && input_detail::SameFrame(cache.Metadata(), expected)) {
                cache.m_gate.Renew(expected, now); m_error = S_OK; return GpuInputReadResult::Unchanged;
            }
            // A cleared/replaced cache cannot be rebuilt from a slot already
            // released to the broker, even when its metadata is still fresh.
            if (expected.SourceSerial <= generation->LastCopiedSerial) {
                cache.Clear(); m_error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return GpuInputReadResult::Unavailable;
            }
            const uint64_t ready = generation->Ready->GetCompletedValue();
            if (ready == UINT64_MAX) winrt::throw_hresult(DXGI_ERROR_DEVICE_REMOVED);
            if (ready < expected.ReadyFenceValue) { m_error = E_PENDING; return GpuInputReadResult::Pending; }
            if (!cache.m_texture) {
                D3D11_TEXTURE2D_DESC desc{}; desc.Width = expected.Width; desc.Height = expected.Height;
                desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                winrt::check_hresult(s.Device->CreateTexture2D(&desc, nullptr, cache.m_texture.put()));
                winrt::check_hresult(s.Device->CreateShaderResourceView(cache.m_texture.get(), nullptr, cache.m_view.put()));
            }
            if (generation->LastRead >= UINT64_MAX - 1 || generation->NextConsumed >= UINT64_MAX - 1) winrt::throw_hresult(E_UNEXPECTED);
            // Ready has completed in the producer queue. The only shared-input
            // read is this copy; all later optics reads use the private cache.
            generation->UnsealedRead = true;
            // D3D11 predication applies to CopyResource as well as draws. Never
            // acknowledge consumption of a copy skipped by inherited state.
            winrt::com_ptr<ID3D11Predicate> predicate; BOOL predicateValue = FALSE;
            s.Context->GetPredication(predicate.put(), &predicateValue);
            s.Context->SetPredication(nullptr, FALSE);
            s.Context->CopyResource(cache.m_texture.get(), generation->Inputs[expected.TextureSlot].get());
            s.Context->SetPredication(predicate.get(), predicateValue);
            generation->LastCopiedSerial = expected.SourceSerial;
            const uint64_t localValue = generation->LastRead + 1;
            const HRESULT localResult = s.Context4->Signal(generation->ReadComplete.get(), localValue);
            if (SUCCEEDED(localResult)) { generation->LastRead = localValue; generation->UnsealedRead = false; }
            const uint64_t consumedValue = generation->NextConsumed + 1;
            const HRESULT consumedResult = SUCCEEDED(localResult) ? s.Context4->Signal(generation->Consumed.get(), consumedValue) : localResult;
            s.Context->Flush();
            winrt::check_hresult(localResult); winrt::check_hresult(consumedResult);
            generation->NextConsumed = consumedValue;
            winrt::check_hresult(s.Device->GetDeviceRemovedReason());
            if (!PublishConsumed(record->Consumed[expected.TextureSlot], {expected.Generation, expected.SourceSerial, consumedValue}))
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            if (!generation->Gate.Commit(expected) || !cache.m_gate.Commit(expected)
                || !ActivateRecord(*s.Mapping.Get(), uint32_t(s.Active))) winrt::throw_hresult(E_ABORT);
            m_error = S_OK; return GpuInputReadResult::Copied;
        } catch (...) { const auto error = winrt::to_hresult(); cache.Clear(); RequestRetire(); m_error = error; return GpuInputReadResult::Unavailable; }
    }
    bool RenewAcceptedMetadata(GpuExpectedFrame const& expected, GpuPrivateBaseCache& cache,
        uint64_t now = GetTickCount64()) noexcept {
        auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex);
        input_detail::Generation* generation = nullptr; GpuGenerationRecord* record = nullptr;
        if (!Current(generation, record) || !cache.m_texture || cache.m_device.get() != m_state->Device.get()
            || cache.m_context.get() != m_state->Context.get() || memcmp(&cache.m_offer, &generation->Offer, sizeof(GpuOffer))
            || !generation->Gate.Renew(expected, now) || !cache.m_gate.Renew(expected, now)) return Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        m_error = S_OK; return true;
    }
    void RequestRetire() noexcept {
        auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex); CancelActive(); m_error = E_PENDING;
    }
    void PollRetirement() noexcept {
        auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex);
        if (m_state) registry.Poll(*m_state);
    }
    void Reset() noexcept {
        if (!m_state) return;
        auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex);
        if(m_state->FirstGeneration){m_previousFirst=m_state->FirstGeneration;m_previousLast=m_state->NextGeneration;}
        CancelActive(); m_state->Detached = true; registry.Poll(*m_state); m_state.reset(); m_error = E_PENDING;
        // Registry ownership and its worker were reserved by Initialize, so
        // Stop/destruction cannot fail an allocation and drop a claimed handle.
    }
    HRESULT LastError() const noexcept { return m_error; }
    std::wstring ControlPath() const { return m_state ? m_state->Path : std::wstring{}; }
    static uint64_t RetirementBytes() noexcept {
        auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex); return registry.Bytes;
    }
    static size_t RetirementChannels() noexcept {
        auto& registry = input_detail::RetirementRegistry::Get(); std::lock_guard lock(registry.Mutex); return registry.Channels.size();
    }
};
}
