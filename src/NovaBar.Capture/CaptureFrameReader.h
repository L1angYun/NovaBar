#pragma once
#include "GpuTransfer.h"

namespace novabar::capture
{
    class FrameReader
    {
        HANDLE m_file = INVALID_HANDLE_VALUE, m_mapping = nullptr;
        detail::Mapping const* m_view = nullptr;
        uint64_t m_bytes = 0;
        uint64_t m_lastRevision = 0, m_lastSerial = 0;
        std::wstring m_path;
        std::wstring m_metadataDirectory;
        uint32_t m_metadataPid = 0;
        uint64_t m_metadataId = 0, m_metadataRevision = 0, m_metadataTick = 0;
        uint64_t m_mappedGpuInterval = 0;
        Request m_metadataRequest{};
        detail::Response m_metadataResponse{};

        static bool MetadataFresh(uint64_t stamp, uint64_t interval, uint64_t now)
        {
            return interval && stamp && now >= stamp && now - stamp < interval;
        }

        bool ValidMapping(detail::Response const& response) const
        {
            return m_view->Magic == MappingMagic && m_view->Version == ProtocolVersion && m_view->Size == sizeof(detail::Mapping)
                && m_view->CapacityBytes == response.CapacityBytes && m_bytes == detail::MappingBytes(response.CapacityBytes);
        }

