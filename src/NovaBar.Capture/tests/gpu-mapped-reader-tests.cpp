#include "../CaptureFrameReader.h"
#include "../CaptureFrameWriter.h"
#include <cstdio>

namespace ipc = novabar::capture;
namespace
{
    int checks = 0, failures = 0;
    void Check(bool ok, char const* label)
    {
        ++checks;
        printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    }

    struct Fixture
    {
        std::wstring Directory;
        ipc::Request Request;
        ipc::detail::FrameWriter Writer;
        ipc::FrameReader Reader;
        ipc::Frame Out;

        Fixture(std::filesystem::path const& directory, uint64_t id)
            : Directory(directory.wstring())
        {
            Request.Pid = GetCurrentProcessId(); Request.TargetId = id; Request.Revision = 4;
            Request.HeartbeatTick = GetTickCount64();
            Request.TopLevelHwnd = Request.CoordinateHwnd = 1;
            Request.Bounds = {-4, 16, 12, 24};
            Request.OutputWidth = 16; Request.OutputHeight = 8;
            Request.Flags = ipc::HostClipsCorners | ipc::WorldCanvas | ipc::ClientOptics | ipc::GpuBase;
        }
        bool Open() { return ipc::WriteRequest(Directory, Request) && Writer.Ensure(Directory, Request.Pid, Request.TargetId, 4); }
        ipc::gpu::GpuExpectedFrame Publish()
        {
            const auto serial = Writer.ReserveGpuSerial();
            ipc::gpu::GpuExpectedFrame frame{4, 3, serial, serial + 10, 1, 16, 8, {-4, 16, 12, 24}, GetTickCount64()};
            Check(Writer.PublishGpu(frame), "fixture publishes GPU metadata");
            return frame;
        }
        bool Seed(bool acceptGpu = true)
        { return Reader.ReadLatest(Directory, Request.Pid, Request.TargetId, Request.Revision, Out, 100, acceptGpu); }
        bool Mapped(uint64_t now = GetTickCount64())
        { return Reader.ReadMappedGpu(Request.Pid, Request.TargetId, Request.Revision, Out, now); }
        bool Pending() const
        { return Out.Status == ipc::Status::Waiting && Out.Error == E_PENDING && !Out.Serial && Out.Pixels.empty(); }
        bool Invalid() const
        {
            return Out.Status == ipc::Status::Unavailable && Out.Error == HRESULT_FROM_WIN32(ERROR_INVALID_DATA)
                && !Out.Serial && Out.Pixels.empty();
        }
        template<class Edit> void EditSlot(Edit edit)
        {
            auto& slot = ActiveSlot();
            InterlockedIncrement64(&slot.Sequence);
            edit(slot);
            InterlockedIncrement64(&slot.Sequence);
        }
        ipc::detail::Slot& ActiveSlot()
        {
            auto* view = Writer.ViewForTest();
            return view->Slots[view->ActiveSlot];
        }
    };

    void LockedMetadata(Fixture& f)
    {
        const auto first = f.Publish();
        Check(!f.Mapped() && f.Pending(), "unseeded render read never opens publication files");
        Check(!f.Seed(false) && f.Invalid(), "normal reader still rejects GPU without explicit opt-in");
        Check(!f.Mapped() && f.Pending(), "failed default read cannot seed mapped GPU");
        Check(f.Seed() && f.Out.Serial == first.SourceSerial, "normal opt-in validates and seeds current mapping");
        const auto lock = [&](wchar_t const* suffix)
        {
            const auto path = ipc::detail::Path(f.Directory, f.Request.Pid, f.Request.TargetId, suffix);
            return CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        };
        const HANDLE requestLock = lock(L".request"), responseLock = lock(L".response");
        Check(requestLock != INVALID_HANDLE_VALUE && responseLock != INVALID_HANDLE_VALUE,
            "exclusive locks prevent ordinary metadata opens");
        ipc::FrameReader strict;
        ipc::Frame out;
        Check(!strict.ReadLatest(f.Directory, f.Request.Pid, f.Request.TargetId, 4, out, 0, true)
            && out.Error == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "strict file-backed read is actually blocked");

        auto fresh = f.Publish();
        Check(f.Mapped() && f.Out.Serial == fresh.SourceSerial && f.Out.Generation == 3 && f.Out.TextureSlot == 1
            && f.Out.ReadyFenceValue == fresh.ReadyFenceValue && f.Out.Width == 16 && f.Out.Height == 8
            && f.Out.BitmapBounds.Left == -4 && f.Out.BitmapBounds.Bottom == 24
            && f.Out.PayloadFlags == 3 && !f.Out.Stride && f.Out.Pixels.empty(),
            "mapped-only read sees the new exact GPU tuple under both metadata locks");
        auto& current = f.ActiveSlot();
        f.EditSlot([&](auto& slot) { slot.PublishedTick -= 1; });
        Check(!f.Mapped() && f.Out.Status == ipc::Status::Ready && f.Out.PublishedTick == fresh.PublishedTick - 1,
            "same serial returns current metadata without another new-frame signal");
        Check(f.Writer.RenewGpu(fresh) && !f.Mapped() && f.Out.Serial == fresh.SourceSerial
            && f.Out.PublishedTick == fresh.PublishedTick && f.Out.Pixels.empty(),
            "same-serial broker renewal is visible through the map only");
        InterlockedIncrement64(&current.Sequence);
        Check(!f.Mapped() && f.Pending(), "odd publication sequence is transient and exposes no data");
        InterlockedIncrement64(&current.Sequence);
        Check(!f.Mapped() && f.Out.Status == ipc::Status::Ready, "sequence recovery needs no file reopen");
        if (requestLock != INVALID_HANDLE_VALUE) CloseHandle(requestLock);
        if (responseLock != INVALID_HANDLE_VALUE) CloseHandle(responseLock);
    }

