#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef GetCurrentTime
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <unordered_set>
#include <atomic>
#include <memory>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "GlassPixels.h"
#include "CaptureWake.h"

namespace novabar
{
    namespace capture_detail
    {
        // Retained capture-resource ownership is separate from WGC session
        // lifetime so resource failures can be recovered without a new window.
        inline void EnsureRetainedGpuTexture(ID3D11Device* device, D3D11_TEXTURE2D_DESC source,
            winrt::com_ptr<ID3D11Texture2D>& latest, winrt::com_ptr<ID3D11ShaderResourceView>& view,
            std::shared_ptr<void>& lease)
        {
            D3D11_TEXTURE2D_DESC existing{};
            if (latest) latest->GetDesc(&existing);
            // A caller may retain a generation across another drain or Close.
            // Copy-on-write keeps that GPU snapshot's pixels and view stable.
            if (!latest || !view || source.Width != existing.Width || source.Height != existing.Height
                || source.Format != existing.Format || (lease && lease.use_count() > 1))
            {
                // Commit one complete generation. An allocation failure must
                // not leave a texture without its SRV or with an older lease.
                winrt::com_ptr<ID3D11Texture2D> next;
                winrt::com_ptr<ID3D11ShaderResourceView> nextView;
                source.Usage = D3D11_USAGE_DEFAULT; source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                source.CPUAccessFlags = 0; source.MiscFlags = 0;
                winrt::check_hresult(device->CreateTexture2D(&source, nullptr, next.put()));
                winrt::check_hresult(device->CreateShaderResourceView(next.get(), nullptr, nextView.put()));
                auto nextLease=std::make_shared<int>(0);
                latest=std::move(next);view=std::move(nextView);lease=std::move(nextLease);
            }
        }
    }

