#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include "../NovaBar.XamlBridge/GlassPixels.h"

// Versioned, same-user file transport. No named kernel objects or ACL changes.
namespace novabar::capture
{
    inline constexpr uint32_t ProtocolMagic = 0x4E424351; // NBCQ
    inline constexpr uint32_t ProtocolVersion = 2;
    inline constexpr uint32_t ResponseMagic = 0x4E424352;
    inline constexpr uint32_t MappingMagic = 0x4E424346;
    inline constexpr uint64_t MaxPixels = 8ull * 1024 * 1024;
    inline constexpr uint64_t MaxFrameBytes = MaxPixels * 4;
    inline constexpr uint64_t HeartbeatTimeoutMs = 1500;
    enum class Kind : uint32_t { Taskbar = 0, Panel = 1 };
    enum class Status : uint32_t { Waiting = 0, Ready = 1, Unavailable = 2, InvalidRequest = 3 };
    inline constexpr uint32_t HostClipsCorners = 1u << 0;
    inline constexpr uint32_t KeepWarm = 1u << 1;
    inline constexpr uint32_t WorldCanvas = 1u << 2;
    inline constexpr uint32_t ClientOptics = 1u << 3;
    inline constexpr uint32_t GpuBase = 1u << 4;
    inline constexpr uint32_t BaseCanvasPayload = 1u << 0;
    inline constexpr uint32_t GpuBasePayload = 1u << 1;

    struct Request
    {
        uint32_t Magic = ProtocolMagic, Version = ProtocolVersion;
        uint32_t Size = sizeof(Request), Pid = 0;
        uint64_t TargetId = 0, Revision = 0, HeartbeatTick = 0;
        uint64_t CoordinateHwnd = 0, TopLevelHwnd = 0;
        PixelRect Bounds;
        capture::Kind Kind = capture::Kind::Panel;
        uint32_t Flags = 0;
        double Scale = 1, Radius = 16;
        GlassOptions Material;
        double Specular = .35;
        uint32_t Visible = 1, Reserved2 = 0;
        // Layout allocation is independent of animated screen-space sampling.
        // Zero dimensions preserve the scale-derived allocation for taskbars.
        uint32_t OutputWidth = 0, OutputHeight = 0;
        uint64_t MotionSequence = 0;
    };
    static_assert(std::is_trivially_copyable_v<Request>);
    static_assert(sizeof(Request) == 176 && offsetof(Request, Flags) == 76
        && offsetof(Request, OutputWidth) == 160 && offsetof(Request, MotionSequence) == 168);

    struct Frame
    {
        std::vector<uint8_t> Pixels;
        uint32_t Width = 0, Height = 0, Stride = 0;
        uint64_t Revision = 0, Serial = 0;
        capture::Status Status = capture::Status::Waiting;
        HRESULT Error = S_OK;
        uint64_t PublishedTick = 0;
        uint32_t PayloadFlags = 0;
        PixelRect RenderedBounds{};
        uint64_t MotionSequence = 0;
        PixelRect BitmapBounds{};
        uint64_t Generation = 0, ReadyFenceValue = 0;
        uint32_t TextureSlot = 0;
    };

