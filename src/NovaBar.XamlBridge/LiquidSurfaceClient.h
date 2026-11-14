#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#undef GetCurrentTime
#include <CoreWindow.h>
#include <robuffer.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include "../NovaBar.Capture/CaptureFrameReader.h"
#include "../NovaBar.Capture/ClientCanvasOptics.h"
#include "GpuLiquidSurface.h"

namespace novabar
{
    namespace client_detail
    {
        using Brush = winrt::Windows::UI::Xaml::Media::Brush;
        class PresentationDriver
        {
            bool m_active = false, m_entered = false, m_haveFrame = false;
            int64_t m_frame = 0;
        public:
            void Start() { m_active = true; m_haveFrame = false; }
            void Stop() { m_active = false; }
            bool Enter(bool rendering, int64_t frame = 0)
            {
                if (!m_active || m_entered || (rendering && m_haveFrame && frame == m_frame)) return false;
                m_entered = true;
                if (rendering) { m_frame = frame; m_haveFrame = true; }
                return true;
            }
            void Leave() { m_entered = false; }
        };
        inline int SourceInterval(bool panel, bool visible) { return panel ? (visible ? 8 : 100) : 16; }
        struct SurfaceVisibility
        {
            bool Loaded = false, ElementVisible = false, CoordinateVisible = false, TopVisible = false;
            bool Iconic = false, Cloaked = false, CloakKnown = false, Owned = false, Resolved = false;
            int HostVisible = -1, WindowVisible = -1;
        };
        inline bool EffectiveVisibility(SurfaceVisibility const& state)
        {
            return state.Loaded && state.ElementVisible && state.CoordinateVisible && state.TopVisible && state.Owned
                && state.CloakKnown && !state.Cloaked && !state.Iconic;
        }
        struct PresentationWork { bool Present, Source; };
        inline PresentationWork SelectPresentationWork(bool sameEpoch, bool published, bool visibilityChanged)
        {
            return {sameEpoch && published, !sameEpoch || !published || visibilityChanged};
        }
        struct TimingSamples
        {
            uint64_t Count = 0;
            double Total = 0, Minimum = 0, Maximum = 0;
            void Add(double value)
            {
                Minimum = Count ? std::min(Minimum, value) : value;
                ++Count; Total += value; Maximum = std::max(Maximum, value);
            }
            double Mean() const { return Count ? Total / Count : 0; }
        };
        struct SurfaceLayout
        {
            uintptr_t Root = 0;
            double Width = 0, Height = 0, Raster = 1, CornerRadius = 0;
        };
        inline bool SameRect(PixelRect const& a, PixelRect const& b)
        {
            return a.Left == b.Left && a.Top == b.Top && a.Right == b.Right && a.Bottom == b.Bottom;
        }
        inline bool CompatibleBaseCanvas(capture::Request const& request, capture::Frame const& frame)
        {
            uint32_t width = 0, height = 0;
            return (request.Flags & capture::ClientOptics) && frame.Status == capture::Status::Ready
                && frame.PayloadFlags == capture::BaseCanvasPayload && !frame.MotionSequence
                && SameRect(frame.RenderedBounds, {}) && frame.Revision == request.Revision
                && capture::detail::Dimensions(request, width, height) && frame.Width == width && frame.Height == height
                && SameRect(frame.BitmapBounds, capture::detail::CanvasBounds(request));
        }
        inline bool CanRetainBaseCanvas(bool ready, uint64_t publishedTick, uint64_t now)
        {
            return ready && capture::detail::Fresh(publishedTick, now);
        }
        inline bool CompatibleGpuBaseCanvas(capture::Request const& request, capture::Frame const& frame,
                                            uint64_t now = GetTickCount64())
        {
            uint32_t width = 0, height = 0;
            return (request.Flags & capture::GpuBase) && (request.Flags & capture::ClientOptics)
                && gpu_surface_detail::Admissible(frame, request.Revision, frame.Width, frame.Height, now)
                && capture::detail::Dimensions(request, width, height) && frame.Width == width && frame.Height == height
                && SameRect(frame.BitmapBounds, capture::detail::CanvasBounds(request));
        }
        class CurrentCanvasOptics
        {
            PixelFrame m_base;
            std::vector<uint8_t> m_pixels;
            capture::Request m_pose;
            PixelRect m_viewport;
            uint64_t m_revision = 0, m_serial = 0;
            bool m_rendered = false;
        public:
            void Clear()
            {
                m_base = {}; m_pixels.clear(); m_revision = m_serial = 0; m_rendered = false;
            }
            void SetBase(PixelFrame base, uint64_t revision, uint64_t serial)
            {
                m_base = std::move(base); m_revision = revision; m_serial = serial; m_rendered = false;
            }
            bool HasBase() const { return !m_base.Pixels.empty(); }
            PixelFrame const& Base() const { return m_base; }
            std::vector<uint8_t> const& Pixels() const { return m_pixels; }
            uint64_t Serial() const { return m_serial; }
            bool Apply(capture::Request const& current, PixelRect const& viewport, bool& changed)
            {
                changed = false;
                if (!HasBase() || current.Revision != m_revision
                    || !SameRect(m_base.Bounds, capture::detail::CanvasBounds(current))) return false;
                if (m_rendered && SameRect(current.Bounds, m_pose.Bounds) && current.Radius == m_pose.Radius
                    && current.Material.Lens == m_pose.Material.Lens && current.Material.Dispersion == m_pose.Material.Dispersion
                    && current.Specular == m_pose.Specular && SameRect(viewport, m_viewport)) return true;
                if (!RenderClientCanvasOptics(m_base, current.Bounds, current.Radius, current.Material.Lens,
                    current.Material.Dispersion, current.Specular, viewport, m_pixels)) return false;
                m_pose = current; m_viewport = viewport; m_rendered = true; changed = true;
                return true;
            }
        };
        inline bool CanPrepareGeometry(bool loaded, double width, double height, bool panel)
        {
            return (loaded || panel) && std::isfinite(width) && std::isfinite(height)
                && width > 0 && height > 0 && width <= 16384 && height <= 16384;
        }
        inline PixelRect StableCanvas(PixelRect const& nativeHost, PixelRect const& viewport,
                                      double layoutLeft, double layoutWidth, bool layoutUsable)
        {
            if (viewport.Right <= viewport.Left || viewport.Bottom <= viewport.Top
                || nativeHost.Right <= nativeHost.Left || nativeHost.Bottom <= nativeHost.Top) return {};
            double left = nativeHost.Left, right = nativeHost.Right;
            if (static_cast<int64_t>(nativeHost.Right) - nativeHost.Left
                < (static_cast<int64_t>(viewport.Right) - viewport.Left) / 2)
            { left -= 64; right += 64; }
            else if (layoutUsable && std::isfinite(layoutLeft) && std::isfinite(layoutWidth) && layoutWidth > 0)
            { left = std::floor(layoutLeft) - 128; right = std::ceil(layoutLeft + layoutWidth) + 128; }
            left = std::clamp(left, static_cast<double>(viewport.Left), static_cast<double>(viewport.Right));
            right = std::clamp(right, static_cast<double>(viewport.Left), static_cast<double>(viewport.Right));
            if (right <= left)
            {
                left = std::max(nativeHost.Left, viewport.Left);
                right = std::min(nativeHost.Right, viewport.Right);
            }
            return {static_cast<int>(left), viewport.Top, static_cast<int>(right), viewport.Bottom};
        }
        inline winrt::Windows::UI::Xaml::Media::Matrix CanvasBrushMatrix(PixelRect const& canvas, PixelRect const& material)
        {
            const double width = static_cast<double>(material.Right) - material.Left;
            const double height = static_cast<double>(material.Bottom) - material.Top;
            if (width <= 0 || height <= 0) return {1, 0, 0, 1, 0, 0};
            return {(static_cast<double>(canvas.Right) - canvas.Left) / width, 0, 0,
                (static_cast<double>(canvas.Bottom) - canvas.Top) / height,
                (static_cast<double>(canvas.Left) - material.Left) / width,
                (static_cast<double>(canvas.Top) - material.Top) / height};
        }
        inline winrt::Windows::UI::Xaml::Media::Matrix CanvasBrushAbsoluteMatrix(PixelRect const& canvas,
            PixelRect const& material, double widthDip, double heightDip)
        {
            if (!std::isfinite(widthDip) || !std::isfinite(heightDip) || widthDip <= 0 || heightDip <= 0)
                return {1, 0, 0, 1, 0, 0};
            // TileBrush first stretches the image into the painted DIP area.
            // Brush.Transform then maps that local output into the world crop.
            auto matrix = CanvasBrushMatrix(canvas, material);
            matrix.OffsetX *= widthDip;
            matrix.OffsetY *= heightDip;
            return matrix;
        }
        inline bool SameSurfaceEpoch(capture::Request const& a, capture::Request const& b,
                                     SurfaceLayout const& layoutA, SurfaceLayout const& layoutB)
        {
            const bool canvas = (a.Flags & capture::WorldCanvas) != 0;
            return a.CoordinateHwnd == b.CoordinateHwnd && a.TopLevelHwnd == b.TopLevelHwnd
                && a.Kind == b.Kind && (a.Flags & (capture::HostClipsCorners | capture::WorldCanvas | capture::ClientOptics | capture::GpuBase))
                    == (b.Flags & (capture::HostClipsCorners | capture::WorldCanvas | capture::ClientOptics | capture::GpuBase))
                && layoutA.Root == layoutB.Root && layoutA.Raster == layoutB.Raster
                && (canvas ? SameRect(capture::detail::CanvasBounds(a), capture::detail::CanvasBounds(b))
                    : layoutA.Width == layoutB.Width && layoutA.Height == layoutB.Height
                        && layoutA.CornerRadius == layoutB.CornerRadius)
                && a.Material.Lens == b.Material.Lens && a.Material.Dispersion == b.Material.Dispersion
                && a.Material.Noise == b.Material.Noise && a.Material.Tint == b.Material.Tint
                && a.Material.Blur == b.Material.Blur && a.Material.Color == b.Material.Color && a.Specular == b.Specular;
        }
        enum class SurfaceActivity { Waiting, Visible, HiddenWarm, Stop };
        inline SurfaceActivity Activity(bool panel, bool hasGeometry, bool visible, bool stopped = false)
        {
            if (stopped) return SurfaceActivity::Stop;
            if (!hasGeometry) return SurfaceActivity::Waiting;
            if (visible) return SurfaceActivity::Visible;
            return panel ? SurfaceActivity::HiddenWarm : SurfaceActivity::Stop;
        }
        inline bool CanReuseHiddenGeometry(bool loaded, double width, double height, bool hostVisible)
        {
            return !loaded || width <= 0 || height <= 0 || !hostVisible;
        }
        inline bool CanRetainPresentation(bool ready, bool visible, bool currentBounds,
                                          uint64_t publishedTick, uint64_t motionTick, uint64_t now)
        {
            if (!ready) return false;
            if (!visible) return true;
            if (!publishedTick || publishedTick > now || now - publishedTick > 200) return false;
            return currentBounds || (motionTick && motionTick <= now && now - motionTick <= 200);
        }
        struct PendingCanvasField
        {
            uint64_t Tick = 0;
            void Observe(bool visible, bool sameField, uint64_t now)
            {
                if (!visible || sameField) Tick = 0;
                else if (!Tick) Tick = now;
            }
        };
        inline bool CanRetainCanvasPresentation(bool ready, bool visible, bool /*currentBounds*/,
            uint64_t publishedTick, uint64_t /*motionTick*/, uint64_t /*pendingFieldTick*/, uint64_t now)
        {
            if (!ready) return false;
            if (!visible) return true;
            // A world canvas contains the complete sampled scene. An older
            // lens field remains truthful metadata, but cannot invalidate
            // those pixels as the native pane moves over the same canvas.
            // Use the same source heartbeat contract as FrameReader; the
            // independent request-write failure grace remains 200 ms.
            return capture::detail::Fresh(publishedTick, now);
        }
        class PublicationGeneration
        {
            uint64_t m_value = 0;
        public:
            uint64_t Capture() const { return m_value; }
            bool Current(uint64_t value) const { return value == m_value; }
            void Invalidate() { ++m_value; }
        };
        inline HRESULT DiagnosticHresult() noexcept
        {
            try { throw; }
            catch (winrt::hresult_error const& error) { return error.code(); }
            catch (...) { return E_FAIL; }
        }
        enum class RequestWriteFailure { Retry, Cancel };
        inline bool NeedsMotionRegistration(bool epochChanged, bool published, uint64_t revision,
                                            uint64_t publishedRevision, bool samePose)
        {
            return epochChanged || !published || revision != publishedRevision || !samePose;
        }
        struct RequestWriteState
        {
            uint64_t LastSuccessTick = 0;
            bool RetryPending = false;
            void Succeeded(uint64_t tick) { LastSuccessTick = tick; RetryPending = false; }
            RequestWriteFailure Failed(DWORD error)
            {
                if (error == ERROR_INVALID_DATA || error == ERROR_INVALID_PARAMETER) return RequestWriteFailure::Cancel;
                RetryPending = true;
                return RequestWriteFailure::Retry;
            }
            bool CanPresent(bool visible, uint64_t now) const
            {
                return !visible || !RetryPending || (LastSuccessTick && LastSuccessTick <= now && now - LastSuccessTick <= 200);
            }
        };
        inline bool Notify(std::function<void(Brush const&, bool)> const& callback, Brush const& brush, bool ready) noexcept
        {
            try { if (callback) callback(brush, ready); return true; }
            catch (...) { return false; }
        }
        enum class NotificationResult { Accepted, Failed, Superseded };
        inline NotificationResult NotifyCurrent(PublicationGeneration const& publication,
            std::function<void(Brush const&, bool)> const& callback, Brush const& brush, bool ready) noexcept
        {
            const auto generation = publication.Capture();
            const bool accepted = Notify(callback, brush, ready);
            if (!publication.Current(generation)) return NotificationResult::Superseded;
            return accepted ? NotificationResult::Accepted : NotificationResult::Failed;
        }
    }
    struct LiquidAppearance
    {
        GlassOptions Material;
        double Specular = .35;
    };