    class WindowCapture
    {
        using Pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
        using Item = winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
        using Session = winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
        using Device = winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        winrt::com_ptr<ID3D11Texture2D> m_staging;
        winrt::com_ptr<ID3D11Texture2D> m_latest;
        winrt::com_ptr<ID3D11ShaderResourceView> m_latestView;
        std::shared_ptr<void> m_gpuLease;
        Device m_projected{nullptr};
        Pool m_pool{nullptr};
        Item m_item{nullptr};
        Session m_session{nullptr};
        winrt::Windows::Graphics::SizeInt32 m_size{};
        HWND m_window = nullptr;
        RECT m_lastReadWindow{};
        PixelRect m_lastReadRegion;
        int m_latestWindowWidth = 0, m_latestWindowHeight = 0;
        uint64_t m_generation = 0;
        std::shared_ptr<std::atomic<bool>> m_closed;
        std::shared_ptr<capture_detail::CaptureWakeRegistration> m_frameWake;
        winrt::event_token m_frameArrivedToken{};
        bool m_frameArrivedAttached = false;
        bool m_haveRead = false;
        void EnsureLatest(D3D11_TEXTURE2D_DESC source)
        { capture_detail::EnsureRetainedGpuTexture(m_device.get(), source, m_latest, m_latestView, m_gpuLease); }
        void StartSession()
        {
            // Reattach on pool recreation/session resize as well as first start.
            // The delegate retains only safe wake ownership, never this/session,
            // a frame, a context, or any GPU resource.
            if (m_frameWake)
            {
                if (m_frameArrivedAttached) m_pool.FrameArrived(m_frameArrivedToken);
                m_frameArrivedAttached = false;
                m_frameArrivedToken = m_pool.FrameArrived([wake = m_frameWake](auto const&, auto const&)
                    { wake->Notify(); });
                m_frameArrivedAttached = true;
            }
            m_session = m_pool.CreateCaptureSession(m_item);
            try { m_session.IsCursorCaptureEnabled(false); } catch (...) {}
            try { m_session.IncludeSecondaryWindows(false); } catch (...) {}
            // Border policy is enforced by Windows; refusal doesn't abort capture.
            try { m_session.IsBorderRequired(false); } catch (...) {}
            m_session.StartCapture();
        }
    public:
        struct GpuFrame
        {
            winrt::com_ptr<ID3D11Texture2D> Texture;
            winrt::com_ptr<ID3D11ShaderResourceView> View;
            PixelRect WindowBounds{}, Region{}, CropTexels{};
            int Width = 0, Height = 0;
            uint64_t Generation = 0;
            std::shared_ptr<void> Lease;
        };
        ~WindowCapture() { Close(); }
        HWND Window() const { return m_closed && m_closed->load() ? nullptr : m_window; }
        uint64_t Generation() const { return Window() && m_latest ? m_generation : 0; }
        void Close() noexcept
        {
            if (m_frameWake) m_frameWake->Close();
            try { if (m_pool && m_frameArrivedAttached) m_pool.FrameArrived(m_frameArrivedToken); } catch (...) {}
            m_frameArrivedAttached = false; m_frameArrivedToken = {};
            try { if (m_session) m_session.Close(); } catch (...) {}
            try { if (m_pool) m_pool.Close(); } catch (...) {}
            m_session = nullptr; m_pool = nullptr; m_item = nullptr;
            m_staging = nullptr; m_latest = nullptr; m_latestView = nullptr; m_gpuLease.reset();
            m_context = nullptr; m_device = nullptr; m_projected = nullptr;
            m_window = nullptr;
            m_haveRead = false; m_latestWindowWidth = m_latestWindowHeight = 0;
            m_generation = 0;
            m_closed.reset();
            m_frameWake.reset();
        }
        void Start(HWND window)
        { Start(window, nullptr, nullptr); }
        void Start(HWND window, ID3D11Device* sharedDevice, ID3D11DeviceContext* sharedContext,
            std::shared_ptr<capture_detail::CaptureWakeRegistration> frameWake = {})
        {
            Close();
            if (!window || !IsWindow(window)) winrt::throw_hresult(E_INVALIDARG);
            // CreateForWindow accepts top-level windows. Wallpaper Engine renders
            // a child beneath WorkerW/Progman; capture that desktop host's own
            // composition tree, which excludes the separate taskbar window.
            if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)
                window = GetAncestor(window, GA_ROOT);
            if (!Session::IsSupported()) winrt::throw_hresult(E_NOTIMPL);
            if (sharedDevice || sharedContext)
            {
                if (!sharedDevice || !sharedContext) winrt::throw_hresult(E_INVALIDARG);
                winrt::com_ptr<ID3D11Device> owner; sharedContext->GetDevice(owner.put());
                if (owner.get() != sharedDevice) winrt::throw_hresult(E_INVALIDARG);
                m_device.copy_from(sharedDevice); m_context.copy_from(sharedContext);
            }
            else
            {
                D3D_FEATURE_LEVEL feature{};
                winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    m_device.put(), &feature, m_context.put()));
            }
            auto dxgi = m_device.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
            m_projected = inspectable.as<Device>();
            auto interop = winrt::get_activation_factory<Item, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<Item>(), winrt::put_abi(m_item)));
            m_closed = std::make_shared<std::atomic<bool>>(false);
            m_item.Closed([closed = std::weak_ptr<std::atomic<bool>>(m_closed),
                wake = std::weak_ptr<capture_detail::CaptureWakeRegistration>(frameWake)](auto const&, auto const&)
            {
                if (auto state = closed.lock()) state->store(true);
                if (auto registration = wake.lock()) registration->Close();
            });
            m_size = m_item.Size();
            if (m_size.Width <= 0 || m_size.Height <= 0) winrt::throw_hresult(E_INVALIDARG);
            m_pool = Pool::CreateFreeThreaded(m_projected,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_size);
            m_frameWake = std::move(frameWake);
            StartSession();
            m_window = window;
        }
        // Prewarming retains the latest source on the GPU. No staging texture,
        // CPU map or desktop-sized pixel allocation is required until a panel
        // asks for an actual crop.
        bool DrainLatest()
        {
            if (!m_pool || !Window() || !IsWindow(m_window)) return false;
            auto frame = m_pool.TryGetNextFrame();
            if (!frame) return false;
            for (int i = 0; i != 2; ++i)
            {
                auto next = m_pool.TryGetNextFrame();
                if (!next) break;
                frame.Close(); frame = std::move(next);
            }
            const auto size = frame.ContentSize();
            if (size.Width != m_size.Width || size.Height != m_size.Height)
            {
                frame.Close(); m_latest = nullptr; m_latestView = nullptr; m_gpuLease.reset(); m_haveRead = false;
                m_latestWindowWidth = m_latestWindowHeight = 0;
                if (size.Width > 0 && size.Height > 0)
                {
                    m_size = size; m_session.Close(); m_session = nullptr;
                    m_pool.Recreate(m_projected,
                        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_size);
                    StartSession();
                }
                return false;
            }
            RECT windowRect{};
            if (!GetWindowRect(m_window, &windowRect)) { frame.Close(); return false; }
            RECT extended{};
            if (SUCCEEDED(DwmGetWindowAttribute(m_window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(extended)))
                && extended.right > extended.left && extended.bottom > extended.top) windowRect = extended;
            const int width = windowRect.right - windowRect.left, height = windowRect.bottom - windowRect.top;
            if (width <= 0 || height <= 0 || (m_latest
                && (width != m_latestWindowWidth || height != m_latestWindowHeight)))
            { frame.Close(); return false; }
            winrt::com_ptr<ID3D11Texture2D> texture;
            auto surface = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::check_hresult(surface->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            D3D11_TEXTURE2D_DESC source{}; texture->GetDesc(&source); EnsureLatest(source);
            m_context->CopyResource(m_latest.get(), texture.get());
            m_latestWindowWidth = width; m_latestWindowHeight = height;
            ++m_generation; m_haveRead = false; frame.Close(); return true;
        }
        // Return an owned, immutable GPU generation. No staging allocation,
        // CPU pixel readback, or capture of the shell/desktop enters this path.
        bool ReadGpu(PixelRect requested, PixelRect expectedWindowBounds, GpuFrame& out)
        {
            out = {};
            if (!m_pool || !Window() || !IsWindow(m_window)) return false;
            DrainLatest();
            if (!m_latest || !m_latestView || !Window()) return false;
            RECT rectangle{};
            if (!GetWindowRect(m_window, &rectangle)) return false;
            RECT extended{};
            if (SUCCEEDED(DwmGetWindowAttribute(m_window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(extended)))
                && extended.right > extended.left && extended.bottom > extended.top) rectangle = extended;
            if (rectangle.left != expectedWindowBounds.Left || rectangle.top != expectedWindowBounds.Top
                || rectangle.right != expectedWindowBounds.Right || rectangle.bottom != expectedWindowBounds.Bottom
                || rectangle.right - rectangle.left != m_latestWindowWidth
                || rectangle.bottom - rectangle.top != m_latestWindowHeight) return false;
            PixelRect region{std::max<int>(requested.Left, rectangle.left), std::max<int>(requested.Top, rectangle.top),
                std::min<int>(requested.Right, rectangle.right), std::min<int>(requested.Bottom, rectangle.bottom)};
            if (region.Right <= region.Left || region.Bottom <= region.Top) return false;
            const double sx = static_cast<double>(m_size.Width) / m_latestWindowWidth;
            const double sy = static_cast<double>(m_size.Height) / m_latestWindowHeight;
            PixelRect crop{static_cast<int>((region.Left - rectangle.left) * sx),
                static_cast<int>((region.Top - rectangle.top) * sy),
                static_cast<int>((region.Right - rectangle.left) * sx),
                static_cast<int>((region.Bottom - rectangle.top) * sy)};
            if (crop.Right <= crop.Left || crop.Bottom <= crop.Top) return false;
            out.Texture = m_latest; out.View = m_latestView; out.WindowBounds = expectedWindowBounds;
            out.Region = region; out.CropTexels = crop; out.Width = m_size.Width; out.Height = m_size.Height;
            out.Generation = m_generation; out.Lease = m_gpuLease;
            return true;
        }
        // Copies only the target strip plus caller-supplied lens padding from
        // the GPU. The source is the window itself, never a desktop screenshot.
        // retainLatest is used by panel scene capture. A retained GPU copy lets
        // a static window service a new crop or move without waiting for damage.
        // Existing taskbar callers retain their strip-only readback behavior.
        bool Read(PixelRect requested, PixelFrame& out, bool retainLatest = false, bool forceRead = false)
        {
            if (!m_pool || !Window() || !IsWindow(m_window)) return false;
            auto frame = m_pool.TryGetNextFrame();
            const bool fresh = static_cast<bool>(frame);
            if (!fresh && (!retainLatest || !m_latest)) return false;
            for (int i = 0; frame && i != 2; ++i)
            {
                auto next = m_pool.TryGetNextFrame();
                if (!next) break;
                frame.Close(); frame = std::move(next);
            }
            const auto size = frame ? frame.ContentSize() : m_size;
            if (frame && (size.Width != m_size.Width || size.Height != m_size.Height))
            {
                frame.Close();
                m_latest = nullptr; m_latestView = nullptr; m_gpuLease.reset();
                m_haveRead = false; m_latestWindowWidth = m_latestWindowHeight = 0;
                if (size.Width > 0 && size.Height > 0)
                {
                    m_size = size;
                    m_session.Close(); m_session = nullptr;
                    m_pool.Recreate(m_projected,
                        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_size);
                    // A static obscured window may emit only the size-change
                    // frame that was just discarded. A new session requests a
                    // first correctly sized frame without source repainting.
                    StartSession();
                }
                return false;
            }
            RECT windowRect{};
            if (!GetWindowRect(m_window, &windowRect)) { if (frame) frame.Close(); return false; }
            // WGC captures the extended frame, excluding invisible resize borders.
            RECT extended{};
            if (SUCCEEDED(DwmGetWindowAttribute(m_window, DWMWA_EXTENDED_FRAME_BOUNDS, &extended, sizeof(extended)))
                && extended.right > extended.left && extended.bottom > extended.top) windowRect = extended;
            if (windowRect.right <= windowRect.left || windowRect.bottom <= windowRect.top) { if (frame) frame.Close(); return false; }
            PixelRect region{std::max<int>(requested.Left, windowRect.left), std::max<int>(requested.Top, windowRect.top),
                std::min<int>(requested.Right, windowRect.right), std::min<int>(requested.Bottom, windowRect.bottom)};
            if (region.Right <= region.Left || region.Bottom <= region.Top) { if (frame) frame.Close(); return false; }
            if (fresh && retainLatest && m_latest
                && (windowRect.right - windowRect.left != m_latestWindowWidth
                    || windowRect.bottom - windowRect.top != m_latestWindowHeight))
            {
                // A queued frame can still describe the pre-resize surface.
                // The ContentSize change above recreates the pool before a
                // newly sized frame is accepted; do not stretch queued pixels.
                frame.Close(); return false;
            }
            if (!fresh)
            {
                // A move preserves the texture; a resize needs a correctly sized
                // new frame. Never stretch an obsolete surface over new bounds.
                if (windowRect.right - windowRect.left != m_latestWindowWidth
                    || windowRect.bottom - windowRect.top != m_latestWindowHeight) return false;
                if (!forceRead && m_haveRead && EqualRect(&windowRect, &m_lastReadWindow)
                    && region.Left == m_lastReadRegion.Left && region.Top == m_lastReadRegion.Top
                    && region.Right == m_lastReadRegion.Right && region.Bottom == m_lastReadRegion.Bottom) return false;
            }
            const double sx = static_cast<double>(size.Width) / (windowRect.right - windowRect.left);
            const double sy = static_cast<double>(size.Height) / (windowRect.bottom - windowRect.top);
            D3D11_BOX box{static_cast<UINT>((region.Left - windowRect.left) * sx),
                static_cast<UINT>((region.Top - windowRect.top) * sy), 0,
                static_cast<UINT>((region.Right - windowRect.left) * sx),
                static_cast<UINT>((region.Bottom - windowRect.top) * sy), 1};
            const UINT width = box.right - box.left, height = box.bottom - box.top;
            if (!width || !height) { if (frame) frame.Close(); return false; }
            winrt::com_ptr<ID3D11Texture2D> texture;
            if (frame)
            {
                auto surface = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::check_hresult(surface->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
                if (retainLatest)
                {
                    D3D11_TEXTURE2D_DESC source{}; texture->GetDesc(&source); EnsureLatest(source);
                    m_context->CopyResource(m_latest.get(), texture.get());
                    m_latestWindowWidth = windowRect.right - windowRect.left;
                    m_latestWindowHeight = windowRect.bottom - windowRect.top;
                    ++m_generation;
                    texture = m_latest;
                }
            }
            else texture = m_latest;
            D3D11_TEXTURE2D_DESC desc{};
            if (m_staging) m_staging->GetDesc(&desc);
            if (!m_staging || desc.Width != width || desc.Height != height)
            {
                m_staging = nullptr;
                desc = {}; desc.Width = width; desc.Height = height;
                desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                winrt::check_hresult(m_device->CreateTexture2D(&desc, nullptr, m_staging.put()));
            }
            m_context->CopySubresourceRegion(m_staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(m_context->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            out.Width = static_cast<int>(width); out.Height = static_cast<int>(height); out.Bounds = region;
            out.Pixels.resize(static_cast<size_t>(width) * height * 4);
            for (UINT y = 0; y < height; ++y)
                memcpy(out.Pixels.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, width * 4);
            m_context->Unmap(m_staging.get(), 0);
            if (frame) frame.Close();
            m_lastReadWindow = windowRect; m_lastReadRegion = region; m_haveRead = true;
            return true;
        }
    };

    inline HWND FindWallpaperWindow(PixelRect target)
    {
        std::unordered_set<DWORD> wallpaperPids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W entry{sizeof(entry)};
            if (Process32FirstW(snapshot, &entry)) do
            {
                if (_wcsicmp(entry.szExeFile, L"wallpaper64.exe") == 0 || _wcsicmp(entry.szExeFile, L"wallpaper32.exe") == 0)
                    wallpaperPids.insert(entry.th32ProcessID);
            } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
        struct Search { PixelRect Target; const std::unordered_set<DWORD>* Pids; HWND Window = nullptr; int64_t Area = 0; } search{target, &wallpaperPids};
        const auto inspect = [](HWND window, LPARAM param) -> BOOL
        {
            auto& s = *reinterpret_cast<Search*>(param);
            wchar_t cls[128]{}; GetClassNameW(window, cls, ARRAYSIZE(cls));
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            if (!s.Pids->contains(pid) && _wcsicmp(cls, L"WallpaperEngine") != 0 && _wcsicmp(cls, L"wpeng") != 0) return TRUE;
            RECT r{};
            if (!IsWindowVisible(window) || !GetWindowRect(window, &r)) return TRUE;
            const int64_t width = std::max(0L, std::min(r.right, static_cast<LONG>(s.Target.Right)) - std::max(r.left, static_cast<LONG>(s.Target.Left)));
            const int64_t height = std::max(0L, std::min(r.bottom, static_cast<LONG>(s.Target.Bottom)) - std::max(r.top, static_cast<LONG>(s.Target.Top)));
            if (width * height > s.Area) { s.Area = width * height; s.Window = window; }
            return TRUE;
        };
        struct EnumContext { Search* State; WNDENUMPROC Inspect; } context{&search, inspect};
        EnumWindows([](HWND window, LPARAM p) -> BOOL
        {
            const auto& c = *reinterpret_cast<EnumContext*>(p);
            c.Inspect(window, reinterpret_cast<LPARAM>(c.State));
            EnumChildWindows(window, c.Inspect, reinterpret_cast<LPARAM>(c.State));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return search.Window;
    }
}