    namespace detail
    {
        struct Response
        {
            uint32_t Magic = ResponseMagic, Version = ProtocolVersion;
            uint32_t Size = sizeof(Response), Reserved = 0;
            uint64_t CapacityBytes = 0;
            wchar_t FileName[128]{};
        };
        struct alignas(64) Slot
        {
            alignas(8) volatile LONG64 Sequence = 0;
            uint32_t Width = 0, Height = 0, Stride = 0;
            capture::Status Status = capture::Status::Waiting;
            HRESULT Error = S_OK;
            uint32_t PayloadFlags = 0;
            uint64_t Revision = 0, Serial = 0, PublishedTick = 0;
            PixelRect RenderedBounds{};
            uint64_t MotionSequence = 0;
            PixelRect BitmapBounds{};
            uint64_t Generation = 0, ReadyFenceValue = 0;
            uint32_t TextureSlot = 0, GpuReserved0 = 0;
            uint64_t GpuReserved1 = 0;
        };
        static_assert(sizeof(Slot) == 128 && offsetof(Slot, PayloadFlags) == 28 && offsetof(Slot, RenderedBounds) == 56
            && offsetof(Slot, MotionSequence) == 72 && offsetof(Slot, BitmapBounds) == 80
            && offsetof(Slot, Generation) == 96 && offsetof(Slot, ReadyFenceValue) == 104
            && offsetof(Slot, TextureSlot) == 112);
        struct alignas(64) Mapping
        {
            uint32_t Magic = MappingMagic, Version = ProtocolVersion;
            uint32_t Size = sizeof(Mapping), Reserved = 0;
            uint64_t CapacityBytes = 0;
            alignas(4) volatile LONG ActiveSlot = 0;
            Slot Slots[2];
        };
        inline uint64_t MappingBytes(uint64_t capacity) { return sizeof(Mapping) + capacity * 2; }
        inline LONG64 ReadSequence(volatile LONG64 const& value)
        {
            const LONG64 result = value; // aligned 64-bit read on the required x64 host
            std::atomic_thread_fence(std::memory_order_acquire);
            return result;
        }
        inline std::wstring Stem(uint32_t pid, uint64_t id)
        { return L"capture." + std::to_wstring(pid) + L"." + std::to_wstring(id); }
        inline std::wstring Path(std::wstring const& dir, uint32_t pid, uint64_t id, wchar_t const* suffix)
        { return (std::filesystem::path(dir) / (Stem(pid, id) + suffix)).wstring(); }
        inline bool Fresh(uint64_t stamp, uint64_t now = GetTickCount64())
        { return stamp != 0 && stamp <= now && now - stamp <= HeartbeatTimeoutMs; }
        inline bool SafeFileName(wchar_t const* value, size_t count)
        {
            size_t length = 0;
            while (length < count && value[length]) ++length;
            if (!length || length == count || length < 12) return false;
            const std::wstring name(value, length);
            if (!name.starts_with(L"capture.") || !name.ends_with(L".frames")) return false;
            for (wchar_t c : name) if (!(c >= L'0' && c <= L'9') && c != L'.'
                && !(c >= L'a' && c <= L'z') && c != L'-') return false;
            return name.find(L"..") == std::wstring::npos;
        }
        inline bool ReadSmall(std::wstring const& path, void* data, DWORD size, DWORD* failure = nullptr)
        {
            if (failure) *failure = ERROR_SUCCESS;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) { if (failure) *failure = GetLastError(); return false; }
            LARGE_INTEGER length{};
            DWORD read = 0;
            DWORD error = ERROR_SUCCESS;
            if (!GetFileSizeEx(file, &length)) error = GetLastError();
            else if (length.QuadPart != size) error = ERROR_INVALID_DATA;
            else if (!ReadFile(file, data, size, &read, nullptr)) error = GetLastError();
            else if (read != size) error = ERROR_HANDLE_EOF;
            CloseHandle(file);
            if (failure) *failure = error;
            if (error) SetLastError(error);
            return error == ERROR_SUCCESS;
        }
        inline bool WriteSmall(std::wstring const& path, void const* data, DWORD size)
        {
            static std::atomic<uint64_t> sequence{};
            const std::wstring temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId())
                + L"." + std::to_wstring(++sequence);
            HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            const bool saved = !!WriteFile(file, data, size, &written, nullptr);
            const bool ok = saved && written == size;
            DWORD error = saved ? (ok ? ERROR_SUCCESS : ERROR_WRITE_FAULT) : GetLastError();
            CloseHandle(file);
            if (ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            if (ok) error = GetLastError();
            DeleteFileW(temp.c_str());
            SetLastError(error);
            return false;
        }
        inline PixelRect CanvasBounds(Request const& r)
        {
            if (!(r.Flags & WorldCanvas)) return r.Bounds;
            if (!r.OutputWidth || !r.OutputHeight || !std::isfinite(r.Scale) || r.Scale < .25 || r.Scale > 8) return {};
            const int64_t left = static_cast<int64_t>(r.Bounds.Left) + static_cast<int16_t>(r.Reserved2 & 0xffff);
            const int64_t top = static_cast<int64_t>(r.Bounds.Top) + static_cast<int16_t>(r.Reserved2 >> 16);
            const int64_t width = std::llround(r.OutputWidth * r.Scale), height = std::llround(r.OutputHeight * r.Scale);
            if (width <= 0 || height <= 0 || width > 32768 || height > 32768
                || width * height > 16ll * 1024 * 1024 || left < INT_MIN || top < INT_MIN
                || left + width > INT_MAX || top + height > INT_MAX) return {};
            return {static_cast<int>(left), static_cast<int>(top), static_cast<int>(left + width), static_cast<int>(top + height)};
        }
        inline bool SetCanvasBounds(Request& r, PixelRect const& canvas)
        {
            const int64_t width = static_cast<int64_t>(canvas.Right) - canvas.Left;
            const int64_t height = static_cast<int64_t>(canvas.Bottom) - canvas.Top;
            const int64_t dx = static_cast<int64_t>(canvas.Left) - r.Bounds.Left;
            const int64_t dy = static_cast<int64_t>(canvas.Top) - r.Bounds.Top;
            if (width <= 0 || height <= 0 || width > 32768 || height > 32768
                || !std::isfinite(r.Scale) || r.Scale < .25 || r.Scale > 8
                || dx < INT16_MIN || dx > INT16_MAX || dy < INT16_MIN || dy > INT16_MAX)
            { SetLastError(ERROR_INVALID_DATA); return false; }
            const double outputWidth = std::ceil(width / r.Scale), outputHeight = std::ceil(height / r.Scale);
            if (outputWidth < 1 || outputHeight < 1 || outputWidth > 16384 || outputHeight > 16384
                || outputWidth * outputHeight > MaxPixels)
            { SetLastError(ERROR_INVALID_DATA); return false; }
            r.Flags |= WorldCanvas;
            r.OutputWidth = static_cast<uint32_t>(outputWidth); r.OutputHeight = static_cast<uint32_t>(outputHeight);
            r.Reserved2 = static_cast<uint16_t>(static_cast<int16_t>(dx))
                | static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(dy))) << 16;
            const auto result = CanvasBounds(r);
            if (result.Right <= result.Left || result.Bottom <= result.Top)
            { SetLastError(ERROR_INVALID_DATA); return false; }
            return true;
        }
        inline bool Dimensions(Request const& r, uint32_t& width, uint32_t& height)
        {
            const int64_t w = static_cast<int64_t>(r.Bounds.Right) - r.Bounds.Left;
            const int64_t h = static_cast<int64_t>(r.Bounds.Bottom) - r.Bounds.Top;
            if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || !std::isfinite(r.Scale)
                || r.Scale < .25 || r.Scale > 8) return false;
            const double dw = std::ceil(w / r.Scale), dh = std::ceil(h / r.Scale);
            if ((r.OutputWidth == 0) != (r.OutputHeight == 0)) return false;
            const double outputWidth = r.OutputWidth ? r.OutputWidth : dw;
            const double outputHeight = r.OutputHeight ? r.OutputHeight : dh;
            if (outputWidth < 1 || outputHeight < 1 || outputWidth > 16384
                || outputHeight > 16384 || outputWidth * outputHeight > MaxPixels) return false;
            width = static_cast<uint32_t>(outputWidth); height = static_cast<uint32_t>(outputHeight);
            return true;
        }
        inline bool ValidRequest(Request const& r, bool fresh = true)
        {
            uint32_t width = 0, height = 0;
            return r.Magic == ProtocolMagic && r.Version == ProtocolVersion && r.Size == sizeof(Request)
                && r.Pid && r.TargetId && r.Revision && r.CoordinateHwnd && r.TopLevelHwnd
                && (!fresh || Fresh(r.HeartbeatTick)) && Dimensions(r, width, height)
                && (r.Kind == Kind::Panel || r.Kind == Kind::Taskbar) && r.Visible <= 1
                && !(r.Flags & ~(HostClipsCorners | KeepWarm | WorldCanvas | ClientOptics | GpuBase))
                && (!(r.Flags & GpuBase) || (r.Kind == Kind::Panel
                    && (r.Flags & (WorldCanvas | ClientOptics)) == (WorldCanvas | ClientOptics)))
                && (!(r.Flags & KeepWarm) || r.Kind == Kind::Panel)
                && (!(r.Flags & ClientOptics) || (r.Kind == Kind::Panel
                    && (r.Flags & (HostClipsCorners | WorldCanvas)) == (HostClipsCorners | WorldCanvas)))
                && (!(r.Flags & WorldCanvas) || (r.Kind == Kind::Panel
                    && CanvasBounds(r).Right > CanvasBounds(r).Left && CanvasBounds(r).Bottom > CanvasBounds(r).Top))
                && std::isfinite(r.Radius) && r.Radius >= 0 && r.Radius <= 1024
                && std::isfinite(r.Specular) && r.Specular >= 0 && r.Specular <= 1
                && std::isfinite(r.Material.Lens) && std::isfinite(r.Material.Dispersion)
                && std::isfinite(r.Material.Noise) && std::isfinite(r.Material.Tint)
                && std::isfinite(r.Material.Blur);
        }
    }

    inline bool WriteRequest(std::wstring const& dir, Request const& request)
    {
        if (!detail::ValidRequest(request)) { SetLastError(ERROR_INVALID_DATA); return false; }
        return detail::WriteSmall(detail::Path(dir, request.Pid, request.TargetId, L".request"), &request, sizeof(request));
    }
    inline void CancelRequest(std::wstring const& dir, uint32_t pid, uint64_t id)
    { DeleteFileW(detail::Path(dir, pid, id, L".request").c_str()); }
}
