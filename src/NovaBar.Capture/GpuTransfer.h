#pragma once
#include "CaptureProtocol.h"
#include <array>
#include <limits>
#include <utility>

// Client-created resources; only the normal broker pulls handles into itself.
namespace novabar::capture::gpu
{
    inline constexpr uint32_t GpuProtocolVersion = 1;
    inline constexpr uint32_t OwnerMagic = 0x4e42474f; // NBGO
    inline constexpr uint32_t OfferMagic = 0x4e424746; // NBGF
    inline constexpr uint32_t ControlMagic = 0x4e424743; // NBGC
    inline constexpr uint32_t TextureSlotCount = 3, GenerationRecordCount = 2;
    inline constexpr uint32_t BgraFormat = 87, SharedTextureMiscFlags = 0x802;
    inline constexpr uint64_t MaxGenerationBytes = MaxFrameBytes * TextureSlotCount;
    inline constexpr uint64_t MaxControlBytes = 4096;
    static_assert(sizeof(void*) == 8, "GPU transport requires the x64 host");

    enum class GpuPhase : int64_t { Empty = 0, Offered = 1, Claiming = 2, Imported = 3, Rejected = 4, Retired = 5, Abandoned = 6 };
    struct ProcessIdentity { uint32_t Pid = 0, Reserved = 0; uint64_t CreationFileTime = 0; };
    struct GpuOwnerRecord {
        uint32_t Magic = OwnerMagic, Version = gpu::GpuProtocolVersion, Bytes = sizeof(GpuOwnerRecord), Pid = 0;
        uint64_t CreationFileTime = 0, Nonce = 0, HeartbeatTick = 0;
        uint32_t AdapterLow = 0; int32_t AdapterHigh = 0;
        uint32_t GpuProtocolVersion = gpu::GpuProtocolVersion, Flags = 0;
        uint64_t Reserved = 0;
    };
    struct GpuOffer {
        uint32_t Magic = OfferMagic, Version = GpuProtocolVersion, Bytes = sizeof(GpuOffer), SlotCount = TextureSlotCount;
        uint64_t TargetId = 0, ClientEpoch = 0, Generation = 0, RequestRevision = 0;
        ProcessIdentity Client{}, ExpectedBroker{};
        uint32_t AdapterLow = 0; int32_t AdapterHigh = 0;
        uint32_t Width = 0, Height = 0, DxgiFormat = BgraFormat, MiscFlags = SharedTextureMiscFlags;
        uint64_t TextureHandle[3]{}, ReadyFenceHandle = 0, ConsumedFenceHandle = 0;
        uint64_t BrokerEpoch = 0;
        uint64_t Reserved[5]{};
    };
    struct alignas(64) GpuLifecycle {
        alignas(8) volatile LONG64 Phase = static_cast<LONG64>(GpuPhase::Empty);
        alignas(8) volatile LONG64 CancelRequested = 0;
        int32_t Result = S_OK; uint32_t Reserved0 = 0;
        uint64_t Reserved[5]{};
    };
    struct GpuSlotExtension {
        uint64_t Generation = 0, ReadyFenceValue = 0;
        uint32_t TextureSlot = 0, Reserved0 = 0;
        uint64_t Reserved1 = 0;
    };
    struct alignas(64) GpuConsumedSlot {
        alignas(8) volatile LONG64 Sequence = 0;
        uint64_t Generation = 0, SourceSerial = 0, FenceValue = 0;
        uint64_t Reserved[4]{};
    };
    struct alignas(64) GpuGenerationRecord { GpuOffer Offer{}; GpuLifecycle Life{}; GpuConsumedSlot Consumed[3]{}; };
    struct alignas(64) GpuChannelHeader {
        uint32_t Magic = ControlMagic, Version = GpuProtocolVersion, Bytes = 960, RecordCount = GenerationRecordCount;
        ProcessIdentity Client{};
        uint64_t TargetId = 0, ClientEpoch = 0;
        alignas(8) volatile LONG64 ActiveRecord = -1;
        uint64_t Reserved = 0;
    };
    struct alignas(64) GpuControl { GpuChannelHeader Header{}; GpuGenerationRecord Records[2]{}; };
    static_assert(sizeof(ProcessIdentity) == 16 && sizeof(GpuOwnerRecord) == 64 && sizeof(GpuOffer) == 192);
    static_assert(sizeof(GpuLifecycle) == 64 && sizeof(GpuConsumedSlot) == 64 && sizeof(GpuGenerationRecord) == 448);
    static_assert(sizeof(GpuChannelHeader) == 64 && sizeof(GpuControl) == 960 && sizeof(GpuSlotExtension) == 32);
    static_assert(offsetof(GpuOffer, Client) == 48 && offsetof(GpuOffer, ExpectedBroker) == 64
        && offsetof(GpuOffer, TextureHandle) == 104 && offsetof(GpuOffer, BrokerEpoch) == 144);
    static_assert(offsetof(GpuGenerationRecord, Life) == 192 && offsetof(GpuGenerationRecord, Consumed) == 256);
    static_assert(offsetof(GpuControl, Records) == 64 && offsetof(GpuChannelHeader, ActiveRecord) == 48);
    static_assert(std::is_trivially_copyable_v<GpuControl> && std::is_standard_layout_v<GpuControl>);

