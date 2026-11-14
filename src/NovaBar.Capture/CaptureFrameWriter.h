#pragma once
#include "GpuTransfer.h"
#include <utility>

namespace novabar::capture::detail
{
    // Only the full-trust broker creates these files; clients map them read-only.
    class FrameWriter
    {
        HANDLE m_file = INVALID_HANDLE_VALUE, m_mapping = nullptr;
        Mapping* m_view = nullptr;
        std::wstring m_responsePath;
        Response m_response{};
        uint64_t m_serial = 0;
        uint64_t m_gpuReservation = 0;
        void CloseMapping()
        {
            if (m_view) UnmapViewOfFile(m_view);
            if (m_mapping) CloseHandle(m_mapping);
            if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
            m_view = nullptr; m_mapping = nullptr; m_file = INVALID_HANDLE_VALUE;
        }
    public:
        FrameWriter() = default;
        FrameWriter(FrameWriter const&) = delete;
        FrameWriter& operator=(FrameWriter const&) = delete;
        ~FrameWriter() { Close(); }
        void Close()
        {
            if (!m_responsePath.empty())
            {
                Response current{};
                if (ReadSmall(m_responsePath, &current, sizeof(current))
                    && std::memcmp(&current, &m_response, sizeof(current)) == 0) DeleteFileW(m_responsePath.c_str());
            }
            CloseMapping(); m_responsePath.clear(); m_response = {}; m_serial = m_gpuReservation = 0;
        }
        bool Ensure(std::wstring const& dir, uint32_t pid, uint64_t id, uint64_t capacity)
        {
            if (!capacity || capacity > MaxFrameBytes) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
            if (m_view && capacity <= m_response.CapacityBytes) return true;
            static std::atomic<uint64_t> generation{};
            const uint64_t rounded = std::min<uint64_t>(MaxFrameBytes, (capacity + 65535) & ~65535ull);
            const std::wstring filename = Stem(pid, id) + L"." + std::to_wstring(GetCurrentProcessId())
                + L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++generation) + L".frames";
            const std::wstring path = (std::filesystem::path(dir) / filename).wstring();
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{}; size.QuadPart = MappingBytes(rounded);
            if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file))
            { const DWORD error = GetLastError(); CloseHandle(file); SetLastError(error); return false; }
            HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
            if (!mapping) { const DWORD error = GetLastError(); CloseHandle(file); SetLastError(error); return false; }
            auto* view = static_cast<Mapping*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(size.QuadPart)));
            if (!view) { const DWORD error = GetLastError(); CloseHandle(mapping); CloseHandle(file); SetLastError(error); return false; }
            new (view) Mapping{};
            view->CapacityBytes = rounded;
            Response response{};
            response.CapacityBytes = rounded;
            if (filename.size() >= std::size(response.FileName))
            { UnmapViewOfFile(view); CloseHandle(mapping); CloseHandle(file); SetLastError(ERROR_BUFFER_OVERFLOW); return false; }
            std::copy(filename.begin(), filename.end(), response.FileName);
            const std::wstring responsePath = Path(dir, pid, id, L".response");
            if (!WriteSmall(responsePath, &response, sizeof(response)))
            { const DWORD error = GetLastError(); UnmapViewOfFile(view); CloseHandle(mapping); CloseHandle(file); SetLastError(error); return false; }
            CloseMapping();
            m_file = file; m_mapping = mapping; m_view = view;
            m_response = response; m_responsePath = responsePath;
            return true;
        }
        bool Publish(uint64_t revision, Status status, HRESULT error, uint32_t width, uint32_t height,
            std::vector<uint8_t> const& pixels, bool newFrame = true,
            PixelRect renderedBounds = {}, uint64_t motionSequence = 0, PixelRect bitmapBounds = {}, uint32_t payloadFlags = 0)
        {
            if (!m_view) { SetLastError(ERROR_INVALID_HANDLE); return false; }
            const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
            if (status == Status::Ready && (!width || !height || bytes != pixels.size() || bytes > m_response.CapacityBytes))
            { SetLastError(ERROR_INVALID_DATA); return false; }
            if ((payloadFlags & ~BaseCanvasPayload) || (status == Status::Ready && payloadFlags == BaseCanvasPayload
                && (motionSequence || renderedBounds.Left || renderedBounds.Top || renderedBounds.Right || renderedBounds.Bottom
                    || bitmapBounds.Right <= bitmapBounds.Left || bitmapBounds.Bottom <= bitmapBounds.Top)))
            { SetLastError(ERROR_INVALID_DATA); return false; }
            if (status != Status::Ready) width = height = 0;
            const LONG active = m_view->ActiveSlot;
            if (active < 0 || active > 1) { SetLastError(ERROR_INVALID_DATA); return false; }
            const Slot& previous = m_view->Slots[active];
            const bool renew = !newFrame && status == Status::Ready
                && previous.Status == status && previous.Width == width && previous.Height == height
                && previous.Revision == revision && previous.Serial != 0 && previous.PayloadFlags == payloadFlags;
            const LONG index = renew ? active : active == 0 ? 1 : 0;
            Slot& slot = m_view->Slots[index];
            m_gpuReservation = 0;
            InterlockedIncrement64(&slot.Sequence); // odd: reader must retry
            slot.Width = width; slot.Height = height; slot.Stride = width * 4;
            slot.Status = status; slot.Error = error; slot.Revision = revision;
            slot.PayloadFlags = status == Status::Ready ? payloadFlags : 0;
            slot.RenderedBounds = status == Status::Ready ? renderedBounds : PixelRect{};
            slot.MotionSequence = status == Status::Ready ? motionSequence : 0;
            slot.BitmapBounds = status == Status::Ready ? bitmapBounds : PixelRect{};
            slot.Generation = slot.ReadyFenceValue = 0; slot.TextureSlot = 0;
            slot.GpuReserved0 = 0; slot.GpuReserved1 = 0;
            if (newFrame || !m_serial || (status == Status::Ready && !renew)) ++m_serial;
            slot.Serial = m_serial; slot.PublishedTick = GetTickCount64();
            if (status == Status::Ready && !renew)
            {
                auto* target = reinterpret_cast<uint8_t*>(m_view) + sizeof(Mapping) + index * m_response.CapacityBytes;
                std::memcpy(target, pixels.data(), static_cast<size_t>(bytes));
            }
            MemoryBarrier();
            InterlockedIncrement64(&slot.Sequence); // even: immutable until reused
            InterlockedExchange(&m_view->ActiveSlot, index);
            return true;
        }
        // Reserve before broker submission/slot bookkeeping so acknowledgements
        // identify precisely the serial eventually published. Gaps are allowed.
        uint64_t ReserveGpuSerial() noexcept
        {
            if (!m_view || m_serial == UINT64_MAX) return 0;
            m_gpuReservation = ++m_serial;
            return m_gpuReservation;
        }
        bool PublishGpu(gpu::GpuExpectedFrame const& frame)
        {
            if (!m_view) { SetLastError(ERROR_INVALID_HANDLE); return false; }
            if (!gpu::ValidExpectedFrame(frame) || !m_gpuReservation || frame.SourceSerial != m_gpuReservation
                || frame.SourceSerial != m_serial)
            { SetLastError(ERROR_INVALID_DATA); return false; }
            const LONG active = m_view->ActiveSlot;
            if (active < 0 || active > 1) { SetLastError(ERROR_INVALID_DATA); return false; }
            const LONG index = active == 0 ? 1 : 0;
            auto& slot = m_view->Slots[index];
            InterlockedIncrement64(&slot.Sequence);
            slot.Width = frame.Width; slot.Height = frame.Height; slot.Stride = 0;
            slot.Status = Status::Ready; slot.Error = S_OK;
            slot.PayloadFlags = BaseCanvasPayload | GpuBasePayload;
            slot.Revision = frame.Revision; slot.Serial = frame.SourceSerial; slot.PublishedTick = frame.PublishedTick;
            slot.RenderedBounds = {}; slot.MotionSequence = 0; slot.BitmapBounds = frame.BitmapBounds;
            slot.Generation = frame.Generation; slot.ReadyFenceValue = frame.ReadyFenceValue; slot.TextureSlot = frame.TextureSlot;
            slot.GpuReserved0 = 0; slot.GpuReserved1 = 0;
            InterlockedIncrement64(&slot.Sequence);
            InterlockedExchange(&m_view->ActiveSlot, index);
            m_gpuReservation = 0;
            return true;
        }
        // Source status has been revalidated by the broker. Renew only the exact
        // existing GPU publication; a released shared slot is never republished.
        bool RenewGpu(gpu::GpuExpectedFrame const& frame)
        {
            if (!m_view) { SetLastError(ERROR_INVALID_HANDLE); return false; }
            if (!gpu::ValidExpectedFrame(frame) || m_gpuReservation)
            { SetLastError(ERROR_INVALID_DATA); return false; }
            const LONG active = m_view->ActiveSlot;
            if (active < 0 || active > 1) { SetLastError(ERROR_INVALID_DATA); return false; }
            auto& slot = m_view->Slots[active];
            if (slot.Status != Status::Ready || slot.Error != S_OK || slot.PayloadFlags != (BaseCanvasPayload | GpuBasePayload)
                || slot.Revision != frame.Revision || slot.Serial != frame.SourceSerial || slot.Generation != frame.Generation
                || slot.ReadyFenceValue != frame.ReadyFenceValue || slot.TextureSlot != frame.TextureSlot
                || slot.Width != frame.Width || slot.Height != frame.Height || slot.Stride
                || std::memcmp(&slot.BitmapBounds, &frame.BitmapBounds, sizeof(PixelRect))
                || frame.PublishedTick < slot.PublishedTick)
            { SetLastError(ERROR_INVALID_DATA); return false; }
            InterlockedIncrement64(&slot.Sequence);
            slot.PublishedTick = frame.PublishedTick;
            InterlockedIncrement64(&slot.Sequence);
            return true;
        }
        Mapping* ViewForTest() { return m_view; }
        std::wstring MappingNameForTest() const { return m_response.FileName; }
    };
}
