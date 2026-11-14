#pragma once
#include "GpuOwner.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <memory>
#include <optional>
#include <wrl/client.h>

namespace novabar::capture::gpu
{
enum class BrokerImportResult
{
    Ready,
    Pending,
    Rejected,
    Unavailable
};
class BrokerGpuInputTarget;
namespace broker_detail
{
struct LeaseAnchor
{
    BrokerGpuInputTarget *Owner = nullptr;
};
inline bool SameObject(IUnknown *first, IUnknown *second) noexcept
{
    if (!first || !second)
        return false;
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    return SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&a))) && SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&b))) &&
           a.Get() == b.Get();
}
inline bool SameRect(PixelRect const &a, PixelRect const &b) noexcept
{
    return a.Left == b.Left && a.Top == b.Top && a.Right == b.Right && a.Bottom == b.Bottom;
}
} // namespace broker_detail

// Single serialized MTA. The caller verifies OwnsWindows(currentRequest)
// before TryImport and uses the returned RTV only until Publish/CancelDraw.
// A lease is a bounded write reservation, not proof that GPU work completed.
class WritableGpuSlot
{
    friend class BrokerGpuInputTarget;
    std::weak_ptr<broker_detail::LeaseAnchor> m_anchor;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_target;
    uint64_t m_token = 0;
    uint32_t m_entry = 0, m_slot = 0;
    void Release() noexcept;

  public:
    uint64_t Generation = 0, Revision = 0;
    uint32_t Index = 0, Width = 0, Height = 0;
    WritableGpuSlot() = default;
    WritableGpuSlot(WritableGpuSlot const &) = delete;
    WritableGpuSlot &operator=(WritableGpuSlot const &) = delete;
    WritableGpuSlot(WritableGpuSlot &&other) noexcept
    {
        *this = std::move(other);
    }
    WritableGpuSlot &operator=(WritableGpuSlot &&other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_anchor = std::move(other.m_anchor);
            m_target = std::move(other.m_target);
            m_token = std::exchange(other.m_token, 0);
            m_entry = other.m_entry;
            m_slot = other.m_slot;
            Generation = other.Generation;
            Revision = other.Revision;
            Index = other.Index;
            Width = other.Width;
            Height = other.Height;
        }
        return *this;
    }
    ~WritableGpuSlot()
    {
        Release();
    }
    ID3D11RenderTargetView *RenderTarget() const noexcept
    {
        return m_token ? m_target.Get() : nullptr;
    }
};

class BrokerGpuInputTarget
{
    friend class WritableGpuSlot;
    using DevicePtr = Microsoft::WRL::ComPtr<ID3D11Device5>;
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Slot
    {
        ComPtr<ID3D11Texture2D> Texture;
        ComPtr<ID3D11RenderTargetView> Target;
        uint64_t LastSerial = 0, SealValue = 0, LeaseToken = 0;
    };
    struct Generation
    {
        std::shared_ptr<GpuControlMapping> Mapping;
        GpuOffer Offer{};
        uint32_t RecordIndex = 0;
        std::array<Slot, TextureSlotCount> Slots{};
        ComPtr<ID3D11Fence> Ready, Consumed;
        uint64_t LastReady = 0, LastPublishedSerial = 0;
        bool Retiring = false, Unsealed = false, CannotSeal = false;
        HRESULT Failure = S_OK;
        GpuGenerationRecord &Record() const noexcept
        {
            return Mapping->Get()->Records[RecordIndex];
        }
    };
    DevicePtr m_device;
    ComPtr<ID3D11DeviceContext4> m_context;
    GpuOwnerRecord m_owner{};
    LUID m_adapter{};
    std::wstring m_root;
    Request m_request{};
    std::array<std::unique_ptr<Generation>, GenerationRecordCount> m_generations{};
    std::shared_ptr<broker_detail::LeaseAnchor> m_anchor;
    uint64_t m_nextLease = 0;
    DWORD m_thread = 0;
    uint32_t m_clientPid = 0;
    uint64_t m_targetId = 0;
    int m_active = -1;
    HRESULT m_error = S_OK;
    struct RetirementRegistry;
    static RetirementRegistry &Registry();
    int m_retirementIndex = -1;
    bool m_detached = false;
    std::unique_ptr<BrokerGpuInputTarget> m_retirementSpare;
    bool ReserveRetirement();
    void ReleaseRetirement() noexcept;
    void DetachRetirement() noexcept;