    void ValidateMapping(Fixture& f)
    {
        const auto pending = [&](uint32_t pid, uint64_t id, uint64_t revision)
        {
            return !f.Reader.ReadMappedGpu(pid, id, revision, f.Out) && f.Pending() && f.Out.Revision == revision;
        };
        Check(pending(f.Request.Pid + 1, f.Request.TargetId, 4), "different PID cannot read a seeded map");
        Check(pending(f.Request.Pid, f.Request.TargetId + 1, 4), "different target cannot read a seeded map");
        Check(pending(f.Request.Pid, f.Request.TargetId, 5), "different revision cannot read a seeded map");
        Check(!f.Mapped(GetTickCount64() + 100) && f.Pending(), "render polling cannot extend the metadata cache deadline");
        auto saved = f.Publish();
        Check(f.Seed() && f.Out.Serial == saved.SourceSerial, "normal source poll sees a new serial after mapped reads");
        const auto now = GetTickCount64();
        f.EditSlot([&](auto& slot) { slot.Revision = 3; });
        Check(!f.Mapped(now) && f.Pending(), "frame from another revision waits for a fresh normal poll");
        f.EditSlot([&](auto& slot) { slot.Revision = 4; slot.PublishedTick = now + 1; });
        Check(!f.Mapped(now) && f.Out.Status == ipc::Status::Waiting && f.Out.Error == HRESULT_FROM_WIN32(WAIT_TIMEOUT),
            "future publication is never presented");
        f.EditSlot([&](auto& slot) { slot.PublishedTick = now - 1501; });
        Check(!f.Mapped(now) && f.Out.Status == ipc::Status::Waiting && f.Out.Error == HRESULT_FROM_WIN32(WAIT_TIMEOUT),
            "stale publication never refreshes the displayed source");
        f.EditSlot([&](auto& slot) { slot.PublishedTick = saved.PublishedTick; });
        auto* mapping = f.Writer.ViewForTest();
        const LONG active = mapping->ActiveSlot;
        InterlockedExchange(&mapping->ActiveSlot, 2);
        Check(!f.Mapped(now) && f.Invalid(), "invalid active slot is not indexed");
        InterlockedExchange(&mapping->ActiveSlot, active);
        ++mapping->CapacityBytes;
        Check(!f.Mapped(now) && f.Invalid(), "capacity change is rejected before reading slots");
        --mapping->CapacityBytes;
        ++mapping->Reserved;
        Check(!f.Mapped(now) && f.Invalid(), "nonzero GPU mapping reserved field is rejected");
        --mapping->Reserved;
        ++mapping->Version;
        Check(!f.Mapped(now) && f.Invalid(), "invalid mapping version is rejected");
        --mapping->Version;

        const auto corrupt = [&]<typename Field>(Field ipc::detail::Slot::*member, Field bad, char const* label)
        {
            const Field good = f.ActiveSlot().*member;
            f.EditSlot([&](auto& slot) { slot.*member = bad; });
            Check(!f.Mapped(now) && f.Invalid(), label);
            f.EditSlot([&](auto& slot) { slot.*member = good; });
        };
        corrupt(&ipc::detail::Slot::Width, 17u, "wrong output extent is rejected");
        corrupt(&ipc::detail::Slot::BitmapBounds, novabar::PixelRect{-3, 16, 13, 24}, "equal-size wrong canvas position is rejected");
        corrupt(&ipc::detail::Slot::Stride, 64u, "GPU payload cannot carry a CPU stride");
        corrupt(&ipc::detail::Slot::MotionSequence, uint64_t{1}, "GPU payload cannot carry CPU motion");
        corrupt(&ipc::detail::Slot::RenderedBounds, novabar::PixelRect{1, 0, 0, 0}, "GPU payload cannot carry CPU rendered bounds");
        corrupt(&ipc::detail::Slot::GpuReserved0, 1u, "first reserved slot field must be zero");
        corrupt(&ipc::detail::Slot::GpuReserved1, uint64_t{1}, "second reserved slot field must be zero");
        corrupt(&ipc::detail::Slot::Generation, uint64_t{0}, "GPU generation must be specified");
        corrupt(&ipc::detail::Slot::Serial, uint64_t{0}, "source serial must be specified");
        corrupt(&ipc::detail::Slot::ReadyFenceValue, UINT64_MAX, "invalid GPU fence marker is rejected");
        corrupt(&ipc::detail::Slot::TextureSlot, 3u, "out-of-range GPU texture slot is rejected");
        corrupt(&ipc::detail::Slot::Error, E_INVALIDARG, "GPU Ready cannot carry a failed HRESULT");
        corrupt(&ipc::detail::Slot::Status, static_cast<ipc::Status>(99), "unknown source status is rejected");

        for (auto [status, error] : {std::pair{ipc::Status::Waiting, E_PENDING},
            std::pair{ipc::Status::Unavailable, E_ABORT}, std::pair{ipc::Status::InvalidRequest, E_INVALIDARG}})
        {
            const bool posted = f.Writer.Publish(4, status, error, 0, 0, {});
            Check(posted && !f.Mapped() && f.Out.Status == status && f.Out.Error == error
                && f.Out.Serial && f.Out.PublishedTick && f.Out.Pixels.empty(), "real producer status and error propagate without new frames");
            f.EditSlot([](auto& slot) { slot.Generation = 1; });
            Check(!f.Mapped() && f.Invalid(), "nonready GPU status may not carry texture identity");
        }
        const std::vector<uint8_t> pixels(16 * 8 * 4, 77);
        Check(f.Writer.Publish(4, ipc::Status::Ready, S_OK, 16, 8, pixels, true, {}, 0,
            {-4, 16, 12, 24}, ipc::BaseCanvasPayload) && !f.Mapped() && f.Invalid(),
            "a genuine CPU publication never enters mapped GPU consumption");
        f.Reader.Close();
        Check(!f.Mapped() && f.Pending(), "Close revokes the validated mapped context");
        f.Request.Flags &= ~ipc::GpuBase;
        Check(ipc::WriteRequest(f.Directory, f.Request) && f.Seed() && f.Out.Pixels == pixels,
            "ordinary read retains CPU base payload behavior");
        Check(!f.Mapped() && f.Pending(), "an opted-in reader cannot seed fast path with a non-GPU request");
    }

