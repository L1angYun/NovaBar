// NovaBar.XamlBridge —— 注入 explorer.exe，通过 XAML 诊断改任务栏外观。
//
// 与 TranslucentTB 的 ExplorerTAP 相比，这里不做「父节点 == XamlSource.Content()」
// 的匹配：26xxx 起任务栏外面多了 ScrollViewer 等容器，任何固定层数的假设都会失效。
// 本实现改为「沿祖先链找 Taskbar.TaskbarFrame」，与层数无关。
//
// 运行期通过 %LOCALAPPDATA%\NovaBar\state.txt 热更新外观（Engine 负责写）。

#define NOMINMAX
#include <windows.h>
#undef GetCurrentTime

#include <ocidl.h>
#include <robuffer.h>
#include <wincodec.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include "GlassPixels.h"
#include "LiquidSurfaceClient.h"
#include "ShellSurfaceStyler.h"
#include "ShellVisibility.h"
#include "../NovaBar.Shared/BridgeIdentity.h"
#include <xamlOM.h>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <windows.graphics.effects.interop.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

// Win2D 投影由 build\native-build.ps1 生成到 artifacts\win2d-gen。
// 没有它也能编译 —— 桥接层会退回 CPU 折射。
#if __has_include(<winrt/Microsoft.Graphics.Canvas.h>)
#define NOVABAR_HAS_WIN2D 1
#include <winrt/Microsoft.Graphics.Canvas.h>
#include <winrt/Microsoft.Graphics.Canvas.Effects.h>
#endif

#include <roapi.h>
#include <winstring.h>

namespace
{
    // roapi.h 的 typedef 在部分配置下不可见，自己声明一份
    using PFN_GET_ACTIVATION_FACTORY = HRESULT(WINAPI*)(HSTRING, IActivationFactory**);
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wux = winrt::Windows::UI::Xaml;
namespace wuc = winrt::Windows::UI::Xaml::Controls;
namespace wum = winrt::Windows::UI::Xaml::Media;

namespace
{
    constexpr auto& kReadyEventName = novabar::bridge_identity::ReadyEvent;
    constexpr auto& kFailedEventName = novabar::bridge_identity::FailedEvent;
    constexpr auto kSiteClsidValue = novabar::bridge_identity::SiteClsid;

    HMODULE g_module = nullptr;
    std::wstring g_statePath;
    bool g_taskbarHost = false;
    std::once_flag g_pathInitialization;
    void SignalEvent(const wchar_t* name);
    std::mutex g_logMutex;
    std::atomic<bool> g_installStarted{false};

    using PFN_InitializeXamlDiagnosticsEx = HRESULT(WINAPI*)(
        PCWSTR endPointName,
        DWORD pid,
        PCWSTR dllXamlDiagnostics,
        PCWSTR tapDllPath,
        REFCLSID tapClsid,
        PCWSTR senderName);

    std::wstring LocalAppData()
    {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
        return len > 0 ? std::wstring(buffer, len) : std::wstring();
    }

    std::wstring DataDir()
    {
        static const std::wstring dir = []
        {
            std::wstring path = LocalAppData() + L"\\NovaBar";
            if (!g_statePath.empty()) path = g_statePath.substr(0, g_statePath.find_last_of(L"\\/"));
            CreateDirectoryW(path.c_str(), nullptr);
            return path;
        }();

        return dir;
    }

    std::wstring LogPath()
    {
        static const std::wstring path = DataDir() + L"\\bridge.log";
        return path;
    }

    void Log(const wchar_t* format, ...)
    {
        wchar_t message[1024]{};

        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, format, args);
        va_end(args);

        SYSTEMTIME now{};
        GetLocalTime(&now);

        std::lock_guard lock(g_logMutex);

        FILE* file = nullptr;
        if (_wfopen_s(&file, LogPath().c_str(), L"a, ccs=UTF-8") == 0 && file)
        {
            fwprintf(file, L"[%02d:%02d:%02d.%03d] %ls\n",
                     now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
            fclose(file);
        }

        OutputDebugStringW(message);
        OutputDebugStringW(L"\n");
    }

    // ───────────────────────── 外观规格 ─────────────────────────

    enum class Accent
    {
        Normal,
        Clear,
        Color,
        Blur,
        Acrylic,
        LiquidGlass,
    };

    struct Spec
    {
        Accent Accent = Accent::Clear;
        unsigned Color = 0x00000000;   // AARRGGBB
        double Blur = 9;
        bool ShowLine = false;
        double Specular = 0.35;
        double TintStrength = 0.30;
        double Lens = 0.55;            // 折射强度
        double Dispersion = 0.18;
        double Noise = 0.12;
        unsigned Version = 0;
    };

    unsigned ParseAlpha(unsigned argb) { return (argb >> 24) & 0xFF; }

    winrt::Windows::UI::Color ToColor(unsigned argb)
    {
        winrt::Windows::UI::Color color{};
        color.A = static_cast<uint8_t>((argb >> 24) & 0xFF);
        color.R = static_cast<uint8_t>((argb >> 16) & 0xFF);
        color.G = static_cast<uint8_t>((argb >> 8) & 0xFF);
        color.B = static_cast<uint8_t>(argb & 0xFF);
        return color;
    }

