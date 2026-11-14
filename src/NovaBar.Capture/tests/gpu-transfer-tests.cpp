#include "../CaptureFrameReader.h"
#include "../CaptureFrameWriter.h"
#include "../GpuOwner.h"
#include <cstdio>
#include <thread>
namespace ipc = novabar::capture;
namespace gpu = ipc::gpu;
int failures = 0, checks = 0;
void Check(bool ok, char const* label) { ++checks; printf("%s: %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
ipc::Request MakeRequest() {
    ipc::Request r; r.Pid = GetCurrentProcessId(); r.TargetId = 712; r.Revision = 4;
    r.HeartbeatTick = GetTickCount64(); r.CoordinateHwnd = r.TopLevelHwnd = 1;
    r.Bounds = {-8, 12, 8, 20}; r.OutputWidth = 16; r.OutputHeight = 8;
    r.Flags = ipc::HostClipsCorners | ipc::WorldCanvas | ipc::ClientOptics | ipc::GpuBase;
    return r;
}
gpu::GpuOffer MakeOffer() {
    gpu::GpuOffer o; o.TargetId = 712; o.ClientEpoch = 7; o.Generation = 2; o.RequestRevision = 4;
    o.Client = o.ExpectedBroker = gpu::CurrentIdentity(); o.AdapterLow = 57;
    o.Width = 16; o.Height = 8; o.TextureHandle[0] = 31; o.TextureHandle[1] = 32; o.TextureHandle[2] = 33;
    o.ReadyFenceHandle = 34; o.ConsumedFenceHandle = 35; o.BrokerEpoch = 27; return o;
}
bool Valid(gpu::GpuOffer const& o) { auto good = MakeOffer(); return gpu::ValidOffer(o, good.Client, good.ExpectedBroker, good.BrokerEpoch, {57, 0}, 712, 7); }
gpu::GpuClaimExpectation Expectation(gpu::GpuOffer const& o) {
    return {o.Client, o.ExpectedBroker, o.BrokerEpoch, {o.AdapterLow, o.AdapterHigh},
        o.TargetId, o.ClientEpoch, o.RequestRevision, o.Width, o.Height};
}
bool NoHandles(gpu::GpuOffer const& o) {
    return !o.TextureHandle[0] && !o.TextureHandle[1] && !o.TextureHandle[2] && !o.ReadyFenceHandle && !o.ConsumedFenceHandle;
}
gpu::GpuExpectedFrame MakeFrame(ipc::detail::FrameWriter& writer) {
    auto r = MakeRequest();
    return {r.Revision, 2, writer.ReserveGpuSerial(), 1, 0, 16, 8, r.Bounds, GetTickCount64()};
}
template<class Edit> void EditSlot(ipc::detail::FrameWriter& writer, Edit edit) {
    auto* mapping = writer.ViewForTest(); auto& slot = mapping->Slots[mapping->ActiveSlot];
    InterlockedIncrement64(&slot.Sequence); edit(slot); InterlockedIncrement64(&slot.Sequence);
}
void WireAndAdmission() {
    Check(sizeof(ipc::Request) == 176 && sizeof(ipc::detail::Slot) == 128 && ipc::ProtocolVersion == 2, "CPU protocol, request and slot sizes unchanged");
    Check(offsetof(ipc::detail::Slot, Generation) == 96 && offsetof(ipc::detail::Slot, ReadyFenceValue) == 104
        && offsetof(ipc::detail::Slot, TextureSlot) == 112, "GPU extension occupies exact existing slot padding");
    Check(sizeof(gpu::GpuOwnerRecord) == 64 && sizeof(gpu::GpuOffer) == 192 && sizeof(gpu::GpuLifecycle) == 64
        && sizeof(gpu::GpuConsumedSlot) == 64 && sizeof(gpu::GpuGenerationRecord) == 448 && sizeof(gpu::GpuControl) == 960, "all GPU wire records have exact fixed sizes");
    Check(offsetof(gpu::GpuOffer, TextureHandle) == 104 && offsetof(gpu::GpuOffer, BrokerEpoch) == 144
        && offsetof(gpu::GpuControl, Records) == 64 && offsetof(gpu::GpuGenerationRecord, Consumed) == 256, "handle, nonce and cache-line offsets match wire contract");
    auto r = MakeRequest(); Check(ipc::detail::ValidRequest(r, false), "explicit panel world-canvas optics GPU request is admitted");
    for (auto missing : {ipc::WorldCanvas, ipc::ClientOptics, ipc::HostClipsCorners}) { auto bad = r; bad.Flags &= ~missing; Check(!ipc::detail::ValidRequest(bad, false), "GPU admission requires every panel optics prerequisite"); }
    r.Kind = ipc::Kind::Taskbar; Check(!ipc::detail::ValidRequest(r, false), "taskbar GPU request rejected");
    auto o = MakeOffer(); Check(Valid(o), "complete exact-identity offer admitted");
    for (int field = 0; field < 21; ++field) {
        auto bad = o;
        switch (field) {
        case 0: bad.Magic ^= 1; break; case 1: ++bad.Version; break; case 2: --bad.Bytes; break; case 3: bad.SlotCount = 4; break;
        case 4: ++bad.TargetId; break; case 5: ++bad.ClientEpoch; break; case 6: bad.Generation = 0; break; case 7: bad.RequestRevision = 0; break;
        case 8: ++bad.Client.CreationFileTime; break; case 9: ++bad.ExpectedBroker.CreationFileTime; break; case 10: ++bad.BrokerEpoch; break;
        case 11: ++bad.AdapterLow; break; case 12: bad.Width = 0; break; case 13: bad.Width = 16385; break;
        case 14: bad.Height = 16385; break; case 15: bad.DxgiFormat = 28; break; case 16: bad.MiscFlags = 2; break;
        case 17: bad.ReadyFenceHandle = 0; break; case 18: bad.TextureHandle[1] = bad.TextureHandle[0]; break;
        case 19: bad.ConsumedFenceHandle = UINT64_MAX; break; case 20: bad.Reserved[4] = 1; break;
        }
        Check(!Valid(bad), "malformed offer, foreign identity/nonce, unsupported resource rejected");
    }
}
void Metadata(std::wstring const& dir) {
    auto request = MakeRequest(); ipc::detail::FrameWriter writer; ipc::FrameReader reader; ipc::Frame out;
    Check(ipc::WriteRequest(dir, request) && writer.Ensure(dir, request.Pid, request.TargetId, 4), "GPU publication uses bounded metadata mapping without pixel capacity requirement");
    auto source = MakeFrame(writer); Check(writer.PublishGpu(source), "reserved source serial publishes GPU metadata without pixels");
    auto read = [&](bool optIn = true, uint64_t poll = 0) { return reader.ReadLatest(dir, request.Pid, request.TargetId, request.Revision, out, poll, optIn); };
    Check(!read(false) && FAILED(out.Error), "default reader rejects GPU payload");
    Check(read() && out.Pixels.empty() && out.Stride == 0 && out.Serial == source.SourceSerial
        && out.Generation == source.Generation && out.TextureSlot == 0 && out.PublishedTick == source.PublishedTick, "opted-in reader accepts only GPU metadata and original freshness");
    Check(!read() && out.Status == ipc::Status::Ready && out.Serial == source.SourceSerial, "same serial returns validated metadata without new-frame signal");
    auto renewed = source; renewed.PublishedTick = GetTickCount64();
    Check(writer.RenewGpu(renewed) && !read() && out.PublishedTick == renewed.PublishedTick, "revalidated identical source renews only heartbeat, preserves serial");
    for (int field = 0; field < 9; ++field) {
        auto changed = renewed;
        switch (field) { case 0: ++changed.Generation; break; case 1: ++changed.SourceSerial; break; case 2: ++changed.ReadyFenceValue; break;
        case 3: ++changed.TextureSlot; break; case 4: ++changed.Revision; break; case 5: ++changed.Width; break;
        case 6: ++changed.Height; break; case 7: ++changed.BitmapBounds.Left; break; case 8: changed.PublishedTick = UINT64_MAX; break; }
        Check(!writer.RenewGpu(changed), "renewal cannot change source identity, fence, geometry or invent freshness");
    }
    auto reservation = MakeFrame(writer); auto newer = MakeFrame(writer);
    Check(!writer.PublishGpu(reservation) && writer.PublishGpu(newer), "new reservation invalidates earlier source serial");
    Check(read() && out.Serial == newer.SourceSerial, "new GPU metadata becomes visible with reserved serial");
    Check(!writer.PublishGpu(newer), "GPU serial is single-use publication token");
    auto pending = MakeFrame(writer); Check(!writer.RenewGpu(newer), "pending source reservation blocks old-source heartbeat renewal");
    Check(writer.Publish(request.Revision, ipc::Status::Unavailable, E_ACCESSDENIED, 0, 0, {})
        && !writer.PublishGpu(pending), "CPU status publication invalidates pending GPU reservation");
    Check(!read() && out.Status == ipc::Status::Unavailable && out.Error == E_ACCESSDENIED && out.Generation == 0, "per-read unavailable status carries actual error and clears GPU metadata");
    for (int field = 0; field < 16; ++field) {
        auto good = MakeFrame(writer); Check(writer.PublishGpu(good), "restore genuine GPU metadata before corruption case");
        EditSlot(writer, [&](auto& slot) {
            switch (field) {
            case 0: slot.ReadyFenceValue = 0; break; case 1: slot.ReadyFenceValue = UINT64_MAX; break;
            case 2: slot.TextureSlot = 3; break; case 3: slot.Generation = 0; break; case 4: slot.Stride = 64; break;
            case 5: slot.Width = 15; break; case 6: ++slot.BitmapBounds.Left; break; case 7: slot.PayloadFlags = ipc::GpuBasePayload; break;
            case 8: slot.Error = E_FAIL; break; case 9: slot.GpuReserved0 = 1; break; case 10: slot.GpuReserved1 = 1; break;
            case 11: slot.Serial = 0; break; case 12: slot.Status = static_cast<ipc::Status>(99); break;
            case 13: slot.PublishedTick = GetTickCount64() - 1501; break;
            case 14: ++slot.Revision; break; case 15: slot.MotionSequence = 1; break;
            }
        });
        Check(!read(true, 100), "every read rejects corrupted GPU metadata even with metadata cache");
    }
    auto good = MakeFrame(writer); writer.PublishGpu(good); auto* mapping = writer.ViewForTest();
    auto& active = mapping->Slots[mapping->ActiveSlot];
    InterlockedIncrement64(&active.Sequence);
    Check(!read(), "odd publication seqlock never yields GPU metadata");
    InterlockedIncrement64(&active.Sequence);
    request.Flags &= ~ipc::GpuBase; ipc::WriteRequest(dir, request);
    Check(!read(), "reader opt-in alone cannot accept GPU payload without negotiated request");
    request.Flags = 0; request.OutputWidth = request.OutputHeight = 0; ipc::WriteRequest(dir, request);
    std::vector<uint8_t> pixels(16 * 8 * 4, 61);
    Check(writer.Ensure(dir, request.Pid, request.TargetId, pixels.size()) && writer.Publish(request.Revision, ipc::Status::Ready, S_OK, 16, 8, pixels)
        && read(false) && out.Pixels == pixels && out.Stride == 64 && out.Generation == 0, "unchanged CPU default copies original bytes and clears extension");
    Check(!read(false) && out.Pixels.empty() && out.Status == ipc::Status::Ready, "unchanged CPU serial avoids redundant bytes");
    EditSlot(writer, [&](auto& slot) { slot.Status = ipc::Status::Unavailable; slot.Width = slot.Height = slot.Stride = 0; slot.Error = E_ABORT; });
    Check(!read(false, 100) && out.Status == ipc::Status::Unavailable && out.Error == E_ABORT, "cached CPU metadata still observes current per-read source error");
    reader.Close(); writer.Close(); ipc::CancelRequest(dir, request.Pid, request.TargetId);
}
void ClaimSnapshot() {
    const auto original = MakeOffer();
    auto expected = Expectation(original);
    HANDLE client = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, original.Client.Pid);
    gpu::GpuControl control{}; gpu::InitializeControl(control, original.Client, original.TargetId, original.ClientEpoch);
    auto& record = control.Records[0];
    Check(gpu::PublishOffer(record, original), "preclaim replacement fixture publishes original offer A");
    const auto prevalidated = record.Offer;
    Check(Valid(prevalidated), "broker prevalidates original offer A before client cancellation");
    gpu::RequestCancel(record);
    Check(gpu::ResetTerminal(record, true), "client disposes cancelled preclaim generation before broker wins CAS");
    auto replacement = original; ++replacement.Generation; ++replacement.BrokerEpoch;
    replacement.TextureHandle[0] += 10;
    Check(gpu::PublishOffer(record, replacement), "client reoffers B with a new nonce and handle before broker claims");
    gpu::GpuOffer snapshot = prevalidated;
    int pullCount = 0;
    auto claim = gpu::TryClaimSnapshot(control, 0, client, expected, snapshot);
    if (claim == gpu::GpuClaimResult::Claimed) ++pullCount;
    Check(claim == gpu::GpuClaimResult::Rejected && gpu::Phase(record) == gpu::GpuPhase::Rejected
        && FAILED(record.Life.Result), "postclaim mismatched replacement receives terminal rejection");
    Check(pullCount == 0 && NoHandles(snapshot) && !snapshot.Generation && !snapshot.BrokerEpoch,
        "rejected replacement exposes no old or new handles and invokes no broker pull");
    gpu::ResetTerminal(record, true);
    ++replacement.RequestRevision; replacement.Width += 8;
    expected = Expectation(replacement);
    Check(gpu::PublishOffer(record, replacement), "valid replacement publishes current nonce revision extent and handles");
    claim = gpu::TryClaimSnapshot(control, 0, client, expected, snapshot);
    if (claim == gpu::GpuClaimResult::Claimed) ++pullCount;
    Check(claim == gpu::GpuClaimResult::Claimed && gpu::Phase(record) == gpu::GpuPhase::Claiming && !gpu::CanDispose(record, true)
        && snapshot.TextureHandle[0] == replacement.TextureHandle[0] && snapshot.TextureHandle[0] != prevalidated.TextureHandle[0]
        && snapshot.BrokerEpoch == replacement.BrokerEpoch && snapshot.RequestRevision == replacement.RequestRevision
        && snapshot.Width == replacement.Width && pullCount == 1, "matching replacement yields only its actual pinned postclaim snapshot");
    Check(gpu::CompleteImport(record, S_OK) && gpu::Phase(record) == gpu::GpuPhase::Imported,
        "matching replacement follows the ordinary successful import acknowledgement");
    gpu::RequestCancel(record); gpu::CompleteRetirement(record); gpu::ResetTerminal(record, true);
    for (int field = 0; field < 16; ++field) {
        gpu::InitializeControl(control, original.Client, original.TargetId, original.ClientEpoch);
        gpu::PublishOffer(record, original);
        auto mismatch = Expectation(original);
        switch (field) {
        case 0: ++mismatch.Client.Pid; break; case 1: ++mismatch.Client.CreationFileTime; break;
        case 2: ++mismatch.Broker.Pid; break; case 3: ++mismatch.Broker.CreationFileTime; break;
        case 4: ++mismatch.BrokerNonce; break; case 5: ++mismatch.Adapter.LowPart; break;
        case 6: ++mismatch.TargetId; break; case 7: ++mismatch.ClientEpoch; break;
        case 8: ++mismatch.RequestRevision; break; case 9: ++mismatch.Width; break;
        case 10: ++mismatch.Height; break; case 11: ++control.Header.Client.CreationFileTime; break;
        case 12: ++control.Header.TargetId; break; case 13: ++control.Header.ClientEpoch; break;
        case 14: ++control.Header.Client.Pid; break; case 15: ++mismatch.Adapter.HighPart; break;
        }
        snapshot = original;
        claim = gpu::TryClaimSnapshot(control, 0, client, mismatch, snapshot);
        if (claim == gpu::GpuClaimResult::Claimed) ++pullCount;
        Check(claim == gpu::GpuClaimResult::Rejected && gpu::Phase(record) == gpu::GpuPhase::Rejected
            && NoHandles(snapshot), "postclaim process owner adapter request extent and control identity must all match");
    }
    Check(pullCount == 1, "none of the rejected postclaim mismatches invoke handle pull");
    gpu::InitializeControl(control, original.Client, original.TargetId, original.ClientEpoch);
    gpu::PublishOffer(record, original); snapshot = original;
    Check(gpu::TryClaimSnapshot(control, 2, client, Expectation(original), snapshot) == gpu::GpuClaimResult::NotClaimed
        && gpu::Phase(record) == gpu::GpuPhase::Offered && NoHandles(snapshot), "invalid record index leaves lifecycle untouched and clears previous snapshot");
    snapshot = original;
    Check(gpu::TryClaimSnapshot(control, 0, nullptr, Expectation(original), snapshot) == gpu::GpuClaimResult::Rejected
        && gpu::Phase(record) == gpu::GpuPhase::Rejected && NoHandles(snapshot), "missing verified process handle rejects claim without exposing offer handles");
    gpu::InitializeControl(control, original.Client, original.TargetId, original.ClientEpoch);
    gpu::PublishOffer(record, original); snapshot = original;
    HANDLE queryOnly = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, original.Client.Pid);
    Check(queryOnly && gpu::TryClaimSnapshot(control, 0, queryOnly, Expectation(original), snapshot) == gpu::GpuClaimResult::Rejected
        && gpu::Phase(record) == gpu::GpuPhase::Rejected && NoHandles(snapshot), "client liveness must be proven with SYNCHRONIZE rights before claim success");
    if (queryOnly) CloseHandle(queryOnly);
    if (client) CloseHandle(client);
}
void Ownership() {
    const auto offer = MakeOffer();
    gpu::GpuGenerationRecord idle{}; gpu::RequestCancel(idle);
    Check(gpu::PublishOffer(idle, offer) && gpu::TryClaim(idle), "retiring an empty record cannot poison a later offered generation");
    HANDLE verifiedClient = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    Check(verifiedClient && gpu::SameIdentity(gpu::Identity(verifiedClient, GetCurrentProcessId()), offer.Client), "normal broker opens exact client identity for local handle pulls");
    std::array<HANDLE, 5> sourceHandles{};
    for (auto& handle : sourceHandles) handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int cancelAt = 0; cancelAt < 8; ++cancelAt) {
        gpu::GpuGenerationRecord record{}; Check(gpu::PublishOffer(record, offer), "client publishes immutable offer from Empty only");
        if (cancelAt == 0) {
            gpu::RequestCancel(record);
            Check(!gpu::TryClaim(record) && gpu::CanDispose(record, true), "cancel wins Offered CAS; no future handle duplication");
        } else {
            Check(gpu::TryClaim(record), "broker atomically claims immutable offer");
            if (cancelAt <= 6) {
                // Cancellation after claim and after each of five pull/import steps.
                std::array<gpu::LocalHandle, 5> pulled;
                bool pulledOk = true;
                for (int i = 0; i < cancelAt - 1; ++i)
                    pulledOk &= pulled[i].Pull(verifiedClient, reinterpret_cast<uint64_t>(sourceHandles[i]));
                Check(pulledOk, "handle pull prefix succeeds before cancellation at that transition");
                gpu::RequestCancel(record);
                Check(!gpu::CanDispose(record, true) && !gpu::ResetTerminal(record, true), "Claiming pins original handles despite cancellation or timeout");
                for (auto& handle : pulled) handle.Reset();
                Check(gpu::CompleteImport(record, S_OK) && gpu::Phase(record) == gpu::GpuPhase::Rejected && record.Life.Result == E_ABORT,
                    "broker finishes cancelled import with terminal failure acknowledgement");
            } else {
                Check(gpu::CompleteImport(record, S_OK), "successful import is acknowledged");
                gpu::RequestCancel(record);
                Check(!gpu::CanDispose(record, true) && gpu::CompleteRetirement(record), "imported cancellation waits for broker write completion and retirement");
            }
        }
        Check(!gpu::CanDispose(record, false) && gpu::CanDispose(record, true), "terminal ack still requires completion of client GPU reads");
        Check(!gpu::PublishOffer(record, offer) && gpu::ResetTerminal(record, true) && gpu::Phase(record) == gpu::GpuPhase::Empty,
            "only disposed terminal records may be reset and offered again");
    }
    for (auto handle : sourceHandles) CloseHandle(handle);
    for (int failAt = 0; failAt < 5; ++failAt) {
        std::array<HANDLE, 5> originals{};
        for (auto& handle : originals) handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD before = 0; GetProcessHandleCount(GetCurrentProcess(), &before);
        {
            gpu::GpuGenerationRecord record{}; gpu::PublishOffer(record, offer); gpu::TryClaim(record);
            std::array<gpu::LocalHandle, 5> duplicates;
            bool success = true;
            for (int i = 0; i < 5; ++i) {
                if (!duplicates[i].Pull(verifiedClient, i == failAt ? 0x12345678ull : reinterpret_cast<uint64_t>(originals[i]))) { success = false; break; }
            }
            Check(!success && gpu::CompleteImport(record, E_HANDLE) && record.Life.Result == E_HANDLE && gpu::CanDispose(record, true), "each duplicate failure rejects import and acknowledges disposal");
        }
        DWORD after = 0; GetProcessHandleCount(GetCurrentProcess(), &after);
        Check(before == after, "all successful broker-local duplicates close on partial failure");
        bool intact = true;
        for (auto handle : originals) { DWORD flags = 0; intact &= !!GetHandleInformation(handle, &flags); CloseHandle(handle); }
        Check(intact, "broker cleanup never closes client original numeric handles");
    }
    gpu::GpuControl control{}; gpu::InitializeControl(control, offer.Client, offer.TargetId, offer.ClientEpoch);
    Check(gpu::ValidControl(control) && gpu::FindEmptyRecord(control) == 0, "bounded control starts with two empty records");
    gpu::PublishOffer(control.Records[0], offer); gpu::TryClaim(control.Records[0]); gpu::CompleteImport(control.Records[0], S_OK);
    Check(gpu::ActivateRecord(control, 0) && gpu::FindEmptyRecord(control) == 1, "single imported generation becomes active");
    gpu::RequestCancel(control.Records[0]); auto next = offer; ++next.Generation; ++next.RequestRevision;
    gpu::PublishOffer(control.Records[1], next); gpu::TryClaim(control.Records[1]);
    Check(gpu::FindEmptyRecord(control) == -1 && !gpu::PublishOffer(control.Records[0], next), "active plus pinned retiring generation exhausts bounded ownership without allocation");
    Check(!gpu::ValidControl(control, sizeof(control) - 1) && !gpu::ValidControl(control, 4097), "control file bounds checked before mapped access");
    control.Header.Bytes = 959; Check(!gpu::ValidControl(control), "wrong complete control byte size rejected");
    gpu::GpuConsumedSlot ack{};
    Check(!gpu::CanReuseSlot(ack, 2, 41, 100), "slot without consumption ack remains pinned");
    Check(gpu::PublishConsumed(ack, {2, 41, 7}) && !gpu::CanReuseSlot(ack, 2, 41, 6) && gpu::CanReuseSlot(ack, 2, 41, 7), "slot reuse waits for exact queued consumed fence to complete");
    Check(!gpu::PublishConsumed(ack, {2, 42, 6}) && gpu::CanReuseSlot(ack, 2, 41, 7), "consumed-fence values cannot regress inside generation");
    Check(!gpu::PublishConsumed(ack, {3, 42, 8}), "acknowledgement generation cannot change before terminal record reset");
    Check(!gpu::CanReuseSlot(ack, 2, 42, 100) && !gpu::CanReuseSlot(ack, 3, 41, 100), "old serial and old generation acknowledgements cannot release a new publication");
    Check(!gpu::CanReuseSlot(ack, 2, 41, UINT64_MAX), "removed-device fence sentinel never releases shared input");
    InterlockedIncrement64(&ack.Sequence);
    Check(!gpu::CanReuseSlot(ack, 2, 41, 100) && !gpu::PublishConsumed(ack, {2, 42, 8}), "torn acknowledgement cannot release input or admit a concurrent writer");
    InterlockedIncrement64(&ack.Sequence);
    Check(!gpu::PublishConsumed(ack, {2, 41, UINT64_MAX}), "invalid consumed fence signal is never acknowledged");
    gpu::GpuGenerationRecord record{}; gpu::PublishOffer(record, offer); gpu::TryClaim(record); gpu::RequestCancel(record);
    Check(gpu::ProbeExactProcess(offer.ExpectedBroker) == gpu::ExactProcessState::Alive && !gpu::TryAbandonDeadBroker(record), "live exact broker cannot be abandoned after timeout/cancellation");
    auto reused = offer; ++reused.ExpectedBroker.CreationFileTime;
    gpu::GpuGenerationRecord reusedRecord{}; gpu::PublishOffer(reusedRecord, reused); gpu::TryClaim(reusedRecord);
    Check(gpu::TryAbandonDeadBroker(reusedRecord) && gpu::CanDispose(reusedRecord, true), "PID reuse proves exact old broker dead without confusing replacement process");
    Check(gpu::ProbeExactProcess({}) == gpu::ExactProcessState::Unknown, "unverified process identity never proves broker death");
    gpu::GpuGenerationRecord malformed{}; gpu::PublishOffer(malformed, offer); malformed.Life.Reserved0 = 1;
    Check(!gpu::TryClaim(malformed), "nonzero lifecycle reserved status rejected before claiming");
    wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --exit-child";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    const bool launched = !!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    Check(launched, "launch isolated no-window broker-death identity fixture");
    if (launched) {
        const auto childIdentity = gpu::Identity(child.hProcess, child.dwProcessId);
        WaitForSingleObject(child.hProcess, 5000);
        auto deadOffer = offer; deadOffer.ExpectedBroker = childIdentity;
        gpu::GpuGenerationRecord deadRecord{}; gpu::PublishOffer(deadRecord, deadOffer); gpu::TryClaim(deadRecord);
        Check(gpu::TryAbandonDeadBroker(deadRecord) && gpu::CanDispose(deadRecord, true), "verified exact terminated broker releases Claiming ownership without timeout assumptions");
        auto deadClientOffer = offer; deadClientOffer.Client = childIdentity;
        gpu::GpuControl deadClient{}; gpu::InitializeControl(deadClient, childIdentity, offer.TargetId, offer.ClientEpoch);
        gpu::PublishOffer(deadClient.Records[0], deadClientOffer);
        gpu::GpuOffer snapshot = offer;
        Check(gpu::TryClaimSnapshot(deadClient, 0, child.hProcess, Expectation(deadClientOffer), snapshot) == gpu::GpuClaimResult::Rejected
            && gpu::Phase(deadClient.Records[0]) == gpu::GpuPhase::Rejected && NoHandles(snapshot),
            "exited verified client process cannot yield claim success from matching retained creation time");
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    bool racesSafe = true;
    for (int attempt = 0; attempt < 128; ++attempt) {
        gpu::GpuGenerationRecord race{}; gpu::PublishOffer(race, offer);
        std::thread cancellation([&] { gpu::RequestCancel(race); });
        const bool claimed = gpu::TryClaim(race); cancellation.join();
        if (claimed) racesSafe &= gpu::Phase(race) == gpu::GpuPhase::Claiming && !gpu::CanDispose(race, true)
            && gpu::CompleteImport(race, S_OK) && gpu::Phase(race) == gpu::GpuPhase::Rejected;
        else racesSafe &= gpu::Phase(race) == gpu::GpuPhase::Retired;
    }
    Check(racesSafe, "128 concurrent cancellation/claim races preserve exactly one CAS winner and pinned originals");
    if (verifiedClient) CloseHandle(verifiedClient);
}
void OwnerAndMapping(std::wstring const& dir) {
    const auto self = gpu::CurrentIdentity(); const auto nonce = gpu::RandomNonce();
    Check(gpu::ValidIdentity(self) && nonce, "owner uses real process creation identity and random nonce");
    gpu::GpuOwnerWriter writer; gpu::GpuOwnerRecord owner{};
    Check(writer.Publish(dir, self, nonce, {57, 0}) && gpu::TryReadFreshGpuOwner(dir, GetTickCount64(), owner), "owner is atomically published and read without reverse process query");
    Check(gpu::ValidOwner(owner, owner.HeartbeatTick + 1500) && !gpu::ValidOwner(owner, owner.HeartbeatTick + 1501)
        && !gpu::ValidOwner(owner, owner.HeartbeatTick - 1), "owner heartbeat obeys exact existing 1500ms TTL and rejects future stamps");
    for (int field = 0; field < 11; ++field) {
        auto bad = owner;
        switch (field) { case 0: ++bad.Magic; break; case 1: ++bad.Version; break; case 2: ++bad.Bytes; break; case 3: bad.Pid = 0; break;
        case 4: bad.CreationFileTime = 0; break; case 5: bad.Nonce = 0; break; case 6: bad.AdapterLow = bad.AdapterHigh = 0; break;
        case 7: ++bad.GpuProtocolVersion; break; case 8: bad.Flags = 1; break; case 9: bad.Reserved = 1; break; case 10: bad.HeartbeatTick = 0; break; }
        Check(!gpu::ValidOwner(bad, owner.HeartbeatTick), "malformed owner record rejected before offer creation");
    }
    gpu::GpuOwnerWriter second;
    Check(!second.Publish(dir, self, nonce + 1, {57, 0}), "second live writer cannot steal existing broker owner");
    Check(writer.Refresh(GetTickCount64()) && gpu::TryReadFreshGpuOwner(dir, GetTickCount64(), owner) && owner.Nonce == nonce, "owner renewal preserves PID birth nonce and adapter");
    auto replacement = owner; ++replacement.Nonce;
    Check(ipc::detail::WriteSmall(gpu::OwnerPath(dir), &replacement, sizeof(replacement)) && !writer.Refresh(GetTickCount64()), "stale writer cannot renew replacement identity");
    writer.Withdraw();
    Check(gpu::TryReadFreshGpuOwner(dir, GetTickCount64(), owner) && owner.Nonce == replacement.Nonce, "stale withdrawal preserves replacement owner file");
    HANDLE opened = CreateFileW(gpu::OwnerPath(dir).c_str(), DELETE | GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto newest = replacement; ++newest.Nonce;
    // MoveFileEx replacement of an open destination is denied on some hosts.
    // Rename that exact opened object aside, then install the replacement path.
    const auto heldPath = (std::filesystem::path(dir) / L"held-owner.bin").wstring();
    const bool replacedWhileOpened = opened != INVALID_HANDLE_VALUE
        && MoveFileExW(gpu::OwnerPath(dir).c_str(), heldPath.c_str(), 0)
        && ipc::detail::WriteSmall(gpu::OwnerPath(dir), &newest, sizeof(newest));
    if (!replacedWhileOpened) printf("INFO: opened=%p replacement-error=%lu\n", opened, GetLastError());
    Check(replacedWhileOpened, "open old owner file then atomically replace pathname");
    if (opened != INVALID_HANDLE_VALUE) {
        Check(gpu::owner_detail::DeleteOpenedOwner(opened, replacement), "cleanup disposes the exact renamed opened owner file");
        CloseHandle(opened);
        Check(!std::filesystem::exists(heldPath), "disposed old owner file leaves no held-path residue");
    }
    Check(gpu::TryReadFreshGpuOwner(dir, GetTickCount64(), owner) && owner.Nonce == newest.Nonce, "opened-file disposition cannot delete path replacement during cleanup race");
    DeleteFileW(gpu::OwnerPath(dir).c_str());
    Check(writer.Publish(dir, self, nonce, {57, 0}), "withdrawn writer can publish again after ownership released"); writer.Withdraw();
    Check(!std::filesystem::exists(gpu::OwnerPath(dir)), "current owner withdrawal deletes its own opened file");
    auto mappingPath = (std::filesystem::path(dir) / L"control.bin").wstring();
    gpu::GpuControlMapping client, broker;
    Check(client.Create(mappingPath, self, 712, 7) && broker.Open(mappingPath), "client control file is read-write mapped through unnamed mapping by broker");
    auto offer = MakeOffer();
    Check(client.Get() && broker.Get() && gpu::PublishOffer(client.Get()->Records[0], offer) && gpu::TryClaim(broker.Get()->Records[0])
        && gpu::Phase(client.Get()->Records[0]) == gpu::GpuPhase::Claiming, "cross-view Interlocked ownership transitions are visible");
    broker.Close(); client.Close(); Check(!std::filesystem::exists(mappingPath), "control file closes after bounded ownership owner retires");
    const uint64_t malformed = 0;
    ipc::detail::WriteSmall(mappingPath, &malformed, sizeof(malformed));
    Check(!broker.Open(mappingPath), "truncated control rejected before mapped fields accessed"); DeleteFileW(mappingPath.c_str());
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--exit-child") return 0;
    const auto dir = (std::filesystem::current_path() / L"artifacts" / L"capture-tests" /
        (L"gpu-transfer-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()))).wstring();
    std::filesystem::create_directories(dir);
    WireAndAdmission(); Metadata(dir); ClaimSnapshot(); Ownership(); OwnerAndMapping(dir);
    std::filesystem::remove_all(dir);
    printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