    struct GpuExpectedFrame {
        uint64_t Revision = 0, Generation = 0, SourceSerial = 0, ReadyFenceValue = 0;
        uint32_t TextureSlot = 0, Width = 0, Height = 0;
        PixelRect BitmapBounds{};
        uint64_t PublishedTick = 0;
    };
    enum class GpuClaimResult { NotClaimed, Rejected, Claimed };
    struct GpuClaimExpectation {
        ProcessIdentity Client{}, Broker{};
        uint64_t BrokerNonce = 0;
        LUID Adapter{};
        uint64_t TargetId = 0, ClientEpoch = 0, RequestRevision = 0;
        uint32_t Width = 0, Height = 0;
    };
    inline GpuExpectedFrame ExpectedFrame(Frame const& frame) noexcept {
        return {frame.Revision, frame.Generation, frame.Serial, frame.ReadyFenceValue,
            frame.TextureSlot, frame.Width, frame.Height, frame.BitmapBounds, frame.PublishedTick};
    }
    inline bool ValidIdentity(ProcessIdentity const& identity) noexcept
    { return identity.Pid && !identity.Reserved && identity.CreationFileTime; }
    inline bool SameIdentity(ProcessIdentity const& a, ProcessIdentity const& b) noexcept
    { return ValidIdentity(a) && ValidIdentity(b) && a.Pid == b.Pid && a.CreationFileTime == b.CreationFileTime; }
    inline ProcessIdentity Identity(HANDLE process, uint32_t pid) noexcept {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!pid || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
        return {pid, 0, (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
    }
    inline ProcessIdentity CurrentIdentity() noexcept { return Identity(GetCurrentProcess(), GetCurrentProcessId()); }
    inline bool ValidOwner(GpuOwnerRecord const& owner, uint64_t now) noexcept {
        return owner.Magic == OwnerMagic && owner.Version == GpuProtocolVersion && owner.Bytes == sizeof(owner)
            && ValidIdentity({owner.Pid, 0, owner.CreationFileTime}) && owner.Nonce
            && (owner.AdapterLow || owner.AdapterHigh) && owner.GpuProtocolVersion == GpuProtocolVersion
            && !owner.Flags && !owner.Reserved && detail::Fresh(owner.HeartbeatTick, now);
    }
    inline bool SameOwner(GpuOwnerRecord const& a, GpuOwnerRecord const& b) noexcept {
        return SameIdentity({a.Pid, 0, a.CreationFileTime}, {b.Pid, 0, b.CreationFileTime}) && a.Nonce && a.Nonce == b.Nonce;
    }
    inline bool ValidExpectedFrame(GpuExpectedFrame const& f, uint64_t now = GetTickCount64()) noexcept {
        return f.Revision && f.Generation && f.SourceSerial && f.ReadyFenceValue
            && f.ReadyFenceValue != UINT64_MAX && f.TextureSlot < TextureSlotCount && f.Width && f.Height
            && f.Width <= 16384 && f.Height <= 16384 && static_cast<uint64_t>(f.Width) * f.Height <= MaxPixels
            && f.BitmapBounds.Right > f.BitmapBounds.Left && f.BitmapBounds.Bottom > f.BitmapBounds.Top
            && detail::Fresh(f.PublishedTick, now);
    }
    inline bool ValidOffer(GpuOffer const& o, ProcessIdentity const& client, ProcessIdentity const& broker,
        uint64_t brokerNonce, LUID adapter, uint64_t targetId, uint64_t clientEpoch) noexcept {
        if (o.Magic != OfferMagic || o.Version != GpuProtocolVersion || o.Bytes != sizeof(o) || o.SlotCount != TextureSlotCount
            || !o.TargetId || o.TargetId != targetId || !o.ClientEpoch || o.ClientEpoch != clientEpoch
            || !o.Generation || !o.RequestRevision || !SameIdentity(o.Client, client) || !SameIdentity(o.ExpectedBroker, broker)
            || !brokerNonce || o.BrokerEpoch != brokerNonce || (!adapter.LowPart && !adapter.HighPart)
            || o.AdapterLow != adapter.LowPart || o.AdapterHigh != adapter.HighPart
            || !o.Width || !o.Height || o.Width > 16384 || o.Height > 16384
            || static_cast<uint64_t>(o.Width) * o.Height > MaxPixels || o.DxgiFormat != BgraFormat
            || o.MiscFlags != SharedTextureMiscFlags) return false;
        for (auto value : o.Reserved) if (value) return false;
        const uint64_t handles[]{o.TextureHandle[0], o.TextureHandle[1], o.TextureHandle[2], o.ReadyFenceHandle, o.ConsumedFenceHandle};
        for (size_t i = 0; i < std::size(handles); ++i) {
            if (!handles[i] || handles[i] == UINT64_MAX) return false;
            for (size_t j = 0; j < i; ++j) if (handles[i] == handles[j]) return false;
        }
        return true;
    }
    inline LONG64 AtomicRead(volatile LONG64 const& value) noexcept
    { return InterlockedCompareExchange64(const_cast<volatile LONG64*>(&value), 0, 0); }
    inline GpuPhase Phase(GpuGenerationRecord const& record) noexcept { return static_cast<GpuPhase>(AtomicRead(record.Life.Phase)); }
    inline bool Cancelled(GpuGenerationRecord const& record) noexcept { return AtomicRead(record.Life.CancelRequested) != 0; }
    inline bool ValidLifecycle(GpuLifecycle const& life) noexcept {
        const auto phase = AtomicRead(life.Phase), cancelled = AtomicRead(life.CancelRequested);
        if (phase < static_cast<LONG64>(GpuPhase::Empty) || phase > static_cast<LONG64>(GpuPhase::Abandoned)
            || (cancelled != 0 && cancelled != 1) || life.Reserved0) return false;
        for (auto reserved : life.Reserved) if (reserved) return false;
        return true;
    }
    inline bool Terminal(GpuPhase phase) noexcept
    { return phase == GpuPhase::Rejected || phase == GpuPhase::Retired || phase == GpuPhase::Abandoned; }
    inline bool Transition(GpuGenerationRecord& record, GpuPhase from, GpuPhase to) noexcept {
        return InterlockedCompareExchange64(&record.Life.Phase, static_cast<LONG64>(to), static_cast<LONG64>(from)) == static_cast<LONG64>(from);
    }
    // Single client writer; only Empty records may have immutable offer bytes filled.
    inline bool PublishOffer(GpuGenerationRecord& record, GpuOffer const& offer) noexcept {
        if (Phase(record) != GpuPhase::Empty || !ValidLifecycle(record.Life) || Cancelled(record)
            || !ValidOffer(offer, offer.Client, offer.ExpectedBroker, offer.BrokerEpoch,
            {offer.AdapterLow, offer.AdapterHigh}, offer.TargetId, offer.ClientEpoch)) return false;
        record.Offer = offer;
        record.Life.Result = S_OK;
        return Transition(record, GpuPhase::Empty, GpuPhase::Offered);
    }
    inline bool TryClaim(GpuGenerationRecord& record) noexcept {
        return ValidLifecycle(record.Life) && !Cancelled(record) && Transition(record, GpuPhase::Offered, GpuPhase::Claiming);
    }
    inline void RequestCancel(GpuGenerationRecord& record) noexcept {
        const auto phase = Phase(record);
        if (phase == GpuPhase::Empty || Terminal(phase)) return;
        InterlockedExchange64(&record.Life.CancelRequested, 1);
        Transition(record, GpuPhase::Offered, GpuPhase::Retired);
    }
    // Only the broker that won Claiming may write Result/finish import. It must
    // have stopped all handle duplication before publishing either terminal ack.
    inline bool CompleteImport(GpuGenerationRecord& record, HRESULT result) noexcept {
        if (Phase(record) != GpuPhase::Claiming) return false;
        if (Cancelled(record) && SUCCEEDED(result)) result = E_ABORT;
        record.Life.Result = result;
        return Transition(record, GpuPhase::Claiming, SUCCEEDED(result) ? GpuPhase::Imported : GpuPhase::Rejected);
    }
    // Broker calls only after its submitted writes finish and imports are dropped.
    inline bool CompleteRetirement(GpuGenerationRecord& record) noexcept {
        return Cancelled(record) && Transition(record, GpuPhase::Imported, GpuPhase::Retired);
    }
    inline bool CanDispose(GpuGenerationRecord const& record, bool clientReadsComplete) noexcept
    { return clientReadsComplete && Terminal(Phase(record)); }
    inline bool ResetTerminal(GpuGenerationRecord& record, bool clientReadsComplete) noexcept {
        if (!CanDispose(record, clientReadsComplete)) return false;
        record.Offer = {}; record.Life.Result = S_OK;
        for (auto& consumed : record.Consumed) { consumed.Generation = consumed.SourceSerial = consumed.FenceValue = 0; InterlockedExchange64(&consumed.Sequence, 0); }
        InterlockedExchange64(&record.Life.CancelRequested, 0);
        InterlockedExchange64(&record.Life.Phase, static_cast<LONG64>(GpuPhase::Empty));
        return true;
    }
    inline bool ValidControl(GpuControl const& c, uint64_t fileBytes = sizeof(GpuControl)) noexcept {
        const auto& h = c.Header;
        const auto active = AtomicRead(h.ActiveRecord);
        return fileBytes >= sizeof(GpuControl) && fileBytes <= MaxControlBytes && h.Magic == ControlMagic
            && h.Version == GpuProtocolVersion && h.Bytes == sizeof(GpuControl) && h.RecordCount == GenerationRecordCount
            && ValidIdentity(h.Client) && h.TargetId && h.ClientEpoch && !h.Reserved && active >= -1 && active < GenerationRecordCount;
    }
    // Production importer entry point. Preclaim copies are observations only:
    // the client may cancel/reset/reoffer before our Offered CAS. Claim first,
    // then read the actual immutable offer whose original handles are pinned.
    // verifiedClientProcess needs QUERY_LIMITED_INFORMATION and SYNCHRONIZE
    // (and DUP_HANDLE for the caller's subsequent imports).
    inline GpuClaimResult TryClaimSnapshot(GpuControl& control, uint32_t recordIndex, HANDLE verifiedClientProcess,
        GpuClaimExpectation const& expected, GpuOffer& out) noexcept {
        out = {};
        if (recordIndex >= GenerationRecordCount || !ValidControl(control)) return GpuClaimResult::NotClaimed;
        auto& record = control.Records[recordIndex];
        if (!TryClaim(record)) return GpuClaimResult::NotClaimed;
        const GpuOffer claimed = record.Offer;
        const auto reject = [&](HRESULT error) {
            // No handle values were exposed and no DuplicateHandle was issued.
            CompleteImport(record, error);
            return GpuClaimResult::Rejected;
        };
        if (Cancelled(record)) return reject(E_ABORT);
        const auto clientPid = verifiedClientProcess && verifiedClientProcess != INVALID_HANDLE_VALUE
            ? GetProcessId(verifiedClientProcess) : 0;
        const auto client = Identity(verifiedClientProcess, clientPid);
        if (!ValidControl(control) || !SameIdentity(client, expected.Client)
            || WaitForSingleObject(verifiedClientProcess, 0) != WAIT_TIMEOUT
            || !SameIdentity(expected.Broker, CurrentIdentity())
            || !SameIdentity(control.Header.Client, client)
            || control.Header.TargetId != expected.TargetId || control.Header.ClientEpoch != expected.ClientEpoch
            || !ValidOffer(claimed, client, expected.Broker, expected.BrokerNonce, expected.Adapter,
                expected.TargetId, expected.ClientEpoch)
            || claimed.RequestRevision != expected.RequestRevision || claimed.Width != expected.Width || claimed.Height != expected.Height)
            return reject(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        if (Cancelled(record)) return reject(E_ABORT);
        out = claimed;
        return GpuClaimResult::Claimed;
    }
    inline void InitializeControl(GpuControl& c, ProcessIdentity client, uint64_t target, uint64_t epoch) noexcept {
        new (&c) GpuControl{}; c.Header.Client = client; c.Header.TargetId = target; c.Header.ClientEpoch = epoch;
    }
    inline int FindEmptyRecord(GpuControl const& c) noexcept {
        if (!ValidControl(c)) return -1;
        for (int i = 0; i < static_cast<int>(GenerationRecordCount); ++i) if (Phase(c.Records[i]) == GpuPhase::Empty) return i;
        return -1;
    }
    inline bool ActivateRecord(GpuControl& c, uint32_t index) noexcept {
        if (!ValidControl(c) || index >= GenerationRecordCount || Phase(c.Records[index]) != GpuPhase::Imported || Cancelled(c.Records[index])) return false;
        InterlockedExchange64(&c.Header.ActiveRecord, index); return true;
    }
    struct GpuConsumedAck { uint64_t Generation = 0, SourceSerial = 0, FenceValue = 0; };
    // Caller has already queued the sole client consumed-fence signal behind its final read.
    inline bool PublishConsumed(GpuConsumedSlot& slot, GpuConsumedAck ack) noexcept {
        if (!ack.Generation || !ack.SourceSerial || !ack.FenceValue || ack.FenceValue == UINT64_MAX) return false;
        const auto sequence = AtomicRead(slot.Sequence);
        if (sequence < 0 || (sequence & 1)) return false;
        if (sequence != 0 && (slot.Generation != ack.Generation || ack.SourceSerial <= slot.SourceSerial
            || ack.FenceValue <= slot.FenceValue)) return false;
        InterlockedIncrement64(&slot.Sequence);
        slot.Generation = ack.Generation; slot.SourceSerial = ack.SourceSerial; slot.FenceValue = ack.FenceValue;
        InterlockedIncrement64(&slot.Sequence); return true;
    }
    inline bool ReadConsumed(GpuConsumedSlot const& slot, GpuConsumedAck& out) noexcept {
        out = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto sequence = AtomicRead(slot.Sequence); if (sequence <= 0 || (sequence & 1)) continue;
            const GpuConsumedAck value{slot.Generation, slot.SourceSerial, slot.FenceValue};
            if (sequence != AtomicRead(slot.Sequence)) continue;
            for (auto reserved : slot.Reserved) if (reserved) return false;
            if (!value.Generation || !value.SourceSerial || !value.FenceValue || value.FenceValue == UINT64_MAX) return false;
            out = value; return true;
        }
        return false;
    }
    inline bool CanReuseSlot(GpuConsumedSlot const& slot, uint64_t generation, uint64_t lastSerial, uint64_t completedFence) noexcept {
        if (!generation || !lastSerial || completedFence == UINT64_MAX) return false;
        GpuConsumedAck ack{};
        return ReadConsumed(slot, ack) && ack.Generation == generation && ack.SourceSerial == lastSerial && completedFence >= ack.FenceValue;
    }
    enum class ExactProcessState { Alive, Dead, Unknown };
    // Only the normal broker probes process identity. Access denied stays Unknown.
    inline ExactProcessState ProbeExactProcess(ProcessIdentity expected) noexcept {
        if (!ValidIdentity(expected)) return ExactProcessState::Unknown;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, expected.Pid);
        if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? ExactProcessState::Dead : ExactProcessState::Unknown;
        const auto identity = Identity(process, expected.Pid);
        ExactProcessState state = ExactProcessState::Unknown;
        if (ValidIdentity(identity)) {
            if (!SameIdentity(identity, expected)) state = ExactProcessState::Dead;
            else {
                const DWORD wait = WaitForSingleObject(process, 0);
                if (wait == WAIT_OBJECT_0) state = ExactProcessState::Dead;
                else if (wait == WAIT_TIMEOUT) state = ExactProcessState::Alive;
            }
        }
        CloseHandle(process); return state;
    }
    inline bool TryAbandonDeadBroker(GpuGenerationRecord& record) noexcept {
        // Caller holds the fresh owner-file identity and is the sole current
        // normal broker importer/poller (one serialized MTA) for this
        // target. Clients never probe and brokers must not poll the same target
        // concurrently: terminal record recycling otherwise permits phase ABA.
        const auto phase = Phase(record);
        const auto expectedBroker = record.Offer.ExpectedBroker;
        const auto nonce = record.Offer.BrokerEpoch, generation = record.Offer.Generation;
        if ((phase != GpuPhase::Claiming && phase != GpuPhase::Imported) || !nonce || !generation
            || ProbeExactProcess(expectedBroker) != ExactProcessState::Dead
            || !SameIdentity(record.Offer.ExpectedBroker, expectedBroker)
            || record.Offer.BrokerEpoch != nonce || record.Offer.Generation != generation) return false;
        return Transition(record, phase, GpuPhase::Abandoned);
    }

    // RAII accepts only handles returned into this process by DuplicateHandle.
    // Opaque offer numbers are never handed to CloseHandle.
    class LocalHandle {
        HANDLE m_value = nullptr;
    public:
        LocalHandle() = default;
        LocalHandle(LocalHandle const&) = delete;
        LocalHandle& operator=(LocalHandle const&) = delete;
        LocalHandle(LocalHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
        ~LocalHandle() { Reset(); }
        void Reset() noexcept { if (m_value) CloseHandle(std::exchange(m_value, nullptr)); }
        HANDLE Get() const noexcept { return m_value; }
        bool Pull(HANDLE verifiedClientProcess, uint64_t clientValue) noexcept {
            Reset();
            if (!clientValue || clientValue == UINT64_MAX || !verifiedClientProcess || verifiedClientProcess == INVALID_HANDLE_VALUE) return false;
            return !!DuplicateHandle(verifiedClientProcess, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(clientValue)),
                GetCurrentProcess(), &m_value, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }
    };
    class GpuControlMapping {
        HANDLE m_file = INVALID_HANDLE_VALUE, m_mapping = nullptr;
        GpuControl* m_view = nullptr;
        bool Map(bool initialize, ProcessIdentity client = {}, uint64_t target = 0, uint64_t epoch = 0) noexcept {
            LARGE_INTEGER bytes{};
            if (initialize) {
                bytes.QuadPart = sizeof(GpuControl);
                if (!SetFilePointerEx(m_file, bytes, nullptr, FILE_BEGIN) || !SetEndOfFile(m_file)) return false;
            } else if (!GetFileSizeEx(m_file, &bytes) || bytes.QuadPart < sizeof(GpuControl) || bytes.QuadPart > MaxControlBytes) {
                SetLastError(ERROR_INVALID_DATA); return false;
            }
            m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
            if (!m_mapping) return false;
            m_view = static_cast<GpuControl*>(MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(GpuControl)));
            if (!m_view) return false;
            if (initialize) InitializeControl(*m_view, client, target, epoch);
            if (!ValidControl(*m_view, bytes.QuadPart)) { SetLastError(ERROR_INVALID_DATA); return false; }
            return true;
        }
    public:
        GpuControlMapping() = default;
        GpuControlMapping(GpuControlMapping const&) = delete;
        GpuControlMapping& operator=(GpuControlMapping const&) = delete;
        ~GpuControlMapping() { Close(); }
        void Close() noexcept {
            if (m_view) UnmapViewOfFile(m_view);
            if (m_mapping) CloseHandle(m_mapping);
            if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
            m_view = nullptr; m_mapping = nullptr; m_file = INVALID_HANDLE_VALUE;
        }
        bool Create(std::wstring const& path, ProcessIdentity client, uint64_t target, uint64_t epoch) noexcept {
            Close();
            if (!ValidIdentity(client) || !target || !epoch) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
            m_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (m_file != INVALID_HANDLE_VALUE && Map(true, client, target, epoch)) return true;
            const auto error = GetLastError(); Close(); SetLastError(error); return false;
        }
        bool Open(std::wstring const& path) noexcept {
            Close();
            m_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_file != INVALID_HANDLE_VALUE && Map(false)) return true;
            const auto error = GetLastError(); Close(); SetLastError(error); return false;
        }
        GpuControl* Get() noexcept { return m_view; }
    };
}