    unsigned ParseHex(const std::wstring& text)
    {
        auto nibble = [](wchar_t c) -> unsigned
        {
            if (c >= L'0' && c <= L'9')
            {
                return c - L'0';
            }

            if (c >= L'a' && c <= L'f')
            {
                return c - L'a' + 10;
            }

            if (c >= L'A' && c <= L'F')
            {
                return c - L'A' + 10;
            }

            return 0;
        };

        std::wstring value = text;
        if (!value.empty() && value.front() == L'#')
        {
            value.erase(value.begin());
        }

        if (value.size() != 8)
        {
            return 0;
        }

        unsigned result = 0;
        for (const wchar_t c : value)
        {
            result = (result << 4) | nibble(c);
        }

        return result;
    }

    /// 解析 "accent=x;color=AARRGGBB;blur=9;line=1;specular=0.3;tint=0.3"
    Spec ParseSpec(const std::wstring& text)
    {
        Spec spec;

        size_t start = 0;
        while (start < text.size())
        {
            const size_t end = text.find(L';', start);
            const std::wstring pair = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            start = end == std::wstring::npos ? text.size() : end + 1;

            const auto eq = pair.find(L'=');
            if (eq == std::wstring::npos)
            {
                continue;
            }

            const std::wstring key = pair.substr(0, eq);
            const std::wstring value = pair.substr(eq + 1);

            if (key == L"accent")
            {
                if (value == L"normal")
                {
                    spec.Accent = Accent::Normal;
                }
                else if (value == L"clear")
                {
                    spec.Accent = Accent::Clear;
                }
                else if (value == L"color" || value == L"opaque")
                {
                    spec.Accent = Accent::Color;
                }
                else if (value == L"acrylic")
                {
                    spec.Accent = Accent::Acrylic;
                }
                else if (value == L"blur")
                {
                    spec.Accent = Accent::Blur;
                }
                else if (value == L"liquid")
                {
                    spec.Accent = Accent::LiquidGlass;
                }
            }
            else if (key == L"color")
            {
                spec.Color = ParseHex(value);
            }
            else if (key == L"blur")
            {
                spec.Blur = _wtof(value.c_str());
            }
            else if (key == L"line")
            {
                spec.ShowLine = value == L"1";
            }
            else if (key == L"specular")
            {
                spec.Specular = _wtof(value.c_str());
            }
            else if (key == L"tint")
            {
                spec.TintStrength = _wtof(value.c_str());
            }
            else if (key == L"lens")
            {
                spec.Lens = _wtof(value.c_str());
            }
            else if (key == L"dispersion")
            {
                spec.Dispersion = _wtof(value.c_str());
            }
            else if (key == L"noise")
            {
                spec.Noise = _wtof(value.c_str());
            }
        }

        const auto finiteClamp = [](double value, double fallback, double maximum)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0, maximum) : fallback;
        };
        spec.Lens = finiteClamp(spec.Lens, 0.55, 1.0);
        spec.Dispersion = finiteClamp(spec.Dispersion, 0.18, 1.0);
        spec.Noise = finiteClamp(spec.Noise, 0.12, 1.0);
        spec.Blur = finiteClamp(spec.Blur, 9.0, 750.0);
        spec.Specular = finiteClamp(spec.Specular, 0.35, 1.0);
        spec.TintStrength = finiteClamp(spec.TintStrength, 0.30, 1.0);