    // This class only runs on the element's UI thread. NovaBar.Capture.exe
    // captures the scene; the shell presents its CPU or shared GPU canvas.
    class LiquidSurfaceClient : public std::enable_shared_from_this<LiquidSurfaceClient>
    {
    public:
        using Brush = winrt::Windows::UI::Xaml::Media::Brush;
        using Changed = std::function<void(Brush const&, bool)>;
        using WindowResolver = std::function<HWND(winrt::Windows::UI::Xaml::FrameworkElement const&)>;
        LiquidSurfaceClient(winrt::Windows::UI::Xaml::FrameworkElement const& element,
                            std::wstring directory, capture::Kind kind, HWND taskbar, Changed changed, WindowResolver resolver = {})
            : m_element(winrt::make_weak(element)), m_directory(std::move(directory)), m_kind(kind),
              m_taskbar(taskbar), m_changed(std::move(changed)), m_resolver(std::move(resolver))
        {
            static std::atomic<uint64_t> next{capture::gpu::RandomNonce()};
            m_id = ++next;
            m_clientEpoch = capture::gpu::RandomNonce();
            m_brush = Transparent();
        }
        ~LiquidSurfaceClient() { Stop(L"destructor"); }
        Brush CurrentBrush() const { return m_brush; }
        bool Ready() const { return m_ready; }
        void SetAppearance(LiquidAppearance const& appearance)
        {
            m_publication.Invalidate();
            m_appearance = appearance;
            m_driver.Start();
            if (!m_timer)
            {
                m_timer = winrt::Windows::UI::Xaml::DispatcherTimer();
                m_timer.Interval(std::chrono::milliseconds(client_detail::SourceInterval(m_kind == capture::Kind::Panel, false)));
                m_tick = m_timer.Tick([weak = weak_from_this()](auto const&, auto const&)
                {
                    if (auto self = weak.lock()) self->Tick();
                });
                m_timer.Start();
            }
            if (m_kind == capture::Kind::Panel && !m_rendering.value)
            {
                m_rendering = winrt::Windows::UI::Xaml::Media::CompositionTarget::Rendering(
                    [weak = weak_from_this()](auto const&, winrt::Windows::Foundation::IInspectable const& args)
                    {
                        if (auto self = weak.lock())
                        {
                            auto rendering = args.as<winrt::Windows::UI::Xaml::Media::RenderingEventArgs>();
                            self->Present(rendering.RenderingTime().count());
                        }
                    });
            }
            Tick();
        }
        void Stop(wchar_t const* reason = L"external-stop") noexcept
        {
            TraceLifecycle(L"stop", reason);
            m_publication.Invalidate();
            m_driver.Stop();
            try
            {
                if (m_rendering.value)
                {
                    winrt::Windows::UI::Xaml::Media::CompositionTarget::Rendering(m_rendering);
                    m_rendering = {};
                }
            }
            catch (...) { TraceLifecycle(L"stop-exception", L"stop-rendering", client_detail::DiagnosticHresult()); }
            try { if (m_timer) { m_timer.Stop(); m_timer.Tick(m_tick); m_timer = nullptr; } }
            catch (...) { TraceLifecycle(L"stop-exception", L"stop-timer", client_detail::DiagnosticHresult()); }
            try
            {
                if (client_detail::Activity(m_kind == capture::Kind::Panel, m_hasGeometry, false, true)
                    == client_detail::SurfaceActivity::Stop)
                    capture::CancelRequest(m_directory, GetCurrentProcessId(), m_id);
            }
            catch (...) { TraceLifecycle(L"stop-exception", L"stop-cancel-request", client_detail::DiagnosticHresult()); }
            m_reader.Close(); m_published = false; m_ready = false;
            m_hasGeometry = false; m_motions.clear(); m_root = {};
            m_currentOptics.Clear();
            m_gpuSurface.Reset(); m_gpuAttached = false;
            m_bitmap = nullptr; m_brushTransform = nullptr; m_brush = nullptr;
        }