        enum class SlotRead { None, NewFrame, OtherRevision, OtherGeometry, Invalid };
        SlotRead ReadMappedSlot(Request const& request, detail::Response const& response, Frame& out,
                               bool acceptGpuMetadata, bool gpuOnly, uint64_t now)
        {
            uint32_t expectedWidth = 0, expectedHeight = 0;
            if (!detail::Dimensions(request, expectedWidth, expectedHeight)) return SlotRead::Invalid;
            for (int attempt = 0; attempt != 3; ++attempt)
            {
                const LONG index = m_view->ActiveSlot;
                std::atomic_thread_fence(std::memory_order_acquire);
                if (index < 0 || index > 1) return SlotRead::Invalid;
                detail::Slot const& slot = m_view->Slots[index];
                const LONG64 before = detail::ReadSequence(slot.Sequence);
                if (!before || (before & 1)) continue;
                Frame frame;
                frame.Width = slot.Width; frame.Height = slot.Height; frame.Stride = slot.Stride;
                frame.Revision = slot.Revision; frame.Serial = slot.Serial; frame.Status = slot.Status;
                frame.Error = slot.Error; frame.PublishedTick = slot.PublishedTick;
                frame.PayloadFlags = slot.PayloadFlags;
                frame.RenderedBounds = slot.RenderedBounds; frame.MotionSequence = slot.MotionSequence;
                frame.BitmapBounds = slot.BitmapBounds;
                frame.Generation = slot.Generation; frame.ReadyFenceValue = slot.ReadyFenceValue;
                frame.TextureSlot = slot.TextureSlot;
                const auto gpuReserved0 = slot.GpuReserved0;
                const auto gpuReserved1 = slot.GpuReserved1;
                // The writer completes the inactive slot before advertising
                // it. A reused slot can have a valid sequence but no longer
                // be active if publication raced the first index read.
                if (gpuOnly && index != m_view->ActiveSlot) continue;
                if (before != detail::ReadSequence(slot.Sequence)) continue;
                if (frame.Revision != request.Revision) return SlotRead::OtherRevision;
                const uint64_t publishedNow = gpuOnly ? now : GetTickCount64();
                if (!detail::Fresh(frame.PublishedTick, publishedNow))
                { out.Error = HRESULT_FROM_WIN32(WAIT_TIMEOUT); return SlotRead::None; }
                if (frame.Status != Status::Waiting && frame.Status != Status::Ready
                    && frame.Status != Status::Unavailable && frame.Status != Status::InvalidRequest)
                    return SlotRead::Invalid;
                if (frame.Status == Status::Ready)
                {
                    const bool clientOptics = (request.Flags & ClientOptics) != 0;
                    const bool isGpu = frame.PayloadFlags == (BaseCanvasPayload | GpuBasePayload);
                    if ((gpuOnly && !isGpu) || (isGpu && (!acceptGpuMetadata || !(request.Flags & GpuBase) || !clientOptics
                            || frame.Error != S_OK || gpuReserved0 || gpuReserved1
                            || !gpu::ValidExpectedFrame(gpu::ExpectedFrame(frame), publishedNow)))
                        || frame.PayloadFlags != (isGpu ? BaseCanvasPayload | GpuBasePayload : clientOptics ? BaseCanvasPayload : 0u)
                        || (clientOptics && (frame.MotionSequence || frame.RenderedBounds.Left || frame.RenderedBounds.Top
                            || frame.RenderedBounds.Right || frame.RenderedBounds.Bottom)))
                        return SlotRead::Invalid;
                    if (request.Flags & WorldCanvas)
                    {
                        const auto canvas = detail::CanvasBounds(request);
                        if (frame.BitmapBounds.Left != canvas.Left || frame.BitmapBounds.Top != canvas.Top
                            || frame.BitmapBounds.Right != canvas.Right || frame.BitmapBounds.Bottom != canvas.Bottom)
                            return SlotRead::OtherGeometry;
                    }
                    if (!clientOptics && request.MotionSequence
                        && (!frame.MotionSequence || frame.MotionSequence > request.MotionSequence
                            || frame.RenderedBounds.Right <= frame.RenderedBounds.Left
                            || frame.RenderedBounds.Bottom <= frame.RenderedBounds.Top))
                        return SlotRead::Invalid;
                    const uint64_t bytes = static_cast<uint64_t>(frame.Stride) * frame.Height;
                    if (frame.Width != expectedWidth || frame.Height != expectedHeight || frame.Stride != (isGpu ? 0u : frame.Width * 4)
                        || !frame.Serial || (!isGpu && (!bytes || bytes > response.CapacityBytes || bytes > MaxFrameBytes)))
                        return SlotRead::OtherGeometry;
                    if (!isGpu && (frame.Revision != m_lastRevision || frame.Serial != m_lastSerial))
                    {
                        auto const* pixels = reinterpret_cast<uint8_t const*>(m_view) + sizeof(detail::Mapping) + index * response.CapacityBytes;
                        frame.Pixels.assign(pixels, pixels + bytes);
                    }
                }
                else if (frame.Width || frame.Height || frame.Stride || frame.PayloadFlags
                    || (gpuOnly && (frame.MotionSequence || frame.RenderedBounds.Left || frame.RenderedBounds.Top
                        || frame.RenderedBounds.Right || frame.RenderedBounds.Bottom || frame.BitmapBounds.Left || frame.BitmapBounds.Top
                        || frame.BitmapBounds.Right || frame.BitmapBounds.Bottom || frame.Generation || frame.ReadyFenceValue
                        || frame.TextureSlot || gpuReserved0 || gpuReserved1)))
                    return SlotRead::Invalid;
                std::atomic_thread_fence(std::memory_order_acquire);
                if (gpuOnly && index != m_view->ActiveSlot) continue;
                if (before != detail::ReadSequence(slot.Sequence)) continue;
                const bool newFrame = frame.Status == Status::Ready && (!frame.Pixels.empty()
                    || (frame.PayloadFlags == (BaseCanvasPayload | GpuBasePayload)
                        && (frame.Revision != m_lastRevision || frame.Serial != m_lastSerial)));
                if (newFrame) { m_lastRevision = frame.Revision; m_lastSerial = frame.Serial; }
                out = std::move(frame);
                return newFrame ? SlotRead::NewFrame : SlotRead::None;
            }
            return SlotRead::None;
        }
    public:
        FrameReader() = default;
        FrameReader(FrameReader const&) = delete;
        FrameReader& operator=(FrameReader const&) = delete;
        ~FrameReader() { Close(); }
        void Close()
        {
            if (m_view) UnmapViewOfFile(m_view);
            if (m_mapping) CloseHandle(m_mapping);
            if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
            m_view = nullptr; m_mapping = nullptr; m_file = INVALID_HANDLE_VALUE;
            m_path.clear(); m_bytes = 0;
            m_lastRevision = m_lastSerial = 0;
            m_metadataDirectory.clear(); m_metadataPid = 0;
            m_metadataId = m_metadataRevision = m_metadataTick = 0;
            m_mappedGpuInterval = 0;
            m_metadataRequest = {}; m_metadataResponse = {};
        }
        // Rendering never opens, remaps or closes files. The normal source
        // poll is responsible for validating and refreshing this context.
        bool ReadMappedGpu(uint32_t pid, uint64_t id, uint64_t revision, Frame& out, uint64_t now = GetTickCount64())
        {
            out = {};
            out.Revision = revision;
            out.Error = E_PENDING;
            if (!m_view || m_metadataDirectory.empty() || m_metadataPid != pid || m_metadataId != id
                || m_metadataRevision != revision || !MetadataFresh(m_metadataTick, m_mappedGpuInterval, now)
                || !detail::Fresh(m_metadataRequest.HeartbeatTick, now)) return false;
            const auto result = ValidMapping(m_metadataResponse) && !m_metadataResponse.Reserved && !m_view->Reserved
                ? ReadMappedSlot(m_metadataRequest, m_metadataResponse, out, true, true, now) : SlotRead::Invalid;
            if (result == SlotRead::Invalid || result == SlotRead::OtherGeometry)
            {
                out.Status = Status::Unavailable;
                out.Error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            return result == SlotRead::NewFrame;
        }

        // Zero keeps strict metadata I/O. Opt-in reuse is capped at 100 ms;
        // mapped publication state and freshness are still checked every call.
        bool ReadLatest(std::wstring const& dir, uint32_t pid, uint64_t id, uint64_t revision, Frame& out,
                        uint64_t metadataPollIntervalMs = 0, bool acceptGpuMetadata = false)
        {
            return ReadLatestImpl(dir, pid, id, revision, out,
                std::min<uint64_t>(metadataPollIntervalMs, 100), acceptGpuMetadata, true);
        }
    private:
        bool ReadLatestImpl(std::wstring const& dir, uint32_t pid, uint64_t id, uint64_t revision, Frame& out,
                            uint64_t interval, bool acceptGpuMetadata, bool reuseMetadata)
        {
            out = {};
            out.Revision = revision;
            m_mappedGpuInterval = 0;
            const auto fail = [&](HRESULT error, Status status = Status::Unavailable)
            {
                Close(); out = {}; out.Revision = revision;
                out.Status = status; out.Error = error;
                return false;
            };
            const auto readFailure = [&](DWORD error)
            {
                // No request/response yet is normal while the broker starts.
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    return fail(S_OK, Status::Waiting);
                return fail(HRESULT_FROM_WIN32(error));
            };
            DWORD error = ERROR_SUCCESS;
            const uint64_t metadataNow = GetTickCount64();
            const bool cachedMetadata = reuseMetadata && m_view && m_metadataDirectory == dir
                && m_metadataPid == pid && m_metadataId == id && m_metadataRevision == revision
                && MetadataFresh(m_metadataTick, interval, metadataNow);
            Request request = cachedMetadata ? m_metadataRequest : Request{};
            if (!cachedMetadata
                && !detail::ReadSmall(detail::Path(dir, pid, id, L".request"), &request, sizeof(request), &error)) return readFailure(error);
            if (!detail::ValidRequest(request, false) || request.Pid != pid || request.TargetId != id)
                return fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            if (!detail::Fresh(request.HeartbeatTick) || request.Revision != revision
                || (!request.Visible && !(request.Flags & KeepWarm)))
                return fail(S_OK, Status::Waiting);
            detail::Response response = cachedMetadata ? m_metadataResponse : detail::Response{};
            if (!cachedMetadata
                && !detail::ReadSmall(detail::Path(dir, pid, id, L".response"), &response, sizeof(response), &error)) return readFailure(error);
            if (response.Magic != ResponseMagic || response.Version != ProtocolVersion || response.Size != sizeof(response)
                || !response.CapacityBytes || response.CapacityBytes > MaxFrameBytes
                || !detail::SafeFileName(response.FileName, std::size(response.FileName)))
                return fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            const std::wstring path = (std::filesystem::path(dir) / response.FileName).wstring();
            if (path != m_path)
            {
                Close();
                m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (m_file == INVALID_HANDLE_VALUE) return fail(HRESULT_FROM_WIN32(GetLastError()));
                LARGE_INTEGER size{};
                if (!GetFileSizeEx(m_file, &size)) return fail(HRESULT_FROM_WIN32(GetLastError()));
                if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) != detail::MappingBytes(response.CapacityBytes))
                    return fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
                m_bytes = size.QuadPart;
                m_mapping = CreateFileMappingW(m_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
                if (!m_mapping) return fail(HRESULT_FROM_WIN32(GetLastError()));
                m_view = static_cast<detail::Mapping const*>(MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(m_bytes)));
                if (!m_view) return fail(HRESULT_FROM_WIN32(GetLastError()));
                m_path = path;
            }
            if (!ValidMapping(response))
                return fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            if (!cachedMetadata)
            {
                m_metadataDirectory = dir; m_metadataPid = pid; m_metadataId = id;
                m_metadataRevision = revision; m_metadataTick = metadataNow;
                m_metadataRequest = request; m_metadataResponse = response;
            }
            const auto result = ReadMappedSlot(request, response, out, acceptGpuMetadata, false, 0);
            if (cachedMetadata && (result == SlotRead::OtherGeometry || result == SlotRead::OtherRevision))
                return ReadLatestImpl(dir, pid, id, revision, out, interval, acceptGpuMetadata, false);
            if (result == SlotRead::Invalid || result == SlotRead::OtherGeometry)
                return fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            if (acceptGpuMetadata && (request.Flags & GpuBase)) m_mappedGpuInterval = interval;
            return result == SlotRead::NewFrame;
        }
    };
}
