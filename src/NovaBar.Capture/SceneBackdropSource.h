#pragma once
#include "../NovaBar.XamlBridge/WgcCapture.h"
#include "CaptureTiming.h"
#include "GpuSceneCompositor.h"
#include <array>
#include <emmintrin.h>
#include <memory>
#include <unordered_map>

namespace novabar
{
    struct SceneSourceProfile
    {
        int Width = 0, Height = 0;
        int64_t BoundsWidth = 0, BoundsHeight = 0;
        unsigned Opacity = 255;
        bool Direct = false;
        double ReadMs = 0, ComposeMs = 0;
    };
    struct SceneProfile
    {
        bool Enabled = false;
        double DiscoveryMs = 0, ReadMs = 0, ComposeMs = 0, AllocationMs = 0, CoverageMs = 0, CompareMs = 0;
        uint32_t Windows = 0, Sources = 0, NonDirectSources = 0, DetailCount = 0;
        std::array<SceneSourceProfile, 8> Details{};
    };
    struct SceneUpdate { bool Ready = false, Changed = false; HRESULT Error = S_OK; SceneProfile Profile{}; };
    enum class ScenePlacement { Actual, Promoted };
    namespace scene_detail
    {
        inline bool SameRect(PixelRect a, PixelRect b)
        { return a.Left == b.Left && a.Top == b.Top && a.Right == b.Right && a.Bottom == b.Bottom; }
        inline PixelRect Intersect(PixelRect a, PixelRect b)
        { return {std::max(a.Left, b.Left), std::max(a.Top, b.Top), std::min(a.Right, b.Right), std::min(a.Bottom, b.Bottom)}; }
        inline bool Empty(PixelRect a) { return a.Right <= a.Left || a.Bottom <= a.Top; }
        inline void RecordSourceProfile(SceneProfile& profile, PixelFrame const& frame, unsigned opacity,
            double readMs, double composeMs)
        {
            if (!profile.Enabled) return;
            const int64_t width = static_cast<int64_t>(frame.Bounds.Right) - frame.Bounds.Left;
            const int64_t height = static_cast<int64_t>(frame.Bounds.Bottom) - frame.Bounds.Top;
            const bool direct = frame.Width == width && frame.Height == height;
            ++profile.Sources; if (!direct) ++profile.NonDirectSources;
            if (profile.DetailCount < profile.Details.size())
                profile.Details[profile.DetailCount++] = {frame.Width, frame.Height, width, height, opacity, direct, readMs, composeMs};
        }
        inline PixelRect Bounds(HWND window)
        {
            RECT r{};
            if (!GetWindowRect(window, &r)) return {};
            RECT extended{};
            if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(extended)))
                && extended.right > extended.left && extended.bottom > extended.top) r = extended;
            return {r.left, r.top, r.right, r.bottom};
        }
        // Front-to-back premultiplied source-over. An opaque front window closes
        // coverage immediately, so hidden windows never need a capture session.
        class SceneAccumulator
        {
            PixelFrame m_frame;
            size_t m_open = 0;
            void Span(PixelFrame const& source, PixelRect part, unsigned opacity, uint32_t colorKey)
            {
                const int sw = source.Bounds.Right - source.Bounds.Left;
                const int sh = source.Bounds.Bottom - source.Bounds.Top;
                const bool direct = source.Width == sw && source.Height == sh;
                const auto alphaMask = _mm_set1_epi32(static_cast<int>(0xff000000u));
                const auto zero = _mm_setzero_si128();
                for (int y = part.Top; y < part.Bottom; ++y)
                {
                    const int sy = direct ? y - source.Bounds.Top : static_cast<int>(static_cast<int64_t>(y - source.Bounds.Top) * source.Height / sh);
                    const auto* src = source.Pixels.data() + static_cast<size_t>(sy) * source.Width * 4;
                    auto* dst = m_frame.Pixels.data() + (static_cast<size_t>(y - m_frame.Bounds.Top) * m_frame.Width + part.Left - m_frame.Bounds.Left) * 4;
                    for (int x = part.Left; x < part.Right; ++x, dst += 4)
                    {
                        if (static_cast<int64_t>(part.Right) - x >= 4)
                        {
                            const auto destination = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst));
                            const auto alpha = _mm_and_si128(destination, alphaMask);
                            if (_mm_movemask_epi8(_mm_cmpeq_epi32(alpha, alphaMask)) == 0xffff)
                            { x += 3; dst += 12; continue; }
                            if (direct && opacity == 255 && colorKey > 0xffffff
                                && _mm_movemask_epi8(_mm_cmpeq_epi32(alpha, zero)) == 0xffff)
                            {
                                const auto pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(
                                    src + static_cast<size_t>(x - source.Bounds.Left) * 4));
                                if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(pixels, alphaMask), alphaMask)) == 0xffff)
                                {
                                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), pixels);
                                    m_open -= 4; x += 3; dst += 12; continue;
                                }
                            }
                        }
                        if (dst[3] == 255) continue;
                        const int sx = direct ? x - source.Bounds.Left : static_cast<int>(static_cast<int64_t>(x - source.Bounds.Left) * source.Width / sw);
                        const auto* pixel = src + static_cast<size_t>(sx) * 4;
                        if (colorKey <= 0xFFFFFF && pixel[3] == 255 && pixel[0] == GetBValue(colorKey)
                            && pixel[1] == GetGValue(colorKey) && pixel[2] == GetRValue(colorKey)) continue;
                        const unsigned alpha = (pixel[3] * opacity + 127) / 255;
                        if (!alpha) continue;
                        const unsigned remaining = 255 - dst[3];
                        if (remaining == 255 && alpha == 255)
                        { memcpy(dst, pixel, 4); --m_open; continue; }
                        for (int c = 0; c < 3; ++c)
                        {
                            const unsigned premultiplied = (std::min<unsigned>(pixel[c], pixel[3]) * opacity + 127) / 255;
                            dst[c] = static_cast<uint8_t>(std::min(255u, dst[c] + (premultiplied * remaining + 127) / 255));
                        }
                        dst[3] = static_cast<uint8_t>(dst[3] + (alpha * remaining + 127) / 255);
                        if (dst[3] == 255) --m_open;
                    }
                }
            }
        public:
            explicit SceneAccumulator(PixelRect bounds, PixelFrame storage = {}) : m_frame(std::move(storage))
            {
                m_frame.Bounds = bounds; m_frame.Width = bounds.Right - bounds.Left; m_frame.Height = bounds.Bottom - bounds.Top;
                if (m_frame.Width > 0 && m_frame.Height > 0)
                { m_open = static_cast<size_t>(m_frame.Width) * m_frame.Height; m_frame.Pixels.assign(m_open * 4, 0); }
            }
            bool Composite(PixelFrame const& source, std::vector<PixelRect> const& clips = {}, unsigned opacity = 255,
                           uint32_t colorKey = 0x1000000)
            {
                if (source.Width <= 0 || source.Height <= 0 || Empty(source.Bounds)
                    || source.Pixels.size() != static_cast<size_t>(source.Width) * source.Height * 4) return false;
                const auto overlap = Intersect(source.Bounds, m_frame.Bounds);
                if (Empty(overlap)) return true;
                opacity = std::min(opacity, 255u);
                if (clips.empty()) Span(source, overlap, opacity, colorKey);
                else for (auto const& clip : clips)
                { const auto part = Intersect(overlap, clip); if (!Empty(part)) Span(source, part, opacity, colorKey); }
                return true;
            }
            bool Needs(PixelRect bounds) const
            {
                if (!m_open) return false;
                const auto part = Intersect(bounds, m_frame.Bounds);
                if (Empty(part)) return false;
                const auto alphaMask = _mm_set1_epi32(static_cast<int>(0xff000000u));
                for (int y = part.Top; y < part.Bottom; ++y)
                {
                    const auto* pixel = m_frame.Pixels.data() + (static_cast<size_t>(y - m_frame.Bounds.Top) * m_frame.Width + part.Left - m_frame.Bounds.Left) * 4;
                    for (int x = part.Left; x < part.Right; ++x, pixel += 4)
                    {
                        if (static_cast<int64_t>(part.Right) - x >= 4)
                        {
                            const auto pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixel));
                            if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(pixels, alphaMask), alphaMask)) != 0xffff)
                                return true;
                            x += 3; pixel += 12; continue;
                        }
                        if (pixel[3] < 255) return true;
                    }
                }
                return false;
            }
            bool Complete() const { return m_open == 0 && !m_frame.Pixels.empty(); }
            PixelFrame const& Frame() const { return m_frame; }
            PixelFrame TakeFrame() { return std::move(m_frame); }
        };

        struct WindowSnapshot
        {
            HWND Window = nullptr;
            DWORD Process = 0;
            PixelRect Rectangle;
            std::vector<PixelRect> Region;
            unsigned Opacity = 255;
            uint32_t ColorKey = 0x1000000;
        };
        inline std::vector<WindowSnapshot> WindowsFrom(HWND first, PixelRect requested)
        {
            std::vector<WindowSnapshot> windows;
            std::unordered_set<HWND> seen;
            for (HWND window = first; window && seen.size() < 16384; window = GetWindow(window, GW_HWNDNEXT))
            {
                if (!seen.insert(window).second) break;
                if (!IsWindowVisible(window) || IsIconic(window)) continue;
                DWORD cloaked = 0;
                if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
                WindowSnapshot item; item.Window = window; GetWindowThreadProcessId(window, &item.Process);
                item.Rectangle = Bounds(window);
                if (Empty(Intersect(requested, item.Rectangle))) continue;
                if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED)
                {
                    COLORREF key = 0; BYTE opacity = 255; DWORD flags = 0;
                    // WGC captures the window's raw surface. GDI global opacity
                    // and color-keying are applied by DWM outside that surface.
                    if (GetLayeredWindowAttributes(window, &key, &opacity, &flags))
                    {
                        if (flags & LWA_ALPHA) item.Opacity = opacity;
                        if (flags & LWA_COLORKEY) item.ColorKey = key;
                    }
                }
                if (!item.Opacity) continue;
                // WGC carries per-pixel alpha; explicit Win32 regions require an
                // additional clip because they are not part of every surface.
                HRGN region = CreateRectRgn(0, 0, 0, 0);
                const int kind = region ? GetWindowRgn(window, region) : ERROR;
                if (kind == NULLREGION) { DeleteObject(region); continue; }
                if (kind == SIMPLEREGION || kind == COMPLEXREGION)
                {
                    const DWORD bytes = GetRegionData(region, 0, nullptr);
                    std::vector<uint8_t> data(bytes);
                    RECT outer{}; GetWindowRect(window, &outer);
                    if (bytes && GetRegionData(region, bytes, reinterpret_cast<RGNDATA*>(data.data())))
                    {
                        const auto* details = reinterpret_cast<RGNDATA const*>(data.data());
                        const auto* rectangles = reinterpret_cast<RECT const*>(details->Buffer);
                        for (DWORD i = 0; i < details->rdh.nCount; ++i)
                            item.Region.push_back({outer.left + rectangles[i].left, outer.top + rectangles[i].top,
                                                   outer.left + rectangles[i].right, outer.top + rectangles[i].bottom});
                    }
                }
                if (region) DeleteObject(region);
                windows.push_back(std::move(item));
            }
            return windows;
        }
        inline std::vector<WindowSnapshot> LowerWindows(HWND excluded, PixelRect requested)
        { return WindowsFrom(GetWindow(excluded, GW_HWNDNEXT), requested); }
        struct WindowEnvironment
        {
            struct Owner
            {
                HANDLE Handle = nullptr;
                explicit Owner(DWORD pid) : Handle(OpenProcess(SYNCHRONIZE, FALSE, pid)) {}
                ~Owner() { if (Handle) CloseHandle(Handle); }
                bool Alive() const { return Handle && WaitForSingleObject(Handle, 0) == WAIT_TIMEOUT; }
            };
            static uint64_t Now() { return GetTickCount64(); }
            static auto VisibleWindows(PixelRect requested) { return WindowsFrom(GetTopWindow(nullptr), requested); }
            static auto LowerWindows(HWND excluded, PixelRect requested) { return scene_detail::LowerWindows(excluded, requested); }
            static DWORD Process(HWND window)
            { DWORD process = 0; GetWindowThreadProcessId(window, &process); return process; }
            static bool Valid(WindowSnapshot const& item)
            {
                DWORD pid = 0, cloaked = 0; GetWindowThreadProcessId(item.Window, &pid);
                return pid == item.Process && IsWindowVisible(item.Window) && !IsIconic(item.Window)
                    && !(SUCCEEDED(DwmGetWindowAttribute(item.Window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                    && SameRect(Bounds(item.Window), item.Rectangle);
            }
        };
        template<class Environment = WindowEnvironment>
        inline std::vector<WindowSnapshot> BackdropWindows(HWND excluded, PixelRect requested,
            ScenePlacement placement = ScenePlacement::Actual)
        {
            if (placement == ScenePlacement::Actual) return Environment::LowerWindows(excluded, requested);
            const DWORD owner = Environment::Process(excluded);
            if (!owner) return {};
            // A hidden shell popup will be promoted over normal application
            // windows when opened. Its current hidden Z-position is not that
            // future scene. Preserve the desktop's real front-to-back order,
            // excluding every visual owned by this shell popup process.
            auto windows = Environment::VisibleWindows(requested);
            std::erase_if(windows, [&](auto const& item)
            { return item.Window == excluded || item.Process == owner; });
            return windows;
        }
    }

    // A broker owns one pool; each target retains its own ordered scene. The
    // backend/environment parameters keep GPU capture and window discovery at
    // the boundary while crop ownership, limits and retries stay shared.
    template<class Capture = WindowCapture, class Environment = scene_detail::WindowEnvironment>
    class BasicSceneCapturePool
    {
        struct Crop
        {
            PixelRect Region, Rectangle;
            PixelFrame Pixels;
            uint64_t Generation = 0, Used = 0;
        };
        struct Source
        {
            Capture Session;
            typename Environment::Owner Owner;
            DWORD Process = 0;
            uint64_t Used = 0, ReadAt = 0, RetryAt = 0;
            HRESULT Error = E_PENDING;
            std::vector<Crop> Crops;
            explicit Source(DWORD process) : Owner(process), Process(process) {}
        };
        static constexpr size_t MaxSessions = 24, MaxCrops = 4;
        static constexpr uint64_t MaxCropBytes = 64ull * 1024 * 1024;
        capture_detail::CaptureFrameWake m_frameWake;
        std::unordered_map<HWND, std::unique_ptr<Source>> m_sources;
        std::unordered_set<HWND> m_excluded;
        winrt::com_ptr<ID3D11Device> m_gpuDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_gpuContext;
        void TrimCrops(uint64_t incoming)
        {
            for (;;)
            {
                uint64_t bytes = incoming;
                Source* oldestSource = nullptr; size_t oldestIndex = 0; uint64_t oldest = UINT64_MAX;
                for (auto const& [window, source] : m_sources)
                {
                    (void)window;
                    for (size_t i = 0; i < source->Crops.size(); ++i)
                    {
                        auto const& crop = source->Crops[i]; bytes += crop.Pixels.Pixels.size();
                        if (crop.Used < oldest) { oldest = crop.Used; oldestSource = source.get(); oldestIndex = i; }
                    }
                }
                if (bytes <= MaxCropBytes || !oldestSource) return;
                oldestSource->Crops.erase(oldestSource->Crops.begin() + static_cast<ptrdiff_t>(oldestIndex));
            }
        }
        Source* Acquire(scene_detail::WindowSnapshot const& item, bool required, bool allowStart, size_t* starts = nullptr)
        {
            // Known glass hosts are excluded only from speculative prewarm.
            // A visible scene's real LowerWindows order can legitimately need
            // another lower host (for example calendar padding over taskbar).
            if ((!required && m_excluded.contains(item.Window)) || !Environment::Valid(item)) return nullptr;
            const auto now = Environment::Now();
            auto at = m_sources.find(item.Window);
            if (at != m_sources.end() && (at->second->Process != item.Process
                || (!at->second->Owner.Alive() && now >= at->second->RetryAt)))
            { m_sources.erase(at); at = m_sources.end(); }
            if (at == m_sources.end())
            {
                if (!allowStart) return nullptr;
                if (m_sources.size() >= MaxSessions)
                {
                    auto oldest = m_sources.end();
                    for (auto candidate = m_sources.begin(); candidate != m_sources.end(); ++candidate)
                    {
                        if (!required && now - candidate->second->ReadAt < 500) continue;
                        if (oldest == m_sources.end() || candidate->second->Used < oldest->second->Used) oldest = candidate;
                    }
                    if (oldest == m_sources.end()) return nullptr;
                    m_sources.erase(oldest);
                }
                at = m_sources.emplace(item.Window, std::make_unique<Source>(item.Process)).first;
            }
            auto& source = *at->second;
            source.Used = now; if (required) source.ReadAt = now;
            if (!source.Owner.Alive())
            {
                source.Error = E_ACCESSDENIED;
                if (now >= source.RetryAt) source.RetryAt = now + 1000;
                return &source;
            }
            if (!source.Session.Window() && allowStart && now >= source.RetryAt)
            {
                source.Crops.clear();
                if (starts) ++*starts;
                try {
                    if constexpr (requires { source.Session.Start(item.Window, m_gpuDevice.get(), m_gpuContext.get(), m_frameWake.Subscribe()); })
                        source.Session.Start(item.Window, m_gpuDevice.get(), m_gpuContext.get(), m_frameWake.Subscribe());
                    else if constexpr (requires { source.Session.Start(item.Window, m_gpuDevice.get(), m_gpuContext.get()); })
                    {
                        if (m_gpuDevice && m_gpuContext) source.Session.Start(item.Window, m_gpuDevice.get(), m_gpuContext.get());
                        else source.Session.Start(item.Window);
                    }
                    else source.Session.Start(item.Window);
                    source.Error = S_OK;
                }
                catch (...)
                { source.Error = winrt::to_hresult(); source.Session.Close(); source.RetryAt = now + 1000; }
            }
            return &source;
        }
        static void Failed(Source& source)
        {
            source.Error = winrt::to_hresult(); source.Session.Close(); source.Crops.clear();
            source.RetryAt = Environment::Now() + 1000;
        }
    public:
        explicit BasicSceneCapturePool(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr)
        { m_gpuDevice.copy_from(device); m_gpuContext.copy_from(context); }
        ~BasicSceneCapturePool()
        {
            m_frameWake.Enable(false);
            m_sources.clear();
        }
        HANDLE FrameWakeHandle() const noexcept { return m_frameWake.Handle(); }
        void EnableFrameWake(bool enabled) noexcept { m_frameWake.Enable(enabled); }
        struct ReadResult
        {
            bool Ready = false;
            PixelFrame const* Frame = nullptr;
            HRESULT Error = E_PENDING;
        };
        struct GpuReadResult
        {
            bool Ready = false;
            WindowCapture::GpuFrame Frame;
            HRESULT Error = E_PENDING;
        };
        GpuReadResult ReadGpu(scene_detail::WindowSnapshot const& item, PixelRect requested, bool allowStart = true)
        {
            auto* source = Acquire(item, true, allowStart);
            if (!source) return {};
            if (!source->Session.Window()) return {false, {}, FAILED(source->Error) ? source->Error : E_PENDING};
            try
            {
                WindowCapture::GpuFrame frame;
                if (!source->Session.ReadGpu(requested, item.Rectangle, frame) || !Environment::Valid(item)) return {};
                if (!frame.View || !scene_detail::SameRect(frame.Region, scene_detail::Intersect(requested, item.Rectangle))) return {};
                return {true, std::move(frame), S_OK};
            }
            catch (...) { Failed(*source); return {false, {}, source->Error}; }
        }
        void Warm(PixelRect viewport, std::unordered_set<HWND> const& excluded)
        {
            m_excluded = excluded;
            const auto now = Environment::Now();
            std::erase_if(m_sources, [&](auto const& pair)
            { return (excluded.contains(pair.first)
                    && (!pair.second->ReadAt || now - pair.second->ReadAt > 500))
                || (!pair.second->Owner.Alive() && now >= pair.second->RetryAt)
                || now - pair.second->Used > 2000; });
            size_t starts = 0, considered = 0;
            // Discovery is independent of a hidden panel's old Z-order. Actual
            // visible target composition still uses LowerWindows below.
            for (auto const& item : Environment::VisibleWindows(viewport))
            {
                if (m_excluded.contains(item.Window)) continue;
                if (++considered > MaxSessions) break;
                auto* source = Acquire(item, false, !m_gpuDevice && starts < 1, &starts);
                if (!source) continue;
                try { if (source->Session.Window()) source->Session.DrainLatest(); }
                catch (...) { Failed(*source); }
            }
        }
        ReadResult Read(scene_detail::WindowSnapshot const& item, PixelRect requested)
        {
            using namespace scene_detail;
            auto* source = Acquire(item, true, true);
            if (!source) return {};
            if (!source->Session.Window()) return {false, nullptr, FAILED(source->Error) ? source->Error : E_PENDING};
            const auto region = Intersect(requested, item.Rectangle);
            if (Empty(region)) return {};
            try
            {
                source->Session.DrainLatest();
                const auto generation = source->Session.Generation();
                for (auto& crop : source->Crops)
                    if (generation && crop.Generation == generation && SameRect(crop.Region, region)
                        && SameRect(crop.Rectangle, item.Rectangle) && Environment::Valid(item))
                    { crop.Used = Environment::Now(); return {true, &crop.Pixels, S_OK}; }
                PixelFrame pixels;
                if (!source->Session.Read(region, pixels, true, true) || !Environment::Valid(item)) return {};
                const auto available = Intersect(pixels.Bounds, region);
                if (pixels.Pixels.empty() || !SameRect(region, available)) return {};
                const auto now = Environment::Now();
                TrimCrops(pixels.Pixels.size());
                if (source->Crops.size() >= MaxCrops)
                {
                    const auto oldest = std::min_element(source->Crops.begin(), source->Crops.end(),
                        [](auto const& a, auto const& b) { return a.Used < b.Used; });
                    source->Crops.erase(oldest);
                }
                source->Crops.push_back({region, item.Rectangle, std::move(pixels), source->Session.Generation(), now});
                return {true, &source->Crops.back().Pixels, S_OK};
            }
            catch (...) { Failed(*source); return {false, nullptr, source->Error}; }
        }
    };
    using SceneCapturePool = BasicSceneCapturePool<>;

    // Own and call this object on a single MTA worker. UI threads pass only an
    // HWND and physical pixel bounds; no XAML objects enter the capture worker.
    class SceneBackdropSource
    {
        std::shared_ptr<SceneCapturePool> m_pool;
        PixelFrame m_frame;
        PixelFrame m_scratch;
        ScenePlacement m_framePlacement = ScenePlacement::Actual;
        bool m_ready = false;
        bool m_gpuReady = false;
        PixelRect m_gpuBounds{};
        ScenePlacement m_gpuPlacement = ScenePlacement::Actual;
        std::vector<scene_detail::WindowSnapshot> m_gpuWindows;
        std::vector<uint32_t> m_gpuSources;
    public:
        explicit SceneBackdropSource(std::shared_ptr<SceneCapturePool> pool = {})
            : m_pool(pool ? std::move(pool) : std::make_shared<SceneCapturePool>()) {}
        SceneUpdate UpdateGpu(HWND excludedTopLevel, PixelRect paddedBounds, ScenePlacement placement,
            GpuSceneCompositor& compositor, bool profiling = false)
        {
            using namespace scene_detail;
            if (!IsWindow(excludedTopLevel)) return {false,false,E_INVALIDARG};
            excludedTopLevel = GetAncestor(excludedTopLevel,GA_ROOT);
            const int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
            paddedBounds=Intersect(paddedBounds,{left,top,left+GetSystemMetrics(SM_CXVIRTUALSCREEN),top+GetSystemMetrics(SM_CYVIRTUALSCREEN)});
            const int64_t width=static_cast<int64_t>(paddedBounds.Right)-paddedBounds.Left;
            const int64_t height=static_cast<int64_t>(paddedBounds.Bottom)-paddedBounds.Top;
            if(width<=0 || height<=0 || width>16384 || height>16384 || width*height>16*1024*1024)
                return {false,false,E_INVALIDARG};
            SceneProfile profile;profile.Enabled=profiling;
            double began=profiling?CaptureNowMilliseconds():0;
            const auto windows=BackdropWindows(excludedTopLevel,paddedBounds,placement);
            if(profiling){profile.DiscoveryMs=CaptureNowMilliseconds()-began;profile.Windows=static_cast<uint32_t>(windows.size());}
            std::vector<SceneCapturePool::GpuReadResult> captured(windows.size());
            const auto sameWindow=[](auto const& a,auto const& b){
                if(a.Window!=b.Window || a.Process!=b.Process || a.Opacity!=b.Opacity || a.ColorKey!=b.ColorKey
                    || !SameRect(a.Rectangle,b.Rectangle) || a.Region.size()!=b.Region.size())return false;
                for(size_t i=0;i<a.Region.size();++i)if(!SameRect(a.Region[i],b.Region[i]))return false;
                return true;
            };
            const bool sameScene=m_gpuReady && SameRect(m_gpuBounds,paddedBounds) && placement==m_gpuPlacement
                && windows.size()==m_gpuWindows.size() && std::equal(windows.begin(),windows.end(),m_gpuWindows.begin(),sameWindow);
            const auto read=[&](uint32_t index,bool allowStart){
                const double started=profiling?CaptureNowMilliseconds():0;
                captured[index]=m_pool->ReadGpu(windows[index],paddedBounds,allowStart);
                const double elapsed=profiling?CaptureNowMilliseconds()-started:0;
                if(profiling){profile.ReadMs+=elapsed;if(captured[index].Ready){
                    auto const& frame=captured[index].Frame;PixelFrame dimensions;
                    dimensions.Width=frame.CropTexels.Right-frame.CropTexels.Left;
                    dimensions.Height=frame.CropTexels.Bottom-frame.CropTexels.Top;dimensions.Bounds=frame.Region;
                    RecordSourceProfile(profile,dimensions,windows[index].Opacity,elapsed,0);}}
            };
            if(sameScene)for(auto index:m_gpuSources)if(index<windows.size())read(index,false);
            const auto failed=[&](HRESULT error){const bool changed=m_gpuReady;m_gpuReady=false;
                return SceneUpdate{false,changed,error,profile};};
            for(size_t pass=0;pass<=24;++pass)
            {
                began=profiling?CaptureNowMilliseconds():0;
                if(!compositor.Begin(paddedBounds))return failed(compositor.LastError());
                if(profiling)profile.AllocationMs+=CaptureNowMilliseconds()-began;
                began=profiling?CaptureNowMilliseconds():0;
                for(uint32_t index=0;index<windows.size();++index)
                {
                    auto const& item=windows[index];auto const& source=captured[index];
                    GpuSceneCompositor::Layer layer;
                    layer.View=source.Ready?source.Frame.View.get():nullptr;
                    layer.Region=source.Ready?source.Frame.Region:item.Rectangle;
                    layer.CropTexels=source.Ready?source.Frame.CropTexels:PixelRect{};
                    layer.Clips=&item.Region;layer.Opacity=item.Opacity;layer.ColorKey=item.ColorKey;layer.Index=index;
                    if(!compositor.Composite(layer))return failed(compositor.LastError());
                }
                if(profiling)profile.ComposeMs+=CaptureNowMilliseconds()-began;
                began=profiling?CaptureNowMilliseconds():0;
                const auto result=compositor.Finish(!m_gpuReady || placement!=m_gpuPlacement);
                if(profiling)profile.CompareMs+=CaptureNowMilliseconds()-began;
                if(result.Ready)
                {
                    m_gpuSources.clear();for(uint32_t i=0;i<captured.size();++i)if(captured[i].Ready)m_gpuSources.push_back(i);
                    m_gpuWindows=windows;m_gpuBounds=paddedBounds;m_gpuPlacement=placement;m_gpuReady=true;
                    return {true,result.Changed,S_OK,profile};
                }
                if(FAILED(result.Error) && result.Error!=E_PENDING)return failed(result.Error);
                const uint32_t needed=result.FirstNeededIndex;
                if(needed>=windows.size())return failed(E_PENDING);
                if(captured[needed].Ready)return failed(E_UNEXPECTED);
                read(needed,true);
                if(!captured[needed].Ready)return failed(captured[needed].Error);
            }
            return failed(E_NOTIMPL);
        }
        SceneUpdate Update(HWND excludedTopLevel, PixelRect paddedBounds,
            ScenePlacement placement = ScenePlacement::Actual, bool profiling = false)
        {
            using namespace scene_detail;
            if (!IsWindow(excludedTopLevel)) return {false, false, E_INVALIDARG};
            excludedTopLevel = GetAncestor(excludedTopLevel, GA_ROOT);
            // Padding beyond the virtual desktop has no real samples. The lens
            // renderer clamps to this actual capture extent at screen edges.
            const PixelRect desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            paddedBounds = Intersect(paddedBounds, desktop);
            const int64_t width = static_cast<int64_t>(paddedBounds.Right) - paddedBounds.Left;
            const int64_t height = static_cast<int64_t>(paddedBounds.Bottom) - paddedBounds.Top;
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || width * height > 16 * 1024 * 1024)
                return {false, false, E_INVALIDARG};
            SceneProfile profile; profile.Enabled = profiling;
            double phaseStarted = profiling ? CaptureNowMilliseconds() : 0;
            SceneAccumulator scene(paddedBounds, std::move(m_scratch));
            if (profiling) profile.AllocationMs = CaptureNowMilliseconds() - phaseStarted;
            HRESULT error = S_OK;
            bool missing = false;
            phaseStarted = profiling ? CaptureNowMilliseconds() : 0;
            const auto windows = BackdropWindows(excludedTopLevel, paddedBounds, placement);
            if (profiling) { profile.DiscoveryMs = CaptureNowMilliseconds() - phaseStarted;
                profile.Windows = static_cast<uint32_t>(windows.size()); }
            for (auto const& item : windows)
            {
                if (scene.Complete()) break;
                phaseStarted = profiling ? CaptureNowMilliseconds() : 0;
                const bool needed = scene.Needs(item.Rectangle) && (item.Region.empty()
                    || std::any_of(item.Region.begin(), item.Region.end(),
                        [&](PixelRect clip) { return scene.Needs(Intersect(clip, item.Rectangle)); }));
                if (profiling) profile.CoverageMs += CaptureNowMilliseconds() - phaseStarted;
                if (!needed) continue;
                phaseStarted = profiling ? CaptureNowMilliseconds() : 0;
                const auto captured = m_pool->Read(item, paddedBounds);
                const double readMs = profiling ? CaptureNowMilliseconds() - phaseStarted : 0;
                if (profiling) profile.ReadMs += readMs;
                if (!captured.Ready)
                { missing = true; if (SUCCEEDED(error) || error == E_PENDING) error = captured.Error; continue; }
                phaseStarted = profiling ? CaptureNowMilliseconds() : 0;
                const bool composed = scene.Composite(*captured.Frame, item.Region, item.Opacity, item.ColorKey);
                const double composeMs = profiling ? CaptureNowMilliseconds() - phaseStarted : 0;
                if (profiling) profile.ComposeMs += composeMs;
                RecordSourceProfile(profile, *captured.Frame, item.Opacity, readMs, composeMs);
                if (!composed)
                { missing = true; if (SUCCEEDED(error)) error = E_UNEXPECTED; }
            }
            const bool ready = !missing && scene.Complete();
            if (!ready)
            { const bool changed = m_ready; m_ready = false; m_scratch = scene.TakeFrame();
                return {false, changed, FAILED(error) ? error : E_PENDING, profile}; }
            phaseStarted = profiling ? CaptureNowMilliseconds() : 0;
            auto next = scene.TakeFrame();
            const bool changed = !m_ready || placement != m_framePlacement
                || !SameRect(next.Bounds, m_frame.Bounds) || next.Pixels != m_frame.Pixels;
            m_scratch = std::move(m_frame); m_frame = std::move(next); m_framePlacement = placement; m_ready = true;
            if (profiling) profile.CompareMs = CaptureNowMilliseconds() - phaseStarted;
            return {true, changed, S_OK, profile};
        }
        PixelFrame const& Frame() const { return m_frame; }
    };
}