    private:
        static Brush Transparent()
        {
            winrt::Windows::UI::Xaml::Media::SolidColorBrush brush;
            brush.Color({0, 0, 0, 0});
            return brush;
        }
        static bool SameMotion(capture::Request const& a, capture::Request const& b)
        {
            return client_detail::SameRect(a.Bounds, b.Bounds) && a.Radius == b.Radius && a.Scale == b.Scale;
        }
        static bool OwnsWindows(capture::Request const& request)
        {
            const HWND coordinate = reinterpret_cast<HWND>(request.CoordinateHwnd);
            const HWND top = reinterpret_cast<HWND>(request.TopLevelHwnd);
            DWORD coordinatePid = 0, topPid = 0;
            return coordinate && top && IsWindow(coordinate) && IsWindow(top)
                && GetAncestor(coordinate, GA_ROOT) == top
                && GetWindowThreadProcessId(coordinate, &coordinatePid)
                && GetWindowThreadProcessId(top, &topPid)
                && coordinatePid == GetCurrentProcessId() && topPid == GetCurrentProcessId();
        }
        static bool LayoutLeft(winrt::Windows::UI::Xaml::FrameworkElement const& element, double& left) noexcept
        {
            try
            {
                left = 0;
                winrt::Windows::UI::Xaml::DependencyObject node = element;
                unsigned depth = 0;
                while (node && ++depth <= 128)
                {
                    if (auto visual = node.try_as<winrt::Windows::UI::Xaml::UIElement>())
                    {
                        const auto offset = visual.ActualOffset();
                        if (!std::isfinite(offset.x)) return false;
                        left += offset.x;
                    }
                    node = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(node);
                }
                return !node && std::isfinite(left);
            }
            catch (...) { return false; }
        }
        bool Geometry(winrt::Windows::UI::Xaml::FrameworkElement const& element, capture::Request& request,
                      client_detail::SurfaceLayout& layout,
                      winrt::Windows::Foundation::IInspectable& root, bool& visible, wchar_t const*& reason)
        {
            using namespace winrt::Windows::UI;
            m_visibility = {};
            m_visibility.Loaded = element.IsLoaded();
            m_visibility.ElementVisible = element.Visibility() == Xaml::Visibility::Visible;
            reason = L"read-actual-size";
            layout.Width = element.ActualWidth(); layout.Height = element.ActualHeight();
            if (!client_detail::CanPrepareGeometry(element.IsLoaded(), layout.Width, layout.Height, m_kind == capture::Kind::Panel))
            {
                reason = !std::isfinite(layout.Width) || !std::isfinite(layout.Height) ? L"nonfinite-layout-size"
                    : layout.Width <= 0 || layout.Height <= 0 ? L"unmeasured-layout-size" : L"oversize-layout";
                return false;
            }
            reason = L"read-element-visibility-root";
            visible = m_visibility.Loaded && m_visibility.ElementVisible;
            root = element.XamlRoot();
            if (auto xamlRoot = element.XamlRoot()) m_visibility.HostVisible = xamlRoot.IsHostVisible() ? 1 : 0;
            if (!root)
            {
                Xaml::DependencyObject node = element;
                while (auto parent = Xaml::Media::VisualTreeHelper::GetParent(node)) node = parent;
                root = node;
            }
            layout.Root = reinterpret_cast<uintptr_t>(root.as<::IUnknown>().get());
            reason = L"resolve-coordinate-window";
            HWND coordinate = m_taskbar;
            if (m_kind == capture::Kind::Taskbar)
            {
                RECT rect{};
                if (!coordinate || !IsWindowVisible(coordinate) || !GetWindowRect(coordinate, &rect))
                { reason = L"taskbar-window-unavailable"; return false; }
                request.Bounds = {rect.left, rect.top, rect.right, rect.bottom};
                request.Scale = 1; request.Radius = 0;
                layout.Raster = 1;
            }
            else
            {
                coordinate = m_resolver ? m_resolver(element) : nullptr;
                m_visibility.Resolved = coordinate != nullptr;
                if (!coordinate)
                {
                    auto window = Xaml::Window::Current();
                    auto core = window ? window.CoreWindow() : Core::CoreWindow::GetForCurrentThread();
                    if (!core) { reason = L"core-window-unavailable"; return false; }
                    if (window) m_visibility.WindowVisible = window.Visible() ? 1 : 0;
                    auto interop = core.as<::ICoreWindowInterop>();
                    winrt::check_hresult(interop->get_WindowHandle(&coordinate));
                }
                POINT origin{};
                reason = L"coordinate-client-to-screen";
                if (!coordinate || !ClientToScreen(coordinate, &origin))
                { reason = L"client-to-screen-unavailable"; return false; }
                reason = L"transform-to-root";
                const auto bounds = element.TransformToVisual(nullptr).TransformBounds(
                    {0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight())});
                const double raster = element.XamlRoot() ? element.XamlRoot().RasterizationScale() : GetDpiForWindow(coordinate) / 96.0;
                if (!std::isfinite(raster) || raster <= 0 || raster > 8)
                { reason = L"invalid-raster-scale"; return false; }
                layout.Raster = raster;
                request.Bounds = {origin.x + static_cast<int>(std::floor(bounds.X * raster)),
                    origin.y + static_cast<int>(std::floor(bounds.Y * raster)),
                    origin.x + static_cast<int>(std::ceil((bounds.X + bounds.Width) * raster)),
                    origin.y + static_cast<int>(std::ceil((bounds.Y + bounds.Height) * raster))};
                const double visualScale = std::min(bounds.Width / layout.Width, bounds.Height / layout.Height);
                if (!std::isfinite(visualScale) || visualScale <= 0)
                { reason = L"invalid-or-zero-visual-scale"; return false; }
                reason = L"read-corner-radius";
                request.Scale = std::clamp(raster * visualScale, .25, 8.0);
                request.OutputWidth = static_cast<uint32_t>(std::ceil(layout.Width));
                request.OutputHeight = static_cast<uint32_t>(std::ceil(layout.Height));
                double radius = 0;
                if (auto border = element.try_as<Xaml::Controls::Border>()) radius = border.CornerRadius().TopLeft;
                else if (auto grid = element.try_as<Xaml::Controls::Grid>()) radius = grid.CornerRadius().TopLeft;
                // XAML clips a Border/Grid background to its own corners;
                // rectangular island content is clipped by its native host.
                // Keep optics in the bitmap, without multiplying that mask.
                request.Flags |= capture::HostClipsCorners;
                layout.CornerRadius = radius > 0 ? radius : 12;
                request.Radius = std::min(1024.0, layout.CornerRadius * raster * visualScale);
                if (m_kind == capture::Kind::Panel)
                {
                    request.Flags |= capture::ClientOptics;
                    if (m_gpuEnabled) request.Flags |= capture::GpuBase;
                    RECT client{};
                    reason = L"native-client-bounds";
                    if (!GetClientRect(coordinate, &client)) return false;
                    const PixelRect nativeHost{origin.x + client.left, origin.y + client.top,
                        origin.x + client.right, origin.y + client.bottom};
                    const int desktopLeft = GetSystemMetrics(SM_XVIRTUALSCREEN), desktopTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    const PixelRect viewport{desktopLeft, desktopTop, desktopLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                        desktopTop + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
                    double layoutLeft = 0;
                    const bool layoutUsable = LayoutLeft(element, layoutLeft);
                    const auto canvas = client_detail::StableCanvas(nativeHost, viewport,
                        origin.x + layoutLeft * raster, layout.Width * raster, layoutUsable);
                    // Canvas resolution follows the base DPI, so native open
                    // transforms only move the crop and the material field.
                    request.Scale = raster;
                    reason = L"world-canvas-bounds";
                    if (!capture::detail::SetCanvasBounds(request, canvas)) return false;
                }
            }
            reason = L"resolve-top-level-window";
            const HWND top = GetAncestor(coordinate, GA_ROOT);
            if (!top || !IsWindow(top)) { reason = L"top-level-window-unavailable"; return false; }
            request.CoordinateHwnd = reinterpret_cast<uint64_t>(coordinate);
            request.TopLevelHwnd = reinterpret_cast<uint64_t>(top);
            const bool owned = OwnsWindows(request);
            m_visibility.Owned = owned;
            m_visibility.CoordinateVisible = IsWindowVisible(coordinate) != FALSE;
            m_visibility.TopVisible = IsWindowVisible(top) != FALSE;
            m_visibility.Iconic = IsIconic(top) != FALSE;
            if (m_kind == capture::Kind::Panel)
            {
                DWORD cloaked = 0;
                m_visibility.CloakKnown = SUCCEEDED(DwmGetWindowAttribute(top, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
                m_visibility.Cloaked = cloaked != 0;
                // Notification's CoreWindow/XamlRoot visibility can remain false
                // through its first real native presentation. DWM cloaking is
                // the actual closed-popup boundary even while WS_VISIBLE stays set.
                visible = client_detail::EffectiveVisibility(m_visibility);
            }
            else visible = visible && m_visibility.CoordinateVisible && m_visibility.TopVisible && owned;
            reason = owned ? L"geometry-ready" : L"current-window-owner-invalid";
            return owned;
        }
        void SetUnavailable(bool notify)
        {
            const bool wasReady = m_ready || m_gpuAttached || (m_brush && m_brush.try_as<winrt::Windows::UI::Xaml::Media::ImageBrush>());
            // Discard the consumed serial with its bitmap: a resumed static
            // scene renews its heartbeat without changing the frame serial.
            m_reader.Close();
            m_ready = false;
            m_currentOptics.Clear();
            m_gpuSurface.ClearSource(); m_gpuAttached = false;
            m_bitmap = nullptr; m_brushTransform = nullptr;
            m_lastFreshFrame = m_presentedMotion = m_presentedMotionTick = 0;
            m_presentedBounds = {}; m_bitmapBounds = {};
            if (wasReady)
            {
                m_brush = Transparent();
                if (notify) client_detail::Notify(m_changed, m_brush, false);
            }
        }
        struct Motion
        {
            uint64_t Sequence = 0, Tick = 0;
            PixelRect Bounds;
            double Scale = 1, Radius = 0;
        };
        void CancelCapture(wchar_t const* reason, HRESULT error = S_OK)
        {
            TraceLifecycle(L"cancel", reason, error);
            if (m_published) capture::CancelRequest(m_directory, GetCurrentProcessId(), m_id);
            m_published = false; m_hasGeometry = false; m_motions.clear(); m_root = {};
            m_requestWrites = {}; m_pendingField = {};
            m_gpuSurface.Reset();
            SetUnavailable(true);
        }
        bool SamePresentedField(capture::Request const& current) const
        {
            return client_detail::SameRect(m_presentedBounds, current.Bounds)
                && m_presentedScale == current.Scale && m_presentedRadius == current.Radius;
        }
        bool CanRetain(bool ready, bool visible, bool sameField, uint64_t publishedTick, uint64_t motionTick,
                       capture::Request const& current, uint64_t now) const
        {
            if (current.Flags & capture::ClientOptics)
                return client_detail::CanRetainBaseCanvas(ready, publishedTick, now);
            return current.Flags & capture::WorldCanvas
                ? client_detail::CanRetainCanvasPresentation(ready, visible, sameField, publishedTick, motionTick, m_pendingField.Tick, now)
                : client_detail::CanRetainPresentation(ready, visible, sameField, publishedTick, motionTick, now);
        }
        void UpdateBrushTransform(capture::Request const& current)
        {
            if (current.Flags & capture::GpuBase) return;
            if (!(current.Flags & capture::WorldCanvas) || m_bitmapBounds.Right <= m_bitmapBounds.Left
                || m_bitmapBounds.Bottom <= m_bitmapBounds.Top) return;
            if (!m_brushTransform) return;
            m_brushTransform.Matrix(client_detail::CanvasBrushAbsoluteMatrix(m_bitmapBounds, current.Bounds,
                m_layout.Width, m_layout.Height));
        }
        void UploadPixels(capture::Request const& current, uint32_t width, uint32_t height,
                          PixelRect const& bitmapBounds, std::vector<uint8_t> const& source)
        {
            const double began = PerformanceNow();
            using namespace winrt::Windows::UI::Xaml;
            if (!m_bitmap || m_bitmap.PixelWidth() != static_cast<int>(width) || m_bitmap.PixelHeight() != static_cast<int>(height))
            {
                m_bitmap = Media::Imaging::WriteableBitmap(width, height);
                Media::ImageBrush image; image.ImageSource(m_bitmap); image.Stretch(Media::Stretch::Fill);
                m_brushTransform = Media::MatrixTransform(); image.Transform(m_brushTransform); m_brush = image;
            }
            m_bitmapBounds = bitmapBounds;
            UpdateBrushTransform(current);
            auto access = m_bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
            uint8_t* pixels = nullptr; winrt::check_hresult(access->Buffer(&pixels));
            memcpy(pixels, source.data(), source.size()); m_bitmap.Invalidate();
            m_uploadTiming.Add(PerformanceNow() - began);
        }
        void ApplyCurrentOptics(capture::Request const& current, uint64_t now)
        {
            if (current.Flags & capture::GpuBase) { ApplyGpuOptics(current, now); return; }
            if (!m_currentOptics.HasBase()) return;
            const uint64_t publication = m_publication.Capture();
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const PixelRect viewport{left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            bool changed = false;
            const double began = PerformanceNow();
            if (!m_currentOptics.Apply(current, viewport, changed))
            {
                SetUnavailable(true); ClientStatus(L"current-optics-unavailable", &current); return;
            }
            if (!changed) return;
            m_opticsTiming.Add(PerformanceNow() - began);
            auto const& base = m_currentOptics.Base();
            UploadPixels(current, base.Width, base.Height, base.Bounds, m_currentOptics.Pixels());
            const bool first = !m_ready;
            m_ready = true;
            // The producer owns no material pose in this mode. These bounds
            // and this tick describe the field just rendered on the UI thread.
            m_presentedMotion = 0; m_presentedMotionTick = now;
            m_presentedBounds = current.Bounds; m_presentedScale = current.Scale; m_presentedRadius = current.Radius;
            ++m_localOpticsFrame;
            const bool accepted = client_detail::Notify(m_changed, m_brush, true);
            if (!m_publication.Current(publication)) return;
            if (!accepted)
            {
                SetUnavailable(true); ClientStatus(L"presentation-failed", &current); return;
            }
            if (first) ClientStatus(L"ready-real-scene", &current);
        }
        bool AttachGpuBrush()
        {
            const auto brush = m_gpuSurface.CurrentBrush();
            if (!brush) return false;
            if (!m_gpuAttached || m_brush != brush)
            {
                m_brush = brush; m_gpuAttached = true;
                // Connect before waiting for readiness: the native brush's
                // OnConnected callback is part of its readiness contract.
                const auto result = client_detail::NotifyCurrent(m_publication, m_changed, m_brush, false);
                if (result == client_detail::NotificationResult::Superseded) return false;
                if (result == client_detail::NotificationResult::Failed) { SetUnavailable(true); return false; }
            }
            return true;
        }
        void ApplyGpuOptics(capture::Request const& current, uint64_t now)
        {
            const auto publication = m_publication.Capture();
            m_gpuSurface.PollPending(now);
            if (!AttachGpuBrush()) return;
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const PixelRect viewport{left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            const bool ready = m_gpuSurface.Present(current, m_layout.Width, m_layout.Height, viewport, now);
            if (!m_publication.Current(publication)) return;
            m_ready = ready;
            m_lastFreshFrame = m_gpuSurface.PublishedTick();
            if (m_ready)
            {
                m_presentedMotion = 0; m_presentedMotionTick = now;
                m_presentedBounds = current.Bounds; m_presentedScale = current.Scale; m_presentedRadius = current.Radius;
                ++m_localOpticsFrame;
            }
        }
        wchar_t const* ObserveGpuFrame(capture::Request const& current, capture::Frame const& frame,
                                      bool newFrame, uint64_t now)
        {
            if (frame.Status == capture::Status::Unavailable || frame.Status == capture::Status::InvalidRequest)
                return L"source-unavailable";
            if (frame.Status != capture::Status::Ready) return nullptr;
            // A frame may age out between the read and observation. Allow its
            // display TTL to expire without treating that as a malformed offer.
            if (!capture::detail::Fresh(frame.PublishedTick, now)) return nullptr;
            if (!client_detail::CompatibleGpuBaseCanvas(current, frame, now)) return L"incompatible-gpu-payload";
            const bool hadBase = m_gpuSurface.HasBase();
            if (m_gpuSurface.Observe(frame, now))
            {
                if (newFrame || !hadBase) ++m_sourceChanges;
                m_lastFreshFrame = m_gpuSurface.PublishedTick();
            }
            else if (m_gpuSurface.LastError() != E_PENDING) return L"gpu-source-unavailable";
            return nullptr;
        }
        bool ConsumeMappedGpu(capture::Request const& current, uint64_t now)
        {
            capture::Frame frame;
            const bool newFrame = m_reader.ReadMappedGpu(current.Pid, current.TargetId, current.Revision, frame, now);
            if (!ObserveGpuFrame(current, frame, newFrame, GetTickCount64())) return true;
            // Withdraw the display immediately. Only Tick reacts to a broker
            // status by closing the reader or retiring its input offer. That
            // status may still be mapped while a replacement is importing.
            m_gpuSurface.ClearPresentation(); m_ready = false;
            return false;
        }
        void Consume(capture::Request const& current, bool visible)
        {
            if (!m_published) return;
            const uint64_t publication = m_publication.Capture();
            capture::Frame frame;
            const bool newFrame = m_reader.ReadLatest(m_directory, GetCurrentProcessId(), m_id, m_revision, frame,
                current.Flags & capture::ClientOptics ? 100 : 0, (current.Flags & capture::GpuBase) != 0);
            const uint64_t now = GetTickCount64();
            if (current.Flags & capture::GpuBase)
            {
                if (auto status = ObserveGpuFrame(current, frame, newFrame, now))
                {
                    SetUnavailable(true);
                    if (m_publication.Current(publication)) ClientStatus(status, &current);
                }
                return;
            }
            if (frame.Status == capture::Status::Unavailable || frame.Status == capture::Status::InvalidRequest)
            {
                SetUnavailable(true); ClientStatus(L"source-unavailable", &current); return;
            }
            if (frame.Status != capture::Status::Ready) return;
            uint32_t expectedWidth = 0, expectedHeight = 0;
            if (!capture::detail::Dimensions(current, expectedWidth, expectedHeight)
                || frame.Revision != m_revision || frame.Width != expectedWidth || frame.Height != expectedHeight
                || ((current.Flags & capture::WorldCanvas)
                    && !client_detail::SameRect(frame.BitmapBounds, capture::detail::CanvasBounds(current))))
            {
                SetUnavailable(true); ClientStatus(L"incompatible-frame", &current); return;
            }
            if (current.Flags & capture::ClientOptics)
            {
                if (!client_detail::CompatibleBaseCanvas(current, frame))
                {
                    SetUnavailable(true); ClientStatus(L"incompatible-base-payload", &current); return;
                }
                m_lastFreshFrame = frame.PublishedTick;
                if (newFrame && !frame.Pixels.empty())
                {
                    if (m_currentOptics.Serial() != frame.Serial) ++m_sourceChanges;
                    PixelFrame base;
                    base.Width = frame.Width; base.Height = frame.Height; base.Bounds = frame.BitmapBounds;
                    base.Pixels = std::move(frame.Pixels);
                    m_currentOptics.SetBase(std::move(base), current.Revision, frame.Serial);
                }
                return;
            }
            const auto motion = std::find_if(m_motions.begin(), m_motions.end(), [&](Motion const& candidate)
            { return candidate.Sequence == frame.MotionSequence && client_detail::SameRect(candidate.Bounds, frame.RenderedBounds); });
            if (motion == m_motions.end() || frame.MotionSequence < m_presentedMotion) return;
            const bool currentBounds = client_detail::SameRect(frame.RenderedBounds, current.Bounds)
                && motion->Scale == current.Scale && motion->Radius == current.Radius;
            if (!CanRetain(true, visible, currentBounds, frame.PublishedTick, motion->Tick, current, now)) return;
            m_lastFreshFrame = frame.PublishedTick;
            if (!newFrame || frame.Pixels.empty()) return;
            UploadPixels(current, frame.Width, frame.Height, frame.BitmapBounds, frame.Pixels);
            const bool first = !m_ready;
            m_ready = true; m_presentedMotion = frame.MotionSequence;
            m_presentedMotionTick = motion->Tick; m_presentedBounds = frame.RenderedBounds;
            m_presentedScale = motion->Scale; m_presentedRadius = motion->Radius;
            if (current.Flags & capture::WorldCanvas) m_pendingField.Observe(visible, currentBounds, now);
            const bool accepted = client_detail::Notify(m_changed, m_brush, true);
            if (!m_publication.Current(publication)) return;
            if (!accepted)
            {
                SetUnavailable(true); ClientStatus(L"presentation-failed", &current); return;
            }
            if (first) ClientStatus(L"ready-real-scene", &current);
        }
        static void WriteEpochGeometry(FILE* file, wchar_t const* label, capture::Request const& request,
                                       client_detail::SurfaceLayout const& layout)
        {
            fwprintf(file, L"%ls hwnd=%llu top=%llu kind=%u flags=%u hostClipsCorners=%u root=%llu "
                L"actualWidth=%.17g actualHeight=%.17g raster=%.17g cornerRadius=%.17g "
                L"lens=%.17g dispersion=%.17g noise=%.17g tint=%.17g blur=%.17g color=%08X specular=%.17g "
                L"bounds=%d,%d,%d,%d scale=%.17g radius=%.17g output=%u,%u motion=%llu visible=%u\n",
                label, request.CoordinateHwnd, request.TopLevelHwnd, static_cast<uint32_t>(request.Kind),
                request.Flags, request.Flags & capture::HostClipsCorners, static_cast<unsigned long long>(layout.Root),
                layout.Width, layout.Height, layout.Raster, layout.CornerRadius,
                request.Material.Lens, request.Material.Dispersion, request.Material.Noise, request.Material.Tint,
                request.Material.Blur, request.Material.Color, request.Specular,
                request.Bounds.Left, request.Bounds.Top, request.Bounds.Right, request.Bounds.Bottom,
                request.Scale, request.Radius, request.OutputWidth, request.OutputHeight,
                request.MotionSequence, request.Visible);
        }
        void TraceEpochChange(capture::Request const& request, client_detail::SurfaceLayout const& layout) noexcept
        {
            // Diagnostics never change capture availability. Bound both the
            // number of writes and the size of each per-client trace entry.
            if (m_epochTraceCount >= 64) return;
            const uint32_t entry = ++m_epochTraceCount;
            try
            {
                const auto path = capture::detail::Path(m_directory, GetCurrentProcessId(), m_id, L".epoch-trace.txt");
                FILE* file = nullptr;
                if (_wfopen_s(&file, path.c_str(), entry == 1 ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") != 0 || !file) return;
                fwprintf(file, L"entry=%u tick=%llu pid=%u target=%llu beforeEpoch=%llu afterEpoch=%llu "
                    L"hadGeometry=%u published=%u ready=%u sameSurfaceEpoch=%u\n",
                    entry, GetTickCount64(), GetCurrentProcessId(), m_id, m_revision, m_revision + 1,
                    static_cast<uint32_t>(m_hasGeometry), static_cast<uint32_t>(m_published), static_cast<uint32_t>(m_ready),
                    static_cast<uint32_t>(client_detail::SameSurfaceEpoch(request, m_geometry, layout, m_layout)));
                WriteEpochGeometry(file, L"before", m_geometry, m_layout);
                WriteEpochGeometry(file, L"after", request, layout);
                fclose(file);
            }
            catch (...) {}
        }
        void TraceLifecycle(wchar_t const* event, wchar_t const* reason, HRESULT error = S_OK, int fallbackAllowed = -1) noexcept
        {
            const DWORD savedLastError = GetLastError();
            if (m_lifecycleTraceCount >= 128) return;
            const uint32_t entry = ++m_lifecycleTraceCount;
            int elementAlive = -1, loaded = -1, elementVisible = -1, hostVisible = -1, weakRootAlive = -1;
            double width = 0, height = 0;
            uintptr_t currentRoot = 0, weakRoot = 0;
            HRESULT elementError = S_OK, weakRootError = S_OK;
            try
            {
                const auto element = m_element.get();
                elementAlive = element ? 1 : 0;
                if (element)
                {
                    loaded = element.IsLoaded() ? 1 : 0;
                    width = element.ActualWidth(); height = element.ActualHeight();
                    elementVisible = element.Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible ? 1 : 0;
                    const auto root = element.XamlRoot();
                    if (root)
                    {
                        currentRoot = reinterpret_cast<uintptr_t>(root.as<::IUnknown>().get());
                        hostVisible = root.IsHostVisible() ? 1 : 0;
                    }
                }
            }
            catch (...) { elementError = client_detail::DiagnosticHresult(); }
            try
            {
                const auto root = m_root.get();
                weakRootAlive = root ? 1 : 0;
                if (root) weakRoot = reinterpret_cast<uintptr_t>(root.as<::IUnknown>().get());
            }
            catch (...) { weakRootError = client_detail::DiagnosticHresult(); }
            try
            {
                const auto path = capture::detail::Path(m_directory, GetCurrentProcessId(), m_id, L".lifecycle-trace.txt");
                FILE* file = nullptr;
                if (_wfopen_s(&file, path.c_str(), entry == 1 ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") == 0 && file)
                {
                    fwprintf(file, L"entry=%u tick=%llu pid=%u thread=%u target=%llu event=%ls reason=%ls "
                        L"hresult=0x%08lX lastError=%lu epoch=%llu motion=%llu publication=%llu "
                        L"hadGeometry=%u published=%u ready=%u previousState=%ls fallbackAllowed=%d\n",
                        entry, GetTickCount64(), GetCurrentProcessId(), GetCurrentThreadId(), m_id, event, reason,
                        static_cast<unsigned long>(error), static_cast<unsigned long>(savedLastError), m_revision,
                        m_motionSequence, m_publication.Capture(), static_cast<uint32_t>(m_hasGeometry),
                        static_cast<uint32_t>(m_published), static_cast<uint32_t>(m_ready), m_lastStatus.c_str(), fallbackAllowed);
                    fwprintf(file, L"elementAlive=%d loaded=%d actualWidth=%.17g actualHeight=%.17g elementVisible=%d "
                        L"currentRoot=%llu hostVisible=%d elementSnapshotHresult=0x%08lX "
                        L"weakRootAlive=%d weakRoot=%llu weakRootHresult=0x%08lX "
                        L"rememberedRoot=%llu lastOwnerOwns=%u coordinateVisible=%u topVisible=%u\n",
                        elementAlive, loaded, width, height, elementVisible,
                        static_cast<unsigned long long>(currentRoot), hostVisible, static_cast<unsigned long>(elementError),
                        weakRootAlive, static_cast<unsigned long long>(weakRoot), static_cast<unsigned long>(weakRootError),
                        static_cast<unsigned long long>(m_layout.Root), static_cast<uint32_t>(OwnsWindows(m_geometry)),
                        static_cast<uint32_t>(IsWindowVisible(reinterpret_cast<HWND>(m_geometry.CoordinateHwnd)) != FALSE),
                        static_cast<uint32_t>(IsWindowVisible(reinterpret_cast<HWND>(m_geometry.TopLevelHwnd)) != FALSE));
                    WriteEpochGeometry(file, L"remembered", m_geometry, m_layout);
                    fclose(file);
                }
            }
            catch (...) {}
            SetLastError(savedLastError);
        }
        void TraceGeometryFailure(wchar_t const* reason, winrt::Windows::Foundation::IInspectable const& currentRoot,
                                  winrt::Windows::Foundation::IInspectable const& weakRoot, bool fallbackAllowed) noexcept
        {
            // Hidden, unloaded panels can report the same failure every tick.
            // Record transitions, including weak-root loss, rather than frames.
            const auto current = reinterpret_cast<uintptr_t>(winrt::get_abi(currentRoot));
            const auto remembered = reinterpret_cast<uintptr_t>(winrt::get_abi(weakRoot));
            if (!m_geometryFailureActive || m_lastGeometryReason != reason || m_lastGeometryCurrentRoot != current
                || m_lastGeometryWeakRoot != remembered || m_lastGeometryFallback != fallbackAllowed
                || m_lastGeometryKnown != m_hasGeometry)
            {
                TraceLifecycle(L"geometry-false", reason, S_OK, fallbackAllowed ? 1 : 0);
                m_lastGeometryReason = reason; m_lastGeometryCurrentRoot = current; m_lastGeometryWeakRoot = remembered;
                m_lastGeometryFallback = fallbackAllowed; m_lastGeometryKnown = m_hasGeometry;
            }
            m_geometryFailureActive = true;
        }
        void ClientStatus(wchar_t const* state, capture::Request const* request = nullptr)
        {
            if (m_lastStatus == state) return;
            m_lastStatus = state;
            const auto path = capture::detail::Path(m_directory, GetCurrentProcessId(), m_id, L".client-status.txt");
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") == 0 && file)
            {
                fwprintf(file, L"%ls\n", state);
                if (request) fwprintf(file, L"hwnd=%llu top=%llu bounds=%d,%d,%d,%d scale=%.2f revision=%llu\n",
                    request->CoordinateHwnd, request->TopLevelHwnd, request->Bounds.Left, request->Bounds.Top,
                    request->Bounds.Right, request->Bounds.Bottom, request->Scale, request->Revision);
                fwprintf(file, L"layoutRoot=%llu actualWidth=%.17g actualHeight=%.17g raster=%.17g cornerRadius=%.17g\n",
                    static_cast<unsigned long long>(m_layout.Root), m_layout.Width, m_layout.Height, m_layout.Raster, m_layout.CornerRadius);
                fwprintf(file, L"visibilityInputs loaded=%u elementVisible=%u coordinateVisible=%u topVisible=%u iconic=%u "
                    L"cloaked=%u cloakKnown=%u owned=%u resolver=%u xamlHostVisible=%d coreWindowVisible=%d\n",
                    m_visibility.Loaded, m_visibility.ElementVisible, m_visibility.CoordinateVisible, m_visibility.TopVisible,
                    m_visibility.Iconic, m_visibility.Cloaked, m_visibility.CloakKnown, m_visibility.Owned,
                    m_visibility.Resolved, m_visibility.HostVisible, m_visibility.WindowVisible);
                fwprintf(file, L"presentationReady=%u bitmapBounds=%d,%d,%d,%d presentedMotion=%llu\n",
                    static_cast<unsigned>(m_ready), m_bitmapBounds.Left, m_bitmapBounds.Top,
                    m_bitmapBounds.Right, m_bitmapBounds.Bottom, static_cast<unsigned long long>(m_presentedMotion));
                fwprintf(file, L"presentationBackend=%ls backendReady=%u\n", m_gpuEnabled ? L"gpu-texture" : L"image",
                    m_gpuEnabled ? m_gpuSurface.Ready() : m_ready);
                if (m_gpuEnabled) fwprintf(file, L"gpuBase=%u gpuError=%08X gpuSourceTick=%llu\n",
                    m_gpuSurface.HasBase(), static_cast<unsigned>(m_gpuSurface.LastError()), m_gpuSurface.PublishedTick());
                fwprintf(file, L"localOpticsFrame=%llu localPoseTick=%llu sourceBaseSerial=%llu\n",
                    m_localOpticsFrame, m_presentedMotionTick, m_currentOptics.Serial());
                fclose(file);
            }
        }
        static double PerformanceNow()
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        void TracePerformance() noexcept
        {
            // Opt-in bounded timing aggregates, never per-frame I/O or visual snapshots.
            const uint64_t now = GetTickCount64();
            if (!m_performanceEnabled || (m_lastPerformanceWrite && now - m_lastPerformanceWrite < 1000)) return;
            if (!m_lastPerformanceWrite) { m_lastPerformanceWrite = now; return; }
            m_lastPerformanceWrite = now;
            try
            {
                const auto path = capture::detail::Path(m_directory, GetCurrentProcessId(), m_id, L".performance.json");
                FILE* file = nullptr;
                if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") == 0 && file)
                {
                    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
                    fwprintf(file, L"{\"tick\":%llu,\"visible\":%u,\"qpcFrequency\":%lld,\"poseChanges\":%llu,\"sourceSerialChanges\":%llu,"
                        L"\"baseSerial\":%llu,\"localFrames\":%llu,", now, m_geometry.Visible, frequency.QuadPart,
                        m_poseChanges, m_sourceChanges, m_currentOptics.Serial(), m_localOpticsFrame);
                    fwprintf(file, L"\"visibility\":{\"loaded\":%u,\"elementVisible\":%u,\"coordinateVisible\":%u,\"topVisible\":%u,"
                        L"\"iconic\":%u,\"cloaked\":%u,\"cloakKnown\":%u,\"owned\":%u,\"resolver\":%u,"
                        L"\"xamlHostVisible\":%d,\"coreWindowVisible\":%d},",
                        m_visibility.Loaded, m_visibility.ElementVisible, m_visibility.CoordinateVisible, m_visibility.TopVisible,
                        m_visibility.Iconic, m_visibility.Cloaked, m_visibility.CloakKnown, m_visibility.Owned,
                        m_visibility.Resolved, m_visibility.HostVisible, m_visibility.WindowVisible);
                    const auto timing = [file](wchar_t const* name, client_detail::TimingSamples const& values)
                    {
                        fwprintf(file, L"\"%ls\":{\"count\":%llu,\"meanMs\":%.4f,\"minMs\":%.4f,\"maxMs\":%.4f},",
                            name, values.Count, values.Mean(), values.Minimum, values.Maximum);
                    };
                    timing(L"render", m_renderTiming); timing(L"renderInterval", m_renderInterval);
                    timing(L"source", m_tickTiming); timing(L"sourceInterval", m_tickInterval);
                    timing(L"optics", m_opticsTiming); timing(L"upload", m_uploadTiming);
                    fwprintf(file, L"\"backend\":\"%ls\"}\n", m_gpuEnabled ? L"gpu-texture" : L"image");
                    fclose(file);
                }
            }
            catch (...) {}
            m_renderTiming = {}; m_tickTiming = {}; m_renderInterval = {}; m_tickInterval = {};
            m_opticsTiming = {}; m_uploadTiming = {};
            m_poseChanges = m_sourceChanges = 0;
        }
        void CheckPerformanceEnabled()
        {
            const uint64_t now = GetTickCount64();
            if (m_lastPerformanceCheck && now - m_lastPerformanceCheck < 100) return;
            m_lastPerformanceCheck = now;
            const auto path = (std::filesystem::path(m_directory) / L"performance.enabled").wstring();
            const bool enabled = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
            const auto gpuPath = (std::filesystem::path(m_directory) / L"gpu-texture.enabled").wstring();
            const bool gpu = m_kind == capture::Kind::Panel && GetFileAttributesW(gpuPath.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (gpu != m_gpuEnabled)
            {
                const auto publication = m_publication.Capture();
                SetUnavailable(true);
                if (!m_publication.Current(publication)) return;
                m_gpuSurface.Reset(); m_gpuEnabled = gpu;
            }
            if (enabled != m_performanceEnabled)
            {
                m_performanceEnabled = enabled; m_lastPerformanceWrite = 0;
                m_renderTiming = {}; m_tickTiming = {}; m_renderInterval = {}; m_tickInterval = {};
                m_opticsTiming = {}; m_uploadTiming = {};
                m_poseChanges = m_sourceChanges = 0;
                m_lastRenderTime = m_lastTickTime = 0;
            }
        }
        struct CallbackScope
        {
            LiquidSurfaceClient& Client;
            bool Rendering, Entered;
            double Began = 0;
            CallbackScope(LiquidSurfaceClient& client, bool rendering, int64_t frame = 0)
                : Client(client), Rendering(rendering), Entered(client.m_driver.Enter(rendering, frame))
            {
                if (!Entered) return;
                Began = PerformanceNow();
                auto& last = Rendering ? Client.m_lastRenderTime : Client.m_lastTickTime;
                if (last) (Rendering ? Client.m_renderInterval : Client.m_tickInterval).Add(Began - last);
                last = Began;
            }
            ~CallbackScope()
            {
                if (!Entered) return;
                (Rendering ? Client.m_renderTiming : Client.m_tickTiming).Add(PerformanceNow() - Began);
                Client.TracePerformance();
                Client.m_driver.Leave();
            }
            explicit operator bool() const { return Entered; }
        };
        void UpdateSourceInterval(bool visible)
        {
            if (!m_timer) return;
            const auto interval = std::chrono::milliseconds(client_detail::SourceInterval(m_kind == capture::Kind::Panel, visible));
            if (m_timer.Interval() != interval) m_timer.Interval(interval);
        }
        void Present(int64_t frame) noexcept
        {
            bool sourceNeeded = false;
            {
                CallbackScope callback(*this, true, frame);
                if (!callback) return;
                try
                {
                    const uint64_t publication = m_publication.Capture();
                    auto element = m_element.get();
                    if (!element) { Stop(L"render-target-destroyed"); return; }
                    if (!element.IsLoaded()) return;
                    // Native visibility changes precede the source timer. Use
                    // the current HWND state, never the cached request flag.
                    if (m_hasGeometry && !IsWindowVisible(reinterpret_cast<HWND>(m_geometry.CoordinateHwnd))) return;
                    capture::Request current;
                    client_detail::SurfaceLayout layout;
                    winrt::Windows::Foundation::IInspectable root{nullptr};
                    bool visible = false; wchar_t const* reason = L"render-geometry";
                    if (!Geometry(element, current, layout, root, visible, reason)) sourceNeeded = true;
                    else
                    {
                        UpdateSourceInterval(visible);
                        current.Pid = GetCurrentProcessId(); current.TargetId = m_id; current.Kind = m_kind;
                        current.Material = m_appearance.Material; current.Specular = m_appearance.Specular;
                        current.Visible = visible ? 1 : 0; current.Flags |= capture::KeepWarm;
                        const auto work = client_detail::SelectPresentationWork(m_hasGeometry
                            && client_detail::SameSurfaceEpoch(current, m_geometry, layout, m_layout), m_published,
                            current.Visible != m_request.Visible);
                        sourceNeeded = work.Source;
                        if (work.Present)
                        {
                            current.Revision = m_revision;
                            m_geometry = current; m_layout = layout; m_root = winrt::make_weak(root);
                            const uint64_t now = GetTickCount64();
                            if (current.Flags & capture::GpuBase)
                            {
                                if (!m_requestWrites.CanPresent(visible, now))
                                { m_gpuSurface.ClearPresentation(); m_ready = false; }
                                else if (visible && ConsumeMappedGpu(current, now)
                                    && m_publication.Current(publication))
                                    ApplyGpuOptics(current, GetTickCount64());
                                if (!m_publication.Current(publication)) return;
                            }
                            else
                            {
                                if ((m_ready || m_currentOptics.HasBase()) && (!m_requestWrites.CanPresent(visible, now)
                                    || !CanRetain(true, visible, SamePresentedField(current), m_lastFreshFrame, m_presentedMotionTick, current, now)))
                                    SetUnavailable(true);
                                if (!m_publication.Current(publication)) return;
                                if (visible)
                                {
                                    if (!SamePresentedField(current)) ++m_poseChanges;
                                    UpdateBrushTransform(current);
                                    ApplyCurrentOptics(current, now);
                                }
                            }
                        }
                    }
                }
                catch (...)
                {
                    TraceLifecycle(L"render-exception", L"present-current-pose", client_detail::DiagnosticHresult());
                    sourceNeeded = true;
                }
            }
            // Epoch/visibility transitions must register immediately; ordinary
            // rendering never reads or writes request/response files.
            if (sourceNeeded) Tick();
        }
        void Tick() noexcept
        {
            CallbackScope callback(*this, false);
            if (!callback) return;
            wchar_t const* phase = L"get-element";
            try
            {
                const uint64_t publication = m_publication.Capture();
                CheckPerformanceEnabled();
                if (!m_publication.Current(publication)) return;
                const auto element = m_element.get();
                if (!element)
                {
                    CancelCapture(L"target-destroyed"); ClientStatus(L"target-destroyed"); return;
                }
                capture::Request request;
                client_detail::SurfaceLayout layout;
                winrt::Windows::Foundation::IInspectable root{nullptr};
                bool visible = false;
                if (!Geometry(element, request, layout, root, visible, phase))
                {
                    const auto geometryReason = phase;
                    phase = L"geometry-false-current-root";
                    const auto currentRoot = element.XamlRoot();
                    phase = L"geometry-false-restore-weak-root";
                    root = m_root.get();
                    // Only an unloaded, unmeasured, or explicitly hidden host
                    // may reuse its known source. A visible mapping failure
                    // must cancel instead of retaining an old bitmap forever.
                    phase = L"geometry-false-check-fallback";
                    const bool fallbackAllowed = client_detail::Activity(m_kind == capture::Kind::Panel, m_hasGeometry, false)
                        == client_detail::SurfaceActivity::HiddenWarm && root && OwnsWindows(m_geometry)
                        && client_detail::CanReuseHiddenGeometry(element.IsLoaded(), element.ActualWidth(), element.ActualHeight(),
                            !currentRoot || currentRoot.IsHostVisible())
                        && (!currentRoot || reinterpret_cast<uintptr_t>(currentRoot.as<::IUnknown>().get()) == m_layout.Root);
                    TraceGeometryFailure(geometryReason, currentRoot, root, fallbackAllowed);
                    if (fallbackAllowed)
                    {
                        request = m_geometry; layout = m_layout; visible = false;
                    }
                    else
                    {
                        if (m_hasGeometry) CancelCapture(geometryReason);
                        ClientStatus(L"waiting-for-layout"); return;
                    }
                }
                else m_geometryFailureActive = false;
                UpdateSourceInterval(visible);
                phase = L"prepare-request";
                request.Pid = GetCurrentProcessId(); request.TargetId = m_id; request.Kind = m_kind;
                request.Material = m_appearance.Material; request.Specular = m_appearance.Specular;
                const auto activity = client_detail::Activity(m_kind == capture::Kind::Panel, true, visible);
                if (activity == client_detail::SurfaceActivity::Stop)
                {
                    CancelCapture(L"hidden-non-warm-surface"); ClientStatus(L"hidden"); return;
                }
                request.Visible = visible ? 1 : 0;
                if (m_kind == capture::Kind::Panel) request.Flags |= capture::KeepWarm;
                const bool epochChanged = !m_hasGeometry
                    || !client_detail::SameSurfaceEpoch(request, m_geometry, layout, m_layout);
                if (epochChanged)
                {
                    phase = L"epoch-change";
                    TraceEpochChange(request, layout);
                    ++m_revision; m_motions.clear(); m_pendingField = {}; m_requestWrites = {}; SetUnavailable(true);
                    if (!m_publication.Current(publication)) return;
                }
                request.Revision = m_revision;
                phase = L"remember-root-weak-reference";
                m_hasGeometry = true; m_geometry = request; m_layout = layout; m_root = winrt::make_weak(root);
                uint64_t now = GetTickCount64();
                if (request.Flags & capture::GpuBase)
                {
                    phase = L"prepare-gpu-input";
                    uint32_t width = 0, height = 0;
                    const auto window = winrt::Windows::UI::Xaml::Window::Current();
                    if (window && capture::detail::Dimensions(request, width, height)
                        && m_gpuSurface.Prepare(window.Compositor(), m_directory, m_id, m_clientEpoch, m_revision, width, height))
                    {
                        AttachGpuBrush();
                        if (!m_publication.Current(publication)) return;
                    }
                    else SetUnavailable(true);
                    if (!m_publication.Current(publication)) return;
                }
                if (request.Flags & capture::WorldCanvas)
                    m_pendingField.Observe(visible, m_ready && SamePresentedField(request), now);
                if (m_kind != capture::Kind::Panel || !visible) UpdateBrushTransform(request);
                // Read the completed previous request before moving the
                // requested sample position forward. Motion has one epoch,
                // so a bounded pipeline lag does not starve presentation.
                phase = L"consume-frame";
                if (!epochChanged && m_requestWrites.CanPresent(visible, now)) Consume(request, visible);
                if (!m_publication.Current(publication)) return;
                now = GetTickCount64();
                phase = L"retain-presentation";
                if (!(request.Flags & capture::GpuBase) && (m_ready || m_currentOptics.HasBase()) && (!m_requestWrites.CanPresent(visible, now)
                    || !CanRetain(true, visible, SamePresentedField(request), m_lastFreshFrame, m_presentedMotionTick, request, now)))
                    SetUnavailable(true);
                if (!m_publication.Current(publication)) return;
                // A completed GPU source may be consumed between Rendering
                // callbacks. Queue its optics now; the presenter suppresses
                // duplicate poses and still waits for the GPU ready fence.
                if ((request.Flags & capture::ClientOptics) && (!visible || (request.Flags & capture::GpuBase))
                    && m_requestWrites.CanPresent(visible, now))
                {
                    phase = L"render-current-optics";
                    ApplyCurrentOptics(request, now);
                    if (!m_publication.Current(publication)) return;
                }
                const bool motionChanged = client_detail::NeedsMotionRegistration(epochChanged, m_published,
                    m_revision, m_request.Revision, SameMotion(request, m_request));
                request.MotionSequence = m_motionSequence + (motionChanged ? 1 : 0); request.HeartbeatTick = now;
                const bool changed = (!(request.Flags & capture::ClientOptics) && motionChanged)
                    || epochChanged || !m_published || request.Visible != m_request.Visible;
                if (m_requestWrites.RetryPending || changed || now - m_lastWrite >= 100)
                {
                    phase = L"write-request";
                    if (!capture::WriteRequest(m_directory, request))
                    {
                        const DWORD writeError = GetLastError();
                        const HRESULT error = HRESULT_FROM_WIN32(writeError);
                        const bool alreadyRetrying = m_requestWrites.RetryPending;
                        if (m_requestWrites.Failed(writeError) == client_detail::RequestWriteFailure::Cancel)
                        {
                            CancelCapture(L"request-write-failed", error);
                            ClientStatus(L"request-write-failed", &request); return;
                        }
                        if (!alreadyRetrying) TraceLifecycle(L"request-retry", L"request-write-failed", error);
                        if (!m_requestWrites.CanPresent(visible, now)) SetUnavailable(true);
                        ClientStatus(L"request-write-retry", &request); return;
                    }
                    m_requestWrites.Succeeded(now);
                    if (motionChanged)
                    {
                        m_motionSequence = request.MotionSequence;
                        if (!(request.Flags & capture::ClientOptics))
                        {
                            m_motions.push_back({request.MotionSequence, now, request.Bounds, request.Scale, request.Radius});
                            while (m_motions.size() > 64) m_motions.pop_front();
                        }
                    }
                    m_request = request; m_lastWrite = now; m_published = true;
                }
                phase = L"write-client-status";
                ClientStatus(!visible ? L"hidden-source-warm" : m_ready ? L"ready-real-scene" : L"waiting-for-capture", &request);
            }
            catch (...)
            {
                const HRESULT error = client_detail::DiagnosticHresult();
                TraceLifecycle(L"tick-exception", phase, error);
                try { CancelCapture(L"tick-exception", error); ClientStatus(L"client-error"); } catch (...) {}
            }
        }

        winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> m_element;
        std::wstring m_directory, m_lastStatus;
        capture::Kind m_kind;
        HWND m_taskbar;
        Changed m_changed;
        WindowResolver m_resolver;
        LiquidAppearance m_appearance;
        client_detail::PublicationGeneration m_publication;
        client_detail::RequestWriteState m_requestWrites;
        client_detail::CurrentCanvasOptics m_currentOptics;
        client_detail::PendingCanvasField m_pendingField;
        client_detail::PresentationDriver m_driver;
        client_detail::SurfaceVisibility m_visibility;
        client_detail::TimingSamples m_renderTiming, m_tickTiming, m_renderInterval, m_tickInterval, m_opticsTiming, m_uploadTiming;
        double m_lastRenderTime = 0, m_lastTickTime = 0;
        uint64_t m_lastPerformanceWrite = 0, m_lastPerformanceCheck = 0, m_poseChanges = 0, m_sourceChanges = 0;
        bool m_performanceEnabled = false;
        uint64_t m_id = 0, m_clientEpoch = 0, m_revision = 0, m_lastWrite = 0, m_lastFreshFrame = 0;
        GpuLiquidSurface m_gpuSurface;
        bool m_gpuEnabled = false, m_gpuAttached = false;
        uint32_t m_epochTraceCount = 0, m_lifecycleTraceCount = 0;
        wchar_t const* m_lastGeometryReason = nullptr;
        uintptr_t m_lastGeometryCurrentRoot = 0, m_lastGeometryWeakRoot = 0;
        bool m_geometryFailureActive = false, m_lastGeometryFallback = false, m_lastGeometryKnown = false;
        uint64_t m_motionSequence = 0, m_presentedMotion = 0, m_presentedMotionTick = 0;
        uint64_t m_localOpticsFrame = 0;
        double m_presentedScale = 1, m_presentedRadius = 0;
        bool m_ready = false, m_published = false, m_hasGeometry = false;
        capture::Request m_request, m_geometry;
        client_detail::SurfaceLayout m_layout;
        PixelRect m_presentedBounds, m_bitmapBounds;
        std::deque<Motion> m_motions;
        winrt::weak_ref<winrt::Windows::Foundation::IInspectable> m_root;
        capture::FrameReader m_reader;
        Brush m_brush{nullptr};
        winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap m_bitmap{nullptr};
        winrt::Windows::UI::Xaml::Media::MatrixTransform m_brushTransform{nullptr};
        winrt::Windows::UI::Xaml::DispatcherTimer m_timer{nullptr};
        winrt::event_token m_tick{};
        winrt::event_token m_rendering{};
    };
}