    void HeartbeatAndDirectory(Fixture& f)
    {
        const auto now = GetTickCount64();
        f.Request.HeartbeatTick = now - 1450;
        const bool installed = ipc::WriteRequest(f.Directory, f.Request);
        f.Publish();
        Check(installed && f.Seed(), "normal read admits original request near its heartbeat expiry");
        Check(!f.Mapped(now + 51) && f.Pending(), "expired request heartbeat stops render polling before cache expiry");
        const auto other = (std::filesystem::path(f.Directory) / L"not-the-mapped-directory").wstring();
        Check(!f.Reader.ReadLatest(other, f.Request.Pid, f.Request.TargetId, 4, f.Out, 100, true)
            && !f.Mapped() && f.Pending(), "switching source directory invalidates the old mapping context");
    }
}

int main()
{
    const auto dir = std::filesystem::current_path() / L"artifacts" / L"capture-tests" /
        (L"gpu-mapped-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directories(dir)) return 1;
    {
        Fixture f(dir, 712);
        if (f.Open()) { LockedMetadata(f); ValidateMapping(f); }
        else Check(false, "open GPU mapped reader fixture");
        Fixture h(dir, 713);
        if (h.Open()) HeartbeatAndDirectory(h);
        else Check(false, "open heartbeat fixture");
    }
    // Only owned fixture files, all mappings already released.
    ipc::CancelRequest(dir.wstring(), GetCurrentProcessId(), 712);
    ipc::CancelRequest(dir.wstring(), GetCurrentProcessId(), 713);
    std::filesystem::remove(dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