    bool OnThread() noexcept
    {
        if (m_thread && GetCurrentThreadId() == m_thread)
            return true;
        m_error = RPC_E_WRONG_THREAD;
        return false;
    }
    bool HasOwner(std::wstring const &root)
    {
        GpuOwnerRecord current{};
        if (TryReadFreshGpuOwner(root, GetTickCount64(), current) && SameOwner(current, m_owner) &&
            current.AdapterLow == m_adapter.LowPart && current.AdapterHigh == m_adapter.HighPart)
            return true;
        m_error = HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
        return false;
    }
    bool Healthy() noexcept
    {
        if (!m_device)
        {
            m_error = E_UNEXPECTED;
            return false;
        }
        const HRESULT removed = m_device->GetDeviceRemovedReason();
        if (FAILED(removed))
        {
            m_error = removed;
            return false;
        }
        return true;
    }
    bool FenceValue(ID3D11Fence *fence, uint64_t &completed) noexcept
    {
        completed = fence->GetCompletedValue();
        if (completed != UINT64_MAX)
            return true;
        const auto removed = m_device->GetDeviceRemovedReason();
        m_error = FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED;
        return false;
    }
    void BeginRetire(Generation &generation, HRESULT failure = S_OK) noexcept
    {
        generation.Retiring = true;
        if (FAILED(failure))
            generation.Failure = failure;
        RequestCancel(generation.Record());
    }
    bool Seal(Generation &generation, Slot &slot) noexcept
    {
        slot.LeaseToken = 0;
        generation.Unsealed = true;
        if (generation.LastReady >= UINT64_MAX - 1 || !Healthy())
        {
            if (SUCCEEDED(m_error))
                m_error = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            generation.CannotSeal = generation.LastReady >= UINT64_MAX - 1;
            BeginRetire(generation, m_error);
            return false;
        }
        const auto value = ++generation.LastReady;
        generation.Unsealed = true;
        const HRESULT result = m_context->Signal(generation.Ready.Get(), value);
        if (FAILED(result))
        {
            m_error = result;
            BeginRetire(generation, result);
            return false;
        }
        generation.Unsealed = false;
        slot.SealValue = value;
        m_context->Flush();
        return true;
    }
    bool Matches(Generation const &generation, WritableGpuSlot const &lease) const noexcept
    {
        auto anchor = lease.m_anchor.lock();
        return anchor && anchor == m_anchor && lease.m_token && lease.Index < TextureSlotCount &&
               lease.Index == lease.m_slot && lease.Generation == generation.Offer.Generation &&
               lease.Revision == generation.Offer.RequestRevision && lease.Width == generation.Offer.Width &&
               lease.Height == generation.Offer.Height && generation.Slots[lease.Index].LeaseToken == lease.m_token;
    }
    void CancelLease(uint32_t entry, uint32_t index, uint64_t token) noexcept
    {
        if (!OnThread() || entry >= m_generations.size() || !m_generations[entry] || index >= TextureSlotCount)
            return;
        auto &generation = *m_generations[entry];
        auto &slot = generation.Slots[index];
        if (token && token == slot.LeaseToken)
            Seal(generation, slot);
    }
    HRESULT OpenImports(Generation &generation, HANDLE process) noexcept
    {
        const auto compareHandles = reinterpret_cast<decltype(&CompareObjectHandles)>(
            GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles"));
        if (!compareHandles)
            return E_NOTIMPL;
        auto const &offer = generation.Offer;
        const uint64_t values[]{offer.TextureHandle[0], offer.TextureHandle[1], offer.TextureHandle[2],
                                offer.ReadyFenceHandle, offer.ConsumedFenceHandle};
        std::array<LocalHandle, 5> pulled;
        for (unsigned i = 0; i < std::size(values); ++i)
        {
            if (Cancelled(generation.Record()))
                return E_ABORT;
            if (!pulled[i].Pull(process, values[i]))
                return HRESULT_FROM_WIN32(GetLastError());
            // Separate NT handle numbers may name the same kernel object.
            // OpenSharedResource1 creates distinct COM wrappers, so COM
            // identity alone cannot establish independent shared inputs.
            for (unsigned previous = 0; previous < i; ++previous)
                if (compareHandles(pulled[i].Get(), pulled[previous].Get()))
                    return E_INVALIDARG;
        }
        for (unsigned i = 0; i < std::size(values); ++i)
        {
            if (Cancelled(generation.Record()))
                return E_ABORT;
            auto &local = pulled[i];
            HRESULT result = S_OK;
            if (i < TextureSlotCount)
            {
                auto &slot = generation.Slots[i];
                result = m_device->OpenSharedResource1(local.Get(), IID_PPV_ARGS(&slot.Texture));
                local.Reset();
                if (FAILED(result))
                    return result;
                D3D11_TEXTURE2D_DESC desc{};
                slot.Texture->GetDesc(&desc);
                if (desc.Width != offer.Width || desc.Height != offer.Height ||
                    desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.MipLevels != 1 || desc.ArraySize != 1 ||
                    desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || desc.Usage != D3D11_USAGE_DEFAULT ||
                    desc.CPUAccessFlags || desc.MiscFlags != SharedTextureMiscFlags ||
                    desc.BindFlags != (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
                    return E_INVALIDARG;
                ComPtr<ID3D11Device> device;
                slot.Texture->GetDevice(&device);
                if (!broker_detail::SameObject(device.Get(), m_device.Get()))
                    return E_INVALIDARG;
                for (unsigned prior = 0; prior < i; ++prior)
                    if (broker_detail::SameObject(slot.Texture.Get(), generation.Slots[prior].Texture.Get()))
                        return E_INVALIDARG;
                D3D11_RENDER_TARGET_VIEW_DESC view{};
                view.Format = desc.Format;
                view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                result = m_device->CreateRenderTargetView(slot.Texture.Get(), &view, &slot.Target);
            }
            else
            {
                auto &fence = i == TextureSlotCount ? generation.Ready : generation.Consumed;
                result = m_device->OpenSharedFence(local.Get(), IID_PPV_ARGS(&fence));
                local.Reset();
                if (SUCCEEDED(result))
                {
                    uint64_t completed = 0;
                    if (!FenceValue(fence.Get(), completed))
                        return m_error;
                    if (completed)
                        return E_INVALIDARG; // every generation owns fresh zero-based fences
                }
            }
            if (FAILED(result))
                return result;
        }
        if (broker_detail::SameObject(generation.Ready.Get(), generation.Consumed.Get()))
            return E_INVALIDARG;
        return Cancelled(generation.Record()) ? E_ABORT : S_OK;
    }
    bool Current(Generation const &generation) const noexcept
    {
        return !generation.Retiring && Phase(generation.Record()) == GpuPhase::Imported &&
               !Cancelled(generation.Record()) && generation.Offer.RequestRevision == m_request.Revision &&
               generation.Offer.Width == m_request.OutputWidth && generation.Offer.Height == m_request.OutputHeight;
    }
    void DropRetired(size_t i, bool deviceGone) noexcept
    {
        auto &generation = *m_generations[i];
        // COM imports are dropped before the client receives terminal ownership.
        for (auto &slot : generation.Slots)
        {
            slot.Target.Reset();
            slot.Texture.Reset();
        }
        generation.Ready.Reset();
        generation.Consumed.Reset();
        if (deviceGone || FAILED(generation.Failure))
            generation.Record().Life.Result = deviceGone ? m_error : generation.Failure;
        CompleteRetirement(generation.Record());
        if (m_active == static_cast<int>(i))
            m_active = -1;
        m_generations[i].reset();
    }

  public:
    BrokerGpuInputTarget() = default;
    BrokerGpuInputTarget(BrokerGpuInputTarget const &) = delete;
    BrokerGpuInputTarget &operator=(BrokerGpuInputTarget const &) = delete;
    ~BrokerGpuInputTarget()
    {
        if (!m_device)
            return;
        // Destruction never waits on the shared MTA. Capacity for this
        // fallback was reserved before any offer could become Claiming.
        RequestRetire();
        PollRetirement();
        if (!Idle() && !m_detached)
        {
            DetachRetirement();
            return;
        }
        if (m_anchor)
            m_anchor->Owner = nullptr;
        ReleaseRetirement();
    }
    bool Initialize(ID3D11Device *device, ID3D11DeviceContext *context, GpuOwnerRecord const &owner)
    {
        if (m_device || !device || !context || !ValidOwner(owner, GetTickCount64()) ||
            !SameIdentity({owner.Pid, 0, owner.CreationFileTime}, CurrentIdentity()))
        {
            m_error = E_INVALIDARG;
            return false;
        }
        ComPtr<ID3D11Device> contextDevice;
        context->GetDevice(&contextDevice);
        if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            !broker_detail::SameObject(device, contextDevice.Get()))
        {
            m_error = E_INVALIDARG;
            return false;
        }
        DevicePtr device5;
        ComPtr<ID3D11DeviceContext4> context4;
        HRESULT result = device->QueryInterface(IID_PPV_ARGS(&device5));
        if (SUCCEEDED(result))
            result = context->QueryInterface(IID_PPV_ARGS(&context4));
        if (FAILED(result))
        {
            m_error = result;
            return false;
        }
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        result = device->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(result))
            result = dxgi->GetAdapter(&adapter);
        if (SUCCEEDED(result))
            result = adapter->GetDesc(&desc);
        if (FAILED(result))
        {
            m_error = result;
            return false;
        }
        if (desc.AdapterLuid.LowPart != owner.AdapterLow || desc.AdapterLuid.HighPart != owner.AdapterHigh)
        {
            m_error = E_INVALIDARG;
            return false;
        }
        if (!ReserveRetirement())
        {
            m_error = HRESULT_FROM_WIN32(ERROR_BUSY);
            return false;
        }
        m_device = std::move(device5);
        m_context = std::move(context4);
        m_owner = owner;
        m_adapter = desc.AdapterLuid;
        m_thread = GetCurrentThreadId();
        m_anchor = std::make_shared<broker_detail::LeaseAnchor>();
        m_anchor->Owner = this;
        m_error = S_OK;
        return true;
    }
    BrokerImportResult TryImport(std::wstring const &root, Request const &request, HANDLE verifiedClientProcess)
    {
        if (!OnThread() || !Healthy())
            return BrokerImportResult::Unavailable;
        m_request = request;
        if (!detail::ValidRequest(request) || !(request.Flags & GpuBase))
        {
            m_error = E_INVALIDARG;
            RequestRetire();
            return BrokerImportResult::Unavailable;
        }
        if (!HasOwner(root))
        {
            RequestRetire();
            return BrokerImportResult::Unavailable;
        }
        const auto clientPid = verifiedClientProcess && verifiedClientProcess != INVALID_HANDLE_VALUE
                                   ? GetProcessId(verifiedClientProcess)
                                   : 0;
        const auto client = Identity(verifiedClientProcess, clientPid);
        if (clientPid != request.Pid || !ValidIdentity(client) ||
            WaitForSingleObject(verifiedClientProcess, 0) != WAIT_TIMEOUT)
        {
            m_error = E_INVALIDARG;
            RequestRetire();
            return BrokerImportResult::Rejected;
        }
        if (!m_root.empty() && (root != m_root || request.Pid != m_clientPid || request.TargetId != m_targetId))
        {
            m_error = E_INVALIDARG;
            RequestRetire();
            return BrokerImportResult::Unavailable;
        }
        m_root = root;
        m_clientPid = request.Pid;
        m_targetId = request.TargetId;
        for (auto &generation : m_generations)
            if (generation && (!Current(*generation) || !SameIdentity(generation->Offer.Client, client)))
                BeginRetire(*generation);
        PollRetirement();
        if (m_active >= 0 && Current(*m_generations[m_active]))
        {
            m_error = S_OK;
            return BrokerImportResult::Ready;
        }
        // A new offer may recycle the old generation's five kernel objects.
        // Do not claim it until every old lease/write/import has fully retired;
        // within-offer uniqueness cannot prove independence across epochs.
        if (!Idle())
        {
            m_error = E_PENDING;
            return BrokerImportResult::Pending;
        }
        auto mapping = std::make_shared<GpuControlMapping>();
        if (!mapping->Open(detail::Path(root, request.Pid, request.TargetId, L".gpu")))
        {
            m_error = HRESULT_FROM_WIN32(GetLastError());
            return BrokerImportResult::Pending;
        }
        auto &control = *mapping->Get();
        if (!SameIdentity(control.Header.Client, client) || control.Header.TargetId != request.TargetId)
        {
            m_error = E_INVALIDARG;
            return BrokerImportResult::Rejected;
        }
        // Only the fresh current normal broker may perform exact-death recovery.
        for (auto &record : control.Records)
            TryAbandonDeadBroker(record);
        // A predecessor target may be retained by detached retirement rather
        // than this instance. Its live wire pin must drain before any new claim.
        // Exact-death recovery above is the only way to bypass a dead importer.
        for (auto const &record : control.Records)
        {
            const auto phase = Phase(record);
            if (phase == GpuPhase::Claiming || phase == GpuPhase::Imported)
            {
                m_error = E_PENDING;
                return BrokerImportResult::Pending;
            }
        }
        int empty = -1;
        for (int i = 0; i < static_cast<int>(m_generations.size()); ++i)
            if (!m_generations[i])
            {
                empty = i;
                break;
            }
        if (empty < 0)
        {
            m_error = HRESULT_FROM_WIN32(ERROR_BUSY);
            return BrokerImportResult::Pending;
        }
        uint32_t width = 0, height = 0;
        if (!detail::Dimensions(request, width, height))
        {
            m_error = E_INVALIDARG;
            return BrokerImportResult::Rejected;
        }
        GpuClaimExpectation expected{client,
                                     CurrentIdentity(),
                                     m_owner.Nonce,
                                     m_adapter,
                                     request.TargetId,
                                     control.Header.ClientEpoch,
                                     request.Revision,
                                     width,
                                     height};
        for (uint32_t i = 0; i < GenerationRecordCount; ++i)
        {
            // Allocation occurs before Claiming, so allocation failure
            // cannot leave a client record pinned by this live broker.
            auto generation = std::make_unique<Generation>();
            GpuOffer offer{};
            auto claimed = TryClaimSnapshot(control, i, verifiedClientProcess, expected, offer);
            if (claimed == GpuClaimResult::NotClaimed)
                continue;
            if (claimed == GpuClaimResult::Rejected)
            {
                m_error = control.Records[i].Life.Result;
                return BrokerImportResult::Rejected;
            }
            // From this point every exit must release Claiming after imports drop.
            generation->Mapping = mapping;
            generation->Offer = offer;
            generation->RecordIndex = i;
            HRESULT result = OpenImports(*generation, verifiedClientProcess);
            try
            {
                if (SUCCEEDED(result) && (!HasOwner(root) || !Healthy()))
                    result = m_error;
            }
            catch (...)
            {
                result = E_OUTOFMEMORY;
            }
            if (SUCCEEDED(result) && (!SameIdentity(Identity(verifiedClientProcess, clientPid), client) ||
                                      WaitForSingleObject(verifiedClientProcess, 0) != WAIT_TIMEOUT))
                result = E_ABORT;
            if (SUCCEEDED(result) && Cancelled(control.Records[i]))
                result = E_ABORT;
            if (FAILED(result))
            {
                generation.reset();
                CompleteImport(control.Records[i], result);
                m_error = result;
                return BrokerImportResult::Rejected;
            }
            if (!CompleteImport(control.Records[i], S_OK) || Phase(control.Records[i]) != GpuPhase::Imported)
            {
                generation.reset();
                m_error = E_ABORT;
                return BrokerImportResult::Rejected;
            }
            m_generations[empty] = std::move(generation);
            m_active = empty;
            if (!ActivateRecord(control, i))
            {
                BeginRetire(*m_generations[empty]);
                PollRetirement();
                m_error = E_ABORT;
                return BrokerImportResult::Rejected;
            }
            m_error = S_OK;
            return BrokerImportResult::Ready;
        }
        m_error = E_PENDING;
        return BrokerImportResult::Pending;
    }
    // The latest-only frame mapping cannot advertise a predecessor once it
    // publishes a newer frame. Do not supersede that frame until the client
    // has acknowledged its exact read and the consumed fence has completed.
    std::optional<WritableGpuSlot> TryAcquireWritableSlot(GpuExpectedFrame const *advertised = nullptr)
    {
        if (!OnThread() || !Healthy())
            return std::nullopt;
        PollRetirement();
        if (m_active < 0)
        {
            m_error = E_PENDING;
            return std::nullopt;
        }
        if (!HasOwner(m_root))
        {
            RequestRetire();
            return std::nullopt;
        }
        if (!detail::Fresh(m_request.HeartbeatTick))
        {
            m_error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            RequestRetire();
            return std::nullopt;
        }
        auto &generation = *m_generations[m_active];
        if (!Current(generation))
        {
            BeginRetire(generation);
            return std::nullopt;
        }
        uint64_t ready = 0, consumed = 0;
        if (!FenceValue(generation.Ready.Get(), ready) || !FenceValue(generation.Consumed.Get(), consumed))
        {
            BeginRetire(generation, m_error);
            return std::nullopt;
        }
        if (advertised)
        {
            const auto index = advertised->TextureSlot;
            if (index >= TextureSlotCount || advertised->Generation != generation.Offer.Generation ||
                advertised->Revision != generation.Offer.RequestRevision || !advertised->SourceSerial ||
                advertised->SourceSerial != generation.LastPublishedSerial ||
                advertised->SourceSerial != generation.Slots[index].LastSerial ||
                advertised->ReadyFenceValue != generation.Slots[index].SealValue)
            {
                m_error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return std::nullopt;
            }
            if (!CanReuseSlot(generation.Record().Consumed[index], advertised->Generation,
                              advertised->SourceSerial, consumed))
            {
                m_error = E_PENDING;
                return std::nullopt;
            }
        }
        uint64_t leasedCount = 0;
        for (auto const &slot : generation.Slots)
            if (slot.LeaseToken)
                ++leasedCount;
        // Reserve enough non-sentinel fence values to seal every live lease,
        // including those the caller later cancels instead of publishing.
        if (UINT64_MAX - 1 - generation.LastReady <= leasedCount)
        {
            m_error = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
            BeginRetire(generation, m_error);
            return std::nullopt;
        }
        for (uint32_t i = 0; i < TextureSlotCount; ++i)
        {
            auto &slot = generation.Slots[i];
            if (slot.LeaseToken || ready < slot.SealValue ||
                (slot.LastSerial && !CanReuseSlot(generation.Record().Consumed[i], generation.Offer.Generation,
                                                  slot.LastSerial, consumed)))
                continue;
            if (m_nextLease == UINT64_MAX)
            {
                m_error = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
                BeginRetire(generation);
                return std::nullopt;
            }
            slot.LastSerial = 0;
            slot.LeaseToken = ++m_nextLease;
            WritableGpuSlot lease;
            lease.m_anchor = m_anchor;
            lease.m_target = slot.Target;
            lease.m_token = slot.LeaseToken;
            lease.m_entry = static_cast<uint32_t>(m_active);
            lease.m_slot = i;
            lease.Generation = generation.Offer.Generation;
            lease.Revision = generation.Offer.RequestRevision;
            lease.Index = i;
            lease.Width = generation.Offer.Width;
            lease.Height = generation.Offer.Height;
            m_error = S_OK;
            return lease;
        }
        m_error = E_PENDING;
        return std::nullopt;
    }
    std::optional<GpuExpectedFrame> Publish(WritableGpuSlot &&lease, uint64_t exactSourceSerial, PixelRect bitmapBounds,
                                            uint64_t originalPublishedTick)
    {
        if (!OnThread() || lease.m_entry >= m_generations.size() || !m_generations[lease.m_entry])
            return std::nullopt;
        auto &generation = *m_generations[lease.m_entry];
        if (!Matches(generation, lease))
        {
            m_error = E_INVALIDARG;
            return std::nullopt;
        }
        m_error = S_OK;
        if (!Current(generation) || !Healthy() || !HasOwner(m_root) || !detail::Fresh(m_request.HeartbeatTick) ||
            !exactSourceSerial || exactSourceSerial <= generation.LastPublishedSerial ||
            !detail::Fresh(originalPublishedTick) ||
            !broker_detail::SameRect(bitmapBounds, detail::CanvasBounds(m_request)))
        {
            if (SUCCEEDED(m_error))
                m_error = E_INVALIDARG;
            CancelDraw(std::move(lease));
            return std::nullopt;
        }
        auto &slot = generation.Slots[lease.Index];
        if (!Seal(generation, slot))
        {
            lease.m_token = 0;
            lease.m_target.Reset();
            return std::nullopt;
        }
        slot.LastSerial = exactSourceSerial;
        generation.LastPublishedSerial = exactSourceSerial;
        GpuExpectedFrame frame{generation.Offer.RequestRevision,
                               generation.Offer.Generation,
                               exactSourceSerial,
                               slot.SealValue,
                               lease.Index,
                               generation.Offer.Width,
                               generation.Offer.Height,
                               bitmapBounds,
                               originalPublishedTick};
        lease.m_token = 0;
        lease.m_target.Reset();
        m_error = S_OK;
        return frame;
    }
    void CancelDraw(WritableGpuSlot &&lease) noexcept
    {
        auto anchor = lease.m_anchor.lock();
        if (!anchor || anchor != m_anchor)
        {
            m_error = E_INVALIDARG;
            return;
        }
        CancelLease(lease.m_entry, lease.m_slot, lease.m_token);
        lease.m_token = 0;
        lease.m_target.Reset();
    }
    void RequestRetire() noexcept
    {
        if (!OnThread())
            return;
        for (auto &generation : m_generations)
            if (generation)
                BeginRetire(*generation);
        m_active = -1;
    }
    void PollRetirement() noexcept
    {
        if (!OnThread())
            return;
        const bool deviceGone = !Healthy();
        for (size_t i = 0; i < m_generations.size(); ++i)
            if (m_generations[i])
            {
                auto &generation = *m_generations[i];
                if (Cancelled(generation.Record()) || deviceGone)
                    BeginRetire(generation, deviceGone ? m_error : S_OK);
                if (!generation.Retiring)
                    continue;
                bool leased = false;
                for (auto const &slot : generation.Slots)
                    if (slot.LeaseToken)
                        leased = true;
                if (leased && !deviceGone)
                    continue;
                if (generation.Unsealed && !deviceGone)
                {
                    if (generation.CannotSeal)
                        continue;
                    // Signal failed after caller commands may have been queued.
                    // Retry a fence behind those commands; retain ownership if
                    // a live device still cannot submit the completion proof.
                    const HRESULT result = m_context->Signal(generation.Ready.Get(), generation.LastReady);
                    if (FAILED(result))
                    {
                        m_error = result;
                        continue;
                    }
                    generation.Unsealed = false;
                    m_context->Flush();
                }
                uint64_t completed = 0;
                const bool fenceAlive = FenceValue(generation.Ready.Get(), completed);
                if (deviceGone || !fenceAlive || completed >= generation.LastReady)
                    DropRetired(i, deviceGone || !fenceAlive);
            }
    }
    bool Idle() const noexcept
    {
        for (auto const &generation : m_generations)
            if (generation)
                return false;
        return true;
    }
    // Call from the same sole broker MTA even when no request targets remain.
    // At most 64 initialized/detached target owners exist in this process;
    // each owner holds at most the two protocol generation records.
    static void PollDetachedRetirements() noexcept;
    HRESULT LastError() const noexcept
    {
        return m_error;
    }
    uint64_t ActiveGeneration() const noexcept
    {
        return m_active >= 0 && m_generations[m_active] ? m_generations[m_active]->Offer.Generation : 0;
    }
    uint64_t ActiveRevision() const noexcept
    {
        return m_active >= 0 && m_generations[m_active] ? m_generations[m_active]->Offer.RequestRevision : 0;
    }
};
struct BrokerGpuInputTarget::RetirementRegistry
{
    struct Entry
    {
        bool Reserved = false;
        std::unique_ptr<BrokerGpuInputTarget> Detached;
    };
    std::array<Entry, 64> Entries{};
};
inline BrokerGpuInputTarget::RetirementRegistry &BrokerGpuInputTarget::Registry()
{
    // A fixed-size process-lifetime registry avoids static teardown racing
    // the D3D runtime. The normal broker MTA explicitly polls its contents.
    static auto *registry = new RetirementRegistry();
    return *registry;
}
inline bool BrokerGpuInputTarget::ReserveRetirement()
{
    auto &entries = Registry().Entries;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].Reserved)
        {
            m_retirementSpare = std::make_unique<BrokerGpuInputTarget>();
            entries[i].Reserved = true;
            m_retirementIndex = static_cast<int>(i);
            return true;
        }
    return false;
}
inline void BrokerGpuInputTarget::ReleaseRetirement() noexcept
{
    if (m_retirementIndex >= 0)
        Registry().Entries[m_retirementIndex].Reserved = false;
    m_retirementIndex = -1;
}
inline void BrokerGpuInputTarget::DetachRetirement() noexcept
{
    auto detached = std::move(m_retirementSpare);
    detached->m_device = std::move(m_device);
    detached->m_context = std::move(m_context);
    detached->m_owner = m_owner;
    detached->m_adapter = m_adapter;
    detached->m_root = std::move(m_root);
    detached->m_request = m_request;
    detached->m_generations = std::move(m_generations);
    detached->m_anchor = std::move(m_anchor);
    detached->m_nextLease = m_nextLease;
    detached->m_thread = m_thread;
    detached->m_clientPid = m_clientPid;
    detached->m_targetId = m_targetId;
    detached->m_error = m_error;
    detached->m_active = -1;
    detached->m_detached = true;
    detached->m_retirementIndex = std::exchange(m_retirementIndex, -1);
    detached->m_anchor->Owner = detached.get();
    Registry().Entries[detached->m_retirementIndex].Detached = std::move(detached);
}
inline void BrokerGpuInputTarget::PollDetachedRetirements() noexcept
{
    for (auto &entry : Registry().Entries)
        if (entry.Detached)
        {
            entry.Detached->PollRetirement();
            if (entry.Detached->Idle())
                entry.Detached.reset();
        }
}
inline void WritableGpuSlot::Release() noexcept
{
    if (m_token)
        if (auto anchor = m_anchor.lock(); anchor && anchor->Owner)
            anchor->Owner->CancelLease(m_entry, m_slot, m_token);
    m_token = 0;
    m_target.Reset();
}
} // namespace novabar::capture::gpu