        return spec;
    }

    // ───────────────────────── 监视器 ─────────────────────────

    // ───────────────────────── DWM 窗口级模糊 ─────────────────────────
    //
    // 这是「液态玻璃」和「磨砂」的分界线：
    // XAML 的 AcrylicBrush 会叠一层乳白并降饱和，看起来像磨砂；
    // DWM 的亚克力只做模糊 + 轻微提亮，颜色仍然鲜艳地透出来。
    // ZenDesktop 的液态玻璃做法是「隐藏 XAML 背景 + 交给窗口模糊」，这里同理。

    enum
    {
        kAccentDisabled = 0,
        kAccentEnableAcrylic = 4,
        kCompositionAttribAccentPolicy = 19,
    };

    struct AccentPolicy
    {
        int State;
        int Flags;
        int GradientColor;
        int AnimationId;
    };

    struct CompositionAttribData
    {
        int Attribute;
        void* Data;
        size_t SizeOfData;
    };

    bool SetWindowGlass(HWND window, bool enable, unsigned argb, double strength)
    {
        using PFN = BOOL(WINAPI*)(HWND, CompositionAttribData*);

        static const PFN setAttribute = reinterpret_cast<PFN>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));

        if (!setAttribute || !window)
        {
            return false;
        }

        AccentPolicy policy{};
        policy.State = enable ? kAccentEnableAcrylic : kAccentDisabled;

        if (enable)
        {
            const unsigned alpha = static_cast<unsigned>(((argb >> 24) & 0xFF) * strength);
            const unsigned red = (argb >> 16) & 0xFF;
            const unsigned green = (argb >> 8) & 0xFF;
            const unsigned blue = argb & 0xFF;

            // GradientColor 是 AABBGGRR，注意字节序与 ARGB 相反
            policy.GradientColor = static_cast<int>(
                ((alpha & 0xFF) << 24) | ((blue & 0xFF) << 16) | ((green & 0xFF) << 8) | (red & 0xFF));
        }

        CompositionAttribData data{kCompositionAttribAccentPolicy, &policy, sizeof(policy)};
        return setAttribute(window, &data) != FALSE;
    }

    // ───────────────────────── 壁纸折射（真 lensing）─────────────────────────
    //
    // 桌面状态下任务栏背后就是壁纸。把壁纸按"透镜"重新采样成一条背景图：
    //   1. 靠近玻璃边缘时采样位置向内偏移 —— 这就是弯折
    //   2. R/G/B 用略微不同的偏移量 —— 这就是色散，边缘会出现彩色描边
    //   3. 叠色调
    // 有窗口盖住任务栏时不能用（背后不是壁纸），退回 DWM 模糊。

    bool SetWindowMaterial(HWND window, const Spec& spec)
    {
        using PFN = BOOL(WINAPI*)(HWND, CompositionAttribData*);
        static const auto set = reinterpret_cast<PFN>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
        if (!set || !window) return false;
        AccentPolicy policy{};
        policy.State = spec.Accent == Accent::Normal || spec.Accent == Accent::Color ? 0
            : (spec.Accent == Accent::Clear || spec.Accent == Accent::LiquidGlass) ? 2 : spec.Accent == Accent::Blur ? 3 : 4;
        const auto argb = spec.Color;
        const double strength = spec.Accent == Accent::LiquidGlass ? spec.TintStrength : 1.0;
        const unsigned alpha = static_cast<unsigned>(((argb >> 24) & 255) * strength);
        policy.GradientColor = (spec.Accent == Accent::Clear || spec.Accent == Accent::LiquidGlass) ? 0 : static_cast<int>((alpha << 24)
            | ((argb & 255) << 16) | (argb & 0xFF00) | ((argb >> 16) & 255));
        CompositionAttribData data{kCompositionAttribAccentPolicy, &policy, sizeof(policy)};
        return set(window, &data) != FALSE;
    }

    novabar::ShellSurfaceStyle ShellStyle(const Spec& spec)
    {
        novabar::ShellSurfaceStyle result;
        result.Accent = spec.Accent == Accent::Normal ? novabar::ShellAccent::Normal
            : spec.Accent == Accent::Clear ? novabar::ShellAccent::Clear
            : spec.Accent == Accent::Color ? novabar::ShellAccent::Color
            : spec.Accent == Accent::LiquidGlass ? novabar::ShellAccent::LiquidGlass : novabar::ShellAccent::Acrylic;
        result.Color = spec.Color; result.TintStrength = spec.TintStrength; result.ShowLine = spec.ShowLine;
        result.Blur = spec.Blur; result.Lens = spec.Lens; result.Specular = spec.Specular;
        result.Dispersion = spec.Dispersion; result.Noise = spec.Noise;
        return result;
    }

    double SheenStrength(const Spec& spec)
    {
        return spec.Accent == Accent::LiquidGlass ? spec.Specular : 0.0;
    }

    bool TryReadSpecFile(const std::wstring& path, Spec& spec)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || !file) return false;
        wchar_t buffer[2048]{};
        const bool read = fgetws(buffer, ARRAYSIZE(buffer), file) != nullptr;
        fclose(file);
        if (!read) return false;
        std::wstring text(buffer);
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
        if (text.empty()) return false;
        spec = ParseSpec(text);
        return true;
    }

    bool IsTaskbarRootAdd(VisualElement const& element, VisualMutationType mutation) noexcept
    {
        return mutation == Add && element.Type && wcscmp(element.Type, L"Taskbar.TaskbarFrame") == 0;
    }

    struct TaskbarShapeClaim
    {
        winrt::weak_ref<winrt::Windows::Foundation::IInspectable> Element;
        void* Identity = nullptr;
        std::atomic<bool> Active{true};
    };

    // A new Explorer diagnostics site can enumerate an existing taskbar again.
    // Each watcher keeps its claims; the shared registry holds only weak claims
    // and never retains a XAML object across its lifetime or UI thread.
    class TaskbarShapeOwnership
    {
        std::unordered_map<InstanceHandle, std::shared_ptr<TaskbarShapeClaim>> m_claims;
    public:
        ~TaskbarShapeOwnership()
        { for (auto const& item : m_claims) item.second->Active = false; }
        bool Claim(InstanceHandle handle, winrt::Windows::Foundation::IInspectable const& element)
        {
            const auto identity = novabar::surface_detail::CanonicalIdentity(element);
            if (const auto found = m_claims.find(handle); found != m_claims.end())
            {
                if (found->second->Active && found->second->Identity == identity
                    && found->second->Element.get()) return true;
                Remove(handle);
            }
            auto claim = std::make_shared<TaskbarShapeClaim>();
            claim->Element = winrt::make_weak(element);
            claim->Identity = identity;
            static novabar::surface_detail::WeakOwners<TaskbarShapeClaim> owners;
            if (!owners.Claim(GetCurrentThreadId(), identity, claim, [](auto const& previous, DWORD thread)
                {
                    if (!previous->Active) return false;
                    if (thread != GetCurrentThreadId()) return true;
                    try { return bool(previous->Element.get()); } catch (...) { return false; }
                })) return false;
            m_claims.emplace(handle, std::move(claim));
            return true;
        }
        void Remove(InstanceHandle handle)
        {
            if (const auto found = m_claims.find(handle); found != m_claims.end())
            { found->second->Active = false; m_claims.erase(found); }
        }
    };

    struct Watcher : winrt::implements<Watcher, IVisualTreeServiceCallback2, winrt::non_agile>
    {
        explicit Watcher(winrt::com_ptr<IXamlDiagnostics> diagnostics) :
            m_diagnostics(std::move(diagnostics))
        {
            m_queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            m_visibility = std::make_unique<novabar::ShellVisibility>(g_statePath);
            m_shellStyler.SetCaptureDirectory(DataDir());
            // Establish the requested shell style before diagnostics enumerate
            // the first tree; the poller's first scheduling slice is too late
            // to prevent a native opaque opening frame.
            try { ReloadIfChanged(); } catch (...) {}
            StartPolling();
        }

        ~Watcher()
        {
            m_stop = true;
            if (m_poller.joinable())
            {
                m_poller.join();
            }

            if (m_renderingToken.value != 0)
            {
                wux::Media::CompositionTarget::Rendering(m_renderingToken);
                m_renderingToken = {};
            }
        }

        HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
            ParentChildRelation relation,
            VisualElement element,
            VisualMutationType mutationType) override;

        HRESULT STDMETHODCALLTYPE OnElementStateChanged(
            InstanceHandle /*element*/,
            VisualElementState /*state*/,
            LPCWSTR /*context*/) noexcept override
        {
            return S_OK;
        }

    private:
        winrt::Windows::Foundation::IInspectable FromHandle(InstanceHandle handle) const
        {
            winrt::Windows::Foundation::IInspectable object{nullptr};
            winrt::check_hresult(m_diagnostics->GetIInspectableFromHandle(
                handle,
                reinterpret_cast<::IInspectable**>(winrt::put_abi(object))));
            return object;
        }

        InstanceHandle HandleOf(winrt::Windows::Foundation::IInspectable const& object) const
        {
            InstanceHandle handle = 0;
            winrt::check_hresult(m_diagnostics->GetHandleFromIInspectable(
                reinterpret_cast<::IInspectable*>(winrt::get_abi(object)),
                &handle));
            return handle;
        }

        bool HasRootAncestor(wux::DependencyObject const& start, const wchar_t* rootType) const
        {
            auto current = start;
            for (int depth = 0; depth < 32; ++depth)
            {
                const auto parent = wum::VisualTreeHelper::GetParent(current);
                if (!parent)
                {
                    return false;
                }

                const auto it = m_types.find(HandleOf(parent));
                if (it != m_types.end() && it->second == rootType)
                {
                    return true;
                }

                current = parent;
            }

            return false;
        }

        void Register(wux::Shapes::Shape const& shape, bool stroke);
        void ApplyAll();
        void ApplyOne(wux::Shapes::Shape const& shape, bool stroke);
        void EnsureSheen(wux::Shapes::Shape const& fill);
        void StartPolling();
        void ReloadIfChanged();
        HWND TaskbarFor(wux::Shapes::Shape const& shape);
        void OnRendering(winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&);

        winrt::com_ptr<IXamlDiagnostics> m_diagnostics;
        winrt::Windows::System::DispatcherQueue m_queue{nullptr};
        const DWORD m_ownerThread = GetCurrentThreadId();
        std::mutex m_childrenMutex;
        std::map<DWORD, winrt::com_ptr<Watcher>> m_threadWatchers;

        std::unordered_map<InstanceHandle, std::wstring> m_types;
        std::unordered_set<InstanceHandle> m_roots;

        std::vector<wux::Shapes::Shape> m_fills;
        std::vector<wux::Shapes::Shape> m_strokes;

        novabar::ShellSurfaceStyler m_shellStyler;
        std::unique_ptr<novabar::ShellVisibility> m_visibility;
        std::vector<std::pair<InstanceHandle, winrt::weak_ref<wux::Hosting::DesktopWindowXamlSource>>> m_sources;
        TaskbarShapeOwnership m_shapeOwnership;
        std::unordered_map<InstanceHandle, wux::Shapes::Shape> m_shapeHandles;
        std::unordered_map<InstanceHandle, std::shared_ptr<novabar::LiquidSurfaceClient>> m_liquidClients;

        struct Sheen
        {
            wuc::Panel Parent{nullptr};
            wux::Shapes::Rectangle Shape{nullptr};
            wum::TranslateTransform Transform{nullptr};
            HWND Window = nullptr;
            double LastT = -1, LastOpacity = -1;
        };
        std::unordered_map<InstanceHandle, Sheen> m_sheens;
        winrt::event_token m_renderingToken{};

        // 这个进程里有没有任务栏？有才能借窗口模糊；开始菜单宿主没有，要回退到 XAML 亚克力。
        bool m_isTaskbarHost = false;
        bool m_hostKnown = false;
        int m_dumpCount = 0;

        // 记录原始画刷，`accent=normal` 时原样还回去。
        // 用 vector 而非 map：Brush 是 WinRT 类，没有默认构造函数，不能走 operator[]。
        struct OriginalFill { InstanceHandle Handle; wux::Media::Brush Brush; wux::Visibility Visibility; };
        std::vector<OriginalFill> m_originalFills;

        std::mutex m_specMutex;
        Spec m_spec;
        unsigned long long m_lastWrite = 0;

        std::atomic<bool> m_stop{false};
        std::thread m_poller;
    };

    void Watcher::StartPolling()
    {
        m_poller = std::thread([this]
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            while (!m_stop)
            {
                const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(16);
                try { ReloadIfChanged(); }
                catch (...) { Log(L"后台刷新失败: 0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
                std::this_thread::sleep_until(next);
            }
            winrt::uninit_apartment();
        });
    }

    HWND Watcher::TaskbarFor(wux::Shapes::Shape const& shape)
    {
        for (const auto& [sourceHandle, sourceWeak] : m_sources)
        {
            try
            {
                const auto source = sourceWeak.get();
                if (!source) continue;
                for (wux::DependencyObject node = shape; node; node = wum::VisualTreeHelper::GetParent(node))
                {
                    if (source.Content() == node)
                    {
                        auto native = source.try_as<IDesktopWindowXamlSourceNative>();
                        HWND island = nullptr;
                        if (native && SUCCEEDED(native->get_WindowHandle(&island))) return GetAncestor(island, GA_ROOT);
                    }
                }
            }
            catch (...) {}
        }
        struct Context { DWORD Thread; double Width; HWND Best = nullptr; double Distance = 1e20; } context{GetCurrentThreadId(), shape.ActualWidth()};
        EnumWindows([](HWND window, LPARAM p) -> BOOL
        {
            auto& c = *reinterpret_cast<Context*>(p);
            wchar_t cls[64]{}; GetClassNameW(window, cls, ARRAYSIZE(cls));
            if (wcscmp(cls, L"Shell_TrayWnd") && wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
            RECT r{}; GetWindowRect(window, &r);
            const double scale = GetDpiForWindow(window) / 96.0;
            const double distance = std::abs((r.right - r.left) / scale - c.Width)
                + (GetWindowThreadProcessId(window, nullptr) == c.Thread ? 0 : 100000);
            if (distance < c.Distance) { c.Best = window; c.Distance = distance; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.Best;
    }

    void Watcher::OnRendering(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Windows::Foundation::IInspectable const&)
    {
        Spec spec;
        { std::lock_guard lock(m_specMutex); spec = m_spec; }
        POINT cursor{}; if (!GetCursorPos(&cursor)) return;
        for (auto& [handle, sheen] : m_sheens)
        {
            try
            {
                RECT bounds{};
                if (!GetWindowRect(sheen.Window, &bounds) || bounds.right <= bounds.left) continue;
                const double t = std::clamp(static_cast<double>(cursor.x - bounds.left) / (bounds.right - bounds.left), 0.0, 1.0);
                const double hover = std::clamp(1.0 - std::max(0L, bounds.top - cursor.y) / 260.0, 0.0, 1.0);
                const double opacity = SheenStrength(spec) * hover;
                if (std::abs(t - sheen.LastT) < .002 && std::abs(opacity - sheen.LastOpacity) < .01) continue;
                sheen.LastT = t; sheen.LastOpacity = opacity;
                sheen.Transform.X((t - .5) * sheen.Parent.ActualWidth());
                sheen.Shape.Opacity(opacity);
            }
            catch (...) {}
        }
    }

    void Watcher::ReloadIfChanged()
    {
        const std::wstring path = g_statePath.empty() ? DataDir() + L"\\" + novabar::bridge_identity::TaskbarState : g_statePath;

        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        {
            return;
        }

        const unsigned long long stamp =
            (static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32) |
            info.ftLastWriteTime.dwLowDateTime;

        {
            std::lock_guard lock(m_specMutex);
            if (stamp == m_lastWrite)
            {
                return;
            }

        }
        Spec spec;
        if (!TryReadSpecFile(path, spec)) return;

        {
            std::lock_guard lock(m_specMutex);
            const unsigned version = m_spec.Version + 1;
            m_spec = spec; m_spec.Version = version;
            m_lastWrite = stamp;
        }

        Log(L"收到新外观（版本 %u）", m_spec.Version);

        if (!g_taskbarHost) { m_shellStyler.Apply(ShellStyle(spec)); return; }
        if (m_queue)
        {
            auto weak = get_weak();
            m_queue.TryEnqueue([weak] { if (auto self = weak.get()) self->ApplyAll(); });
        }
    }

    void Watcher::Register(wux::Shapes::Shape const& shape, bool stroke)
    {
        const auto handle = HandleOf(shape);
        // Claim before saving brush values, creating sheen/render timers, or
        // installing a frame client. A non-owner must remain read-only.
        if (!m_shapeOwnership.Claim(handle, shape)) return;
        m_shapeHandles.insert_or_assign(handle, shape);
        auto& list = stroke ? m_strokes : m_fills;
        if (std::find(list.begin(), list.end(), shape) == list.end())
        {
            list.push_back(shape);

            if (stroke) m_originalFills.push_back({HandleOf(shape), shape.Fill(), shape.Visibility()});
            if (!stroke)
            {
                // 只在第一次登记时记原始画刷
                const auto handle = HandleOf(shape);
                const bool known = std::any_of(
                    m_originalFills.begin(), m_originalFills.end(),
                    [handle](auto const& pair) { return pair.Handle == handle; });

                if (!known)
                {
                    m_originalFills.push_back({handle, shape.Fill(), shape.Visibility()});
                }

                if (!m_hostKnown)
                {
                    m_isTaskbarHost = HasRootAncestor(shape, L"Taskbar.TaskbarFrame");
                    m_hostKnown = true;
                    Log(m_isTaskbarHost ? L"宿主类型：任务栏" : L"宿主类型：非任务栏（回退 XAML 亚克力）");
                }

                EnsureSheen(shape);
            }
        }

        ApplyOne(shape, stroke);
    }

    void Watcher::ApplyAll()
    {
        for (const auto& fill : m_fills)
        {
            ApplyOne(fill, false);
        }

        for (const auto& stroke : m_strokes)
        {
            ApplyOne(stroke, true);
        }
    }

    void Watcher::EnsureSheen(wux::Shapes::Shape const& fill)
    {
        const auto handle = HandleOf(fill);
        if (m_sheens.contains(handle)) return;
        try
        {
            const auto parent = wum::VisualTreeHelper::GetParent(fill).try_as<wuc::Panel>();
            if (!parent) return;
            wux::Shapes::Rectangle shape; shape.IsHitTestVisible(false); shape.Opacity(0);
            wum::LinearGradientBrush brush;
            brush.StartPoint({0, 0}); brush.EndPoint({1, 1});
            const std::pair<unsigned, double> stops[] = {{0,0},{0,.36},{0x3AFFFFFF,.5},{0,.64},{0,1}};
            for (auto [color, offset] : stops)
            {
                wum::GradientStop stop; stop.Color(ToColor(color)); stop.Offset(offset); brush.GradientStops().Append(stop);
            }
            shape.Fill(brush);
            wum::TranslateTransform transform; shape.RenderTransform(transform);
            uint32_t index = 0;
            if (parent.Children().IndexOf(fill, index)) parent.Children().InsertAt(index + 1, shape);
            else parent.Children().Append(shape);
            m_sheens.emplace(handle, Sheen{parent, shape, transform, TaskbarFor(fill)});
            if (!m_renderingToken.value) m_renderingToken = wum::CompositionTarget::Rendering({get_weak(), &Watcher::OnRendering});
        }
        catch (...) { Log(L"高光层创建失败: 0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
    }

    void Watcher::ApplyOne(wux::Shapes::Shape const& shape, bool stroke)
    {
        Spec spec;
        { std::lock_guard lock(m_specMutex); spec = m_spec; }
        const auto handle = HandleOf(shape);
        if (spec.Accent != Accent::LiquidGlass)
        {
            if (auto client = m_liquidClients.find(handle); client != m_liquidClients.end())
            { client->second->Stop(); m_liquidClients.erase(client); }
        }
        if (spec.Accent == Accent::Normal)
        {
            for (const auto& original : m_originalFills) if (original.Handle == handle)
            { shape.Fill(original.Brush); shape.Visibility(original.Visibility); break; }
            if (!stroke) SetWindowMaterial(TaskbarFor(shape), spec);
            if (auto found = m_sheens.find(handle); found != m_sheens.end()) found->second.Shape.Opacity(0);
            return;
        }
        if (stroke)
        {
            wum::SolidColorBrush brush; brush.Color(ToColor(spec.ShowLine ? 0x40FFFFFF : 0));
            shape.Fill(brush); shape.Visibility(wux::Visibility::Visible); return;
        }
        const HWND window = TaskbarFor(shape);
        if (spec.Accent == Accent::LiquidGlass)
        {
            shape.Visibility(wux::Visibility::Visible);
            Spec transparent = spec; transparent.Accent = Accent::Clear;
            SetWindowMaterial(window, transparent);
            auto found = m_liquidClients.find(handle);
            if (found == m_liquidClients.end())
            {
                auto weakShape = winrt::make_weak(shape);
                auto weakSelf = get_weak();
                auto client = std::make_shared<novabar::LiquidSurfaceClient>(shape, DataDir(),
                    novabar::capture::Kind::Taskbar, window,
                    [weakShape, weakSelf](auto const& image, bool)
                    {
                        auto self = weakSelf.get(); auto target = weakShape.get();
                        if (!self || !target) return;
                        { std::lock_guard lock(self->m_specMutex); if (self->m_spec.Accent != Accent::LiquidGlass) return; }
                        if (target.Fill() != image) target.Fill(image);
                        target.Visibility(wux::Visibility::Visible);
                    });
                found = m_liquidClients.emplace(handle, std::move(client)).first;
            }
            novabar::LiquidAppearance appearance;
            appearance.Material = {spec.Lens, spec.Dispersion, spec.Noise, spec.TintStrength, spec.Blur, spec.Color};
            appearance.Specular = spec.Specular;
            found->second->SetAppearance(appearance);
            shape.Fill(found->second->CurrentBrush());
            if (auto sheen = m_sheens.find(handle); sheen != m_sheens.end()) sheen->second.Shape.Opacity(spec.Specular);
            return;
        }
        if (auto found = m_sheens.find(handle); found != m_sheens.end()) found->second.Shape.Opacity(0);
        SetWindowMaterial(window, spec);
        if (spec.Accent == Accent::Clear || spec.Accent == Accent::Blur || spec.Accent == Accent::Acrylic)
        { shape.Visibility(wux::Visibility::Collapsed); return; }
        wum::SolidColorBrush brush; brush.Color(ToColor(spec.Color | 0xFF000000));
        shape.Fill(brush); shape.Visibility(wux::Visibility::Visible);
    }

    HRESULT Watcher::OnVisualTreeChange(
        ParentChildRelation /*relation*/,
        VisualElement element,
        VisualMutationType mutationType)
    {
        try
        {
            // Explorer also hosts unrelated XAML islands. Claim another UI
            // thread only when it reports a taskbar root; existing taskbar
            // watchers still receive every callback, including removals.
            if (g_taskbarHost && GetCurrentThreadId() != m_ownerThread)
            {
                winrt::com_ptr<Watcher> watcher;
                {
                    std::lock_guard lock(m_childrenMutex);
                    const DWORD thread = GetCurrentThreadId();
                    auto found = m_threadWatchers.find(thread);
                    if (found == m_threadWatchers.end())
                    {
                        if (!IsTaskbarRootAdd(element, mutationType)) return S_OK;
                        found = m_threadWatchers.emplace(thread, winrt::make_self<Watcher>(m_diagnostics)).first;
                    }
                    watcher = found->second;
                }
                return watcher->OnVisualTreeChange({}, element, mutationType);
            }
            if (!g_taskbarHost)
            {
                if (mutationType == Add)
                {
                    const std::wstring type = element.Type ? element.Type : L"";
                    const std::wstring name = element.Name ? element.Name : L"";
                    const bool window = type == L"Windows.UI.Xaml.Window";
                    const bool source = type == L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource";
                    const bool border = type == L"Windows.UI.Xaml.Controls.Border"
                        && (name == L"AcrylicBorder" || name == L"AcrylicOverlay" || name == L"RootGridBorder" || name.empty());
                    const bool grid = type == L"Windows.UI.Xaml.Controls.Grid"
                        && (name == L"NotificationCenterGrid" || name == L"CalendarCenterGrid" || name == L"ControlCenterRegion");
                    if (!window && !source && !border && !grid) return S_OK;
                    try
                    {
                        const auto object = FromHandle(element.Handle);
                        if (source) m_shellStyler.RegisterXamlSource(object);
                        else if (window) m_visibility->Register(object, type);
                        else if (const auto dependency = object.try_as<wux::DependencyObject>())
                            if (m_shellStyler.Register(dependency, type, name)) Log(L"登记 shell 背景 %ls #%ls", type.c_str(), name.c_str());
                    }
                    catch (...)
                    {
                        Log(L"shell 候选元素解析/登记失败 %ls #%ls: 0x%08X", type.c_str(), name.c_str(),
                            static_cast<unsigned>(winrt::to_hresult()));
                    }
                }
                return S_OK;
            }
            if (mutationType == Add)
            {
                const std::wstring type = element.Type ? element.Type : L"";
                m_types[element.Handle] = type;
                if (type == L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource")
                    if (auto source = FromHandle(element.Handle).try_as<wux::Hosting::DesktopWindowXamlSource>()) m_sources.emplace_back(element.Handle, winrt::make_weak(source));

                // 一次性转储：非任务栏宿主（开始菜单等）的元素命名和任务栏不同，
                // 需要先看清结构再定选择器。上限 200 条，避免刷爆日志。
                if (!m_isTaskbarHost && m_dumpCount < 200)
                {
                    const std::wstring name = element.Name ? element.Name : L"";
                    const bool interesting = !name.empty()
                        || type.find(L"Border") != std::wstring::npos
                        || type.find(L"Root") != std::wstring::npos
                        || type.find(L"Panel") != std::wstring::npos
                        || type.find(L"Rectangle") != std::wstring::npos;

                    if (interesting)
                    {
                        Log(L"[dump] %ls #%ls", type.c_str(), name.c_str());
                        ++m_dumpCount;
                    }
                }

                if (type == L"Taskbar.TaskbarFrame")
                {
                    m_roots.insert(element.Handle);
                    Log(L"发现任务栏根节点（handle=%llu）",
                        static_cast<unsigned long long>(element.Handle));
                }
                else if (type.find(L"Shapes.Rectangle") != std::wstring::npos)
                {
                    const std::wstring name = element.Name ? element.Name : L"";
                    const bool isFill = name == L"BackgroundFill";
                    const bool isStroke = name == L"BackgroundStroke";

                    if (isFill || isStroke)
                    {
                        const auto shape = FromHandle(element.Handle).try_as<wux::Shapes::Shape>();
                        if (shape)
                        {
                            // 每个宿主进程只承载一类界面：explorer 里是任务栏，
                            // StartMenuExperienceHost 里是开始菜单。所以这里不再要求
                            // 必须挂在 TaskbarFrame 下，否则开始菜单永远命中不了。
                            if (HasRootAncestor(shape, L"Taskbar.TaskbarFrame"))
                            {
                                Log(L"登记任务栏背景元素 %ls（pid=%lu）",
                                    name.c_str(), GetCurrentProcessId());
                            }
                            else
                            {
                                Log(L"登记非任务栏背景元素 %ls（pid=%lu）",
                                    name.c_str(), GetCurrentProcessId());
                            }

                            if (HasRootAncestor(shape, L"Taskbar.TaskbarFrame")) Register(shape, isStroke);
                        }
                    }
                }
            }
            else if (mutationType == Remove)
            {
                const auto handle = element.Handle;
                m_roots.erase(handle);
                m_types.erase(handle);
                std::erase_if(m_sources, [=](auto const& entry) { return entry.first == handle; });
                if (auto found = m_sheens.find(handle); found != m_sheens.end())
                {
                    uint32_t index = 0;
                    if (found->second.Parent.Children().IndexOf(found->second.Shape, index)) found->second.Parent.Children().RemoveAt(index);
                    m_sheens.erase(found);
                }
                if (m_sheens.empty() && m_renderingToken.value)
                { wum::CompositionTarget::Rendering(m_renderingToken); m_renderingToken = {}; }
                if (auto found = m_shapeHandles.find(handle); found != m_shapeHandles.end())
                {
                    const auto removed = found->second;
                    std::erase(m_fills, removed); std::erase(m_strokes, removed); m_shapeHandles.erase(found);
                }
                if (auto client = m_liquidClients.find(handle); client != m_liquidClients.end())
                { client->second->Stop(); m_liquidClients.erase(client); }
                std::erase_if(m_originalFills, [=](auto const& entry) { return entry.Handle == handle; });
                m_shapeOwnership.Remove(handle);

            }
        }
        catch (...)
        {
            return winrt::to_hresult();
        }

        return S_OK;
    }

    // ───────────────────────── 注入 ─────────────────────────

    struct Site : winrt::implements<Site, IObjectWithSite, winrt::non_agile>
    {
        HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override
        {
            try
            {
                if (!site)
                {
                    return S_OK;
                }

                winrt::com_ptr<IUnknown> unknown;
                unknown.copy_from(site);

                auto diagnostics = unknown.as<IXamlDiagnostics>();
                std::call_once(g_pathInitialization, [&]
                {
                    BSTR initialization = nullptr;
                    if (SUCCEEDED(diagnostics->GetInitializationData(&initialization)) && initialization)
                        g_statePath.assign(initialization, SysStringLen(initialization));
                    SysFreeString(initialization);
                    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
                    const wchar_t* filename = wcsrchr(exe, L'\\');
                    g_taskbarHost = _wcsicmp(filename ? filename + 1 : exe, L"explorer.exe") == 0;
                });
                auto watcher = winrt::make_self<Watcher>(diagnostics);
                m_watcher = watcher;

                Log(L"已拿到 IXamlDiagnostics，开始订阅可视化树");

                const auto service = diagnostics.as<IVisualTreeService3>();
                winrt::check_hresult(service->AdviseVisualTreeChange(watcher.get()));

                Log(L"订阅完成");
                const auto readyPath = DataDir() + L"\\" + novabar::bridge_identity::ReadyPrefix + std::to_wstring(GetCurrentProcessId()) + L".txt";
                FILE* readyFile = nullptr;
                if (_wfopen_s(&readyFile, readyPath.c_str(), L"w") == 0 && readyFile)
                {
                    fprintf(readyFile, "%lu\n", GetCurrentProcessId());
                    fclose(readyFile);
                }
                SignalEvent(kReadyEventName);
                return S_OK;
            }
            catch (...)
            {
                SignalEvent(kFailedEventName);
                Log(L"SetSite 失败: 0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override
        {
            if (!site)
            {
                return E_INVALIDARG;
            }

            *site = nullptr;
            return E_NOTIMPL;
        }

    private:
        winrt::com_ptr<Watcher> m_watcher;
    };

    struct SiteFactory : winrt::implements<SiteFactory, IClassFactory, winrt::non_agile>
    {
        HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override
        {
            if (outer)
            {
                return CLASS_E_NOAGGREGATION;
            }

            try
            {
                auto site = winrt::make_self<Site>();
                return site->QueryInterface(riid, object);
            }
            catch (...)
            {
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE LockServer(BOOL) override
        {
            return S_OK;
        }
    };

    void SignalEvent(const wchar_t* name)
    {
        // bInitialState 只在创建时生效；Engine 通常已建好同名事件，必须显式 SetEvent。
        const std::wstring eventName = std::wstring(name) + L"." + std::to_wstring(GetCurrentProcessId());
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (event)
        {
            SetEvent(event);
            CloseHandle(event);
        }
    }

}

extern "C" __declspec(dllexport) LRESULT CALLBACK NovaBarHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void** object)
{
    if (!object)
    {
        return E_INVALIDARG;
    }

    *object = nullptr;

    if (clsid != kSiteClsidValue)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    try
    {
        auto factory = winrt::make_self<SiteFactory>();
        return factory->QueryInterface(riid, object);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
