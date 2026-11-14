#pragma once

#include <windows.h>
#undef GetCurrentTime
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include "LiquidSurfaceClient.h"
#include "XamlSourceLocator.h"

namespace novabar
{
    enum class ShellAccent { Normal, Clear, Color, Acrylic, LiquidGlass };

    struct ShellSurfaceStyle
    {
        ShellAccent Accent = ShellAccent::Normal;
        uint32_t Color = 0;
        double TintStrength = 0.3;
        double Blur = 9;
        double Lens = 0.55;
        double Specular = 0.35;
        double Dispersion = 0.18;
        double Noise = 0.12;
        bool ShowLine = false;
    };

    inline bool UsesSystemShellBorder(ShellSurfaceStyle const& style)
    {
        // Liquid renders its own rounded edge and specular light. Restoring
        // the shell's outline on top produces a second bright corner arc.
        // Normal still relinquishes every local override to the system.
        return style.Accent == ShellAccent::Normal
            || (style.Accent != ShellAccent::LiquidGlass && style.ShowLine);
    }

    namespace surface_detail
    {
        inline void* CanonicalIdentity(winrt::Windows::Foundation::IInspectable const& object)
        { return object.as<::IUnknown>().get(); }

        struct ApplyingGuard
        {
            bool& Flag;
            const bool Previous;
            explicit ApplyingGuard(bool& flag) : Flag(flag), Previous(flag) { Flag = true; }
            ~ApplyingGuard() { Flag = Previous; }
        };

        // Different diagnostics sites can enumerate the same live XAML object.
        // Keep a weak process-wide claim, independent of a particular Watcher.
        template<class Entry> class WeakOwners
        {
            using Key = std::pair<DWORD, uintptr_t>;
            std::mutex m_mutex;
            std::map<Key, std::weak_ptr<Entry>> m_entries;
        public:
            template<class Alive> bool Claim(DWORD thread, void* identity,
                                             std::shared_ptr<Entry> const& candidate, Alive alive)
            {
                std::lock_guard lock(m_mutex);
                std::erase_if(m_entries, [&](auto const& item)
                { const auto owner = item.second.lock(); return !owner || !alive(owner, item.first.first); });
                const Key key{thread, reinterpret_cast<uintptr_t>(identity)};
                const auto found = m_entries.find(key);
                if (found != m_entries.end())
                    if (auto owner = found->second.lock()) return owner == candidate;
                m_entries[key] = candidate;
                return true;
            }
        };

        inline bool IsControlCenterTintOverlay(std::wstring const& name, std::wstring const& parentName,
                                               bool parentIsGrid, bool controlCenterAncestor)
        { return name.empty() && parentName == L"L1Grid" && parentIsGrid && controlCenterAncestor; }

        inline bool IsControlCenterBackdrop(std::wstring const& name, bool controlCenterAncestor)
        { return name == L"ControlCenterRegion" && controlCenterAncestor; }

        enum class BackgroundAction { Restore, Clear, Material, Liquid };
        inline BackgroundAction SelectBackground(ShellAccent accent, bool overlay, bool liquidOnlyOverlay,
                                                 bool liquidOnlyMaterial, bool suppressLiquidBackground)
        {
            if (accent == ShellAccent::Normal
                || ((liquidOnlyOverlay || liquidOnlyMaterial) && accent != ShellAccent::LiquidGlass))
                return BackgroundAction::Restore;
            if (accent == ShellAccent::LiquidGlass)
                return overlay || suppressLiquidBackground ? BackgroundAction::Clear : BackgroundAction::Liquid;
            return overlay ? BackgroundAction::Clear : BackgroundAction::Material;
        }
    }

    // Register runs on the element's XAML UI thread. Apply can run on another
    // dispatcher: notification/calendar windows can belong to separate threads.
    // Targets are weakly held so old flyout visual trees can be collected.
    // Shell Acrylic currently uses the system material's fixed blur radius.
    class ShellSurfaceStyler
    {
    public:
        ShellSurfaceStyler() = default;
        ShellSurfaceStyler(ShellSurfaceStyler const&) = delete;
        ShellSurfaceStyler& operator=(ShellSurfaceStyler const&) = delete;
        void SetCaptureDirectory(std::wstring directory)
        {
            std::lock_guard lock(m_context->Mutex);
            m_context->Directory = std::move(directory);
        }
        void RegisterXamlSource(winrt::Windows::Foundation::IInspectable const& source) { m_context->Sources.Register(source); }
        ~ShellSurfaceStyler()
        {
            m_context->Alive = false;
            ++m_context->Revision;
            std::vector<std::shared_ptr<Entry>> entries;
            { std::lock_guard lock(m_context->Mutex); entries.swap(m_entries); }
            for (auto const& entry : entries)
            {
                try
                {
                    if (entry->Dispatcher.HasThreadAccess()) Revoke(entry);
                    else entry->Dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [entry] { Revoke(entry); });
                }
                catch (...) {}
            }
        }

        bool Register(winrt::Windows::UI::Xaml::DependencyObject const& object,
                      std::wstring const& type, std::wstring const& name)
        {
            namespace xaml = winrt::Windows::UI::Xaml;
            namespace controls = xaml::Controls;
            bool overlay = false;
            bool liquidOnlyOverlay = false;
            bool liquidOnlyMaterial = false;
            bool suppressLiquidBackground = false;
            xaml::DependencyProperty background{nullptr};
            xaml::DependencyProperty borderBrush{nullptr};
            if (m_host == Host::Start)
            {
                if (type != L"Windows.UI.Xaml.Controls.Border" || !object.try_as<controls::Border>()) return false;
                if (name != L"AcrylicBorder" && name != L"AcrylicOverlay") return false;
                if (!HasAncestor(object, {L"StartMenu.StartBlendedFlexFrame", L"StartMenu.StartSizingFrame",
                                           L"StartDocked.StartSizingFrame"})) return false;
                overlay = name == L"AcrylicOverlay";
                background = controls::Border::BackgroundProperty();
                borderBrush = controls::Border::BorderBrushProperty();
            }
            else if (m_host == Host::Shell && type == L"Windows.UI.Xaml.Controls.Border")
            {
                if (!object.try_as<controls::Border>()) return false;
                if (!HasAncestor(object, {L"ControlCenter.ControlCenterPage", L"ControlCenter.ControlCenterView"}))
                    return false;
                if (name != L"RootGridBorder")
                {
                    const auto parent = xaml::Media::VisualTreeHelper::GetParent(object);
                    const auto grid = parent ? parent.try_as<controls::Grid>() : nullptr;
                    if (!surface_detail::IsControlCenterTintOverlay(name,
                        grid ? std::wstring(grid.Name()) : std::wstring(), bool(grid), true)) return false;
                    overlay = liquidOnlyOverlay = true;
                }
                else suppressLiquidBackground = true;
                background = controls::Border::BackgroundProperty();
                borderBrush = controls::Border::BorderBrushProperty();
            }
            else if (m_host == Host::Shell && type == L"Windows.UI.Xaml.Controls.Grid")
            {
                if (!object.try_as<controls::Grid>()) return false;
                if (surface_detail::IsControlCenterBackdrop(name,
                    HasAncestor(object, {L"ControlCenter.ControlCenterPage"})))
                {
                    // The actual Acrylic owner is stable across navigation;
                    // RootGridBorder belongs to the sliding inner content.
                    // Put the one liquid client on this outer panel instead.
                    liquidOnlyMaterial = true;
                }
                else
                {
                    if (name != L"NotificationCenterGrid" && name != L"CalendarCenterGrid") return false;
                    if (!HasAncestor(object, {L"ActionCenter.NotificationCenterPage"})) return false;
                }
                background = controls::Panel::BackgroundProperty();
                borderBrush = controls::Grid::BorderBrushProperty();
            }
            else
            {
                return false;
            }

            std::shared_ptr<Entry> added;
            bool hasStyle = false;
            uint64_t revision = 0;
            {
                std::lock_guard lock(m_context->Mutex);
                PruneOnCurrentThread();
                const auto identity = surface_detail::CanonicalIdentity(object);
                for (auto const& entry : m_entries)
                    if (entry->Identity == identity) return true;
                added = std::make_shared<Entry>();
                added->Element = winrt::make_weak(object);
                added->Dispatcher = object.Dispatcher();
                added->Identity = identity;
                added->Owner = m_context;
                static surface_detail::WeakOwners<Entry> owners;
                if (!owners.Claim(GetCurrentThreadId(), identity, added, [](auto const& entry, DWORD thread)
                    {
                        const auto owner = entry->Owner.lock();
                        if (!owner || !owner->Alive) return false;
                        if (thread != GetCurrentThreadId()) return true;
                        try { return bool(entry->Element.get()); } catch (...) { return false; }
                    })) return true;
                added->BackgroundProperty = background;
                added->BorderBrushProperty = borderBrush;
                // Capture once, before any custom brush is installed.
                added->BackgroundLocal = object.ReadLocalValue(background);
                added->BorderBrushLocal = object.ReadLocalValue(borderBrush);
                added->Overlay = overlay;
                added->LiquidOnlyOverlay = liquidOnlyOverlay;
                added->LiquidOnlyMaterial = liquidOnlyMaterial;
                added->SuppressLiquidBackground = suppressLiquidBackground;
                m_entries.push_back(added);
                hasStyle = m_context->HasStyle;
                revision = m_context->Revision.load();
            }
            InstallLoaded(added, m_context, object);
            if (hasStyle) ApplyOwned(added, m_context, revision, true);
            return true;
        }

        void Apply(ShellSurfaceStyle const& style)
        {
            std::vector<std::shared_ptr<Entry>> entries;
            uint64_t revision = 0;
            {
                std::lock_guard lock(m_context->Mutex);
                m_context->Style = style;
                m_context->HasStyle = true;
                revision = ++m_context->Revision;
                PruneOnCurrentThread();
                entries = m_entries;
            }
            for (auto const& entry : entries)
            {
                try
                {
                    if (entry->Dispatcher.HasThreadAccess()) ApplyOwned(entry, m_context, revision, true);
                    else entry->Dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [weak = std::weak_ptr<Entry>(entry), context = std::weak_ptr<Context>(m_context), revision]
                        {
                            try { if (auto current = context.lock()) if (auto target = weak.lock()) ApplyOwned(target, current, revision, true); }
                            catch (...) {}
                        });
                }
                catch (...) {}
            }
        }

        bool HasTargets() const
        {
            std::lock_guard lock(m_context->Mutex);
            return !m_entries.empty();
        }

    private:
        enum class Host { Other, Start, Shell };
        struct Context;
        struct Entry
        {
            winrt::weak_ref<winrt::Windows::UI::Xaml::DependencyObject> Element;
            winrt::Windows::UI::Core::CoreDispatcher Dispatcher{nullptr};
            void* Identity = nullptr;
            winrt::Windows::UI::Xaml::DependencyProperty BackgroundProperty{nullptr};
            winrt::Windows::UI::Xaml::DependencyProperty BorderBrushProperty{nullptr};
            winrt::Windows::Foundation::IInspectable BackgroundLocal{nullptr};
            winrt::Windows::Foundation::IInspectable BorderBrushLocal{nullptr};
            bool Overlay = false;
            bool LiquidOnlyOverlay = false;
            bool LiquidOnlyMaterial = false;
            bool SuppressLiquidBackground = false;
            std::weak_ptr<Context> Owner;
            winrt::Windows::Foundation::IInspectable ExpectedBackground{nullptr};
            winrt::Windows::Foundation::IInspectable ExpectedBorder{nullptr};
            int64_t BackgroundToken = 0, BorderToken = 0;
            winrt::event_token LoadedToken{};
            bool ObservingBackground = false, ObservingBorder = false, ObservingLoaded = false;
            // These flags are accessed exclusively on the element's dispatcher.
            bool Applying = false, Queued = false, Force = false;
            uint64_t PendingRevision = 0;
            std::shared_ptr<LiquidSurfaceClient> Liquid;
        };

        struct Context
        {
            std::mutex Mutex;
            std::atomic<uint64_t> Revision{0};
            std::atomic<bool> Alive{true};
            ShellSurfaceStyle Style;
            bool HasStyle = false;
            std::wstring Directory;
            XamlSourceLocator Sources;
        };

        static Host CurrentHost()
        {
            wchar_t path[MAX_PATH * 4]{};
            const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
            if (!length || length >= ARRAYSIZE(path)) return Host::Other;
            const auto slash = wcsrchr(path, L'\\');
            const wchar_t* name = slash ? slash + 1 : path;
            if (_wcsicmp(name, L"StartMenuExperienceHost.exe") == 0) return Host::Start;
            if (_wcsicmp(name, L"ShellHost.exe") == 0 || _wcsicmp(name, L"ShellExperienceHost.exe") == 0)
                return Host::Shell;
            return Host::Other;
        }

        static bool HasAncestor(winrt::Windows::UI::Xaml::DependencyObject object,
                                std::initializer_list<wchar_t const*> roots)
        {
            for (unsigned depth = 0; object && depth < 64; ++depth)
            {
                const auto name = winrt::get_class_name(object);
                for (const auto root : roots) if (name == root) return true;
                object = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(object);
            }
            return false;
        }

        static winrt::Windows::UI::Color ToColor(uint32_t argb)
        {
            return {static_cast<uint8_t>(argb >> 24), static_cast<uint8_t>(argb >> 16),
                    static_cast<uint8_t>(argb >> 8), static_cast<uint8_t>(argb)};
        }

        static winrt::Windows::UI::Xaml::Media::Brush MakeBrush(ShellSurfaceStyle const& style)
        {
            namespace media = winrt::Windows::UI::Xaml::Media;
            if (style.Accent == ShellAccent::Clear)
                return media::SolidColorBrush(ToColor(0));
            if (style.Accent == ShellAccent::Color)
                return media::SolidColorBrush(ToColor(style.Color | 0xFF000000));
            if (style.Accent == ShellAccent::LiquidGlass)
                return media::SolidColorBrush(ToColor(0));

            // Start/Control Center are separate XAML surfaces: their backdrop is
            // the actual desktop compositor, never a copied wallpaper strip.
            media::AcrylicBrush brush;
            brush.BackgroundSource(media::AcrylicBackgroundSource::HostBackdrop);
            auto tint = ToColor(style.Color);
            const double alpha = tint.A / 255.0;
            tint.A = 255;
            brush.TintColor(tint);
            brush.TintOpacity(std::clamp(alpha * (style.Accent == ShellAccent::LiquidGlass
                                                   ? style.TintStrength : 1.0), 0.0, 1.0));
            brush.TintLuminosityOpacity(winrt::box_value(0.0).as<winrt::Windows::Foundation::IReference<double>>());
            brush.FallbackColor(tint);
            return brush;
        }

        static void Restore(winrt::Windows::UI::Xaml::DependencyObject const& element,
                            winrt::Windows::UI::Xaml::DependencyProperty const& property,
                            winrt::Windows::Foundation::IInspectable const& original)
        {
            if (original == winrt::Windows::UI::Xaml::DependencyProperty::UnsetValue())
                element.ClearValue(property);
            else
                element.SetValue(property, original);
        }

        static bool CurrentStyle(std::shared_ptr<Context> const& context, ShellSurfaceStyle& style, uint64_t& revision)
        {
            if (!context->Alive) return false;
            std::lock_guard lock(context->Mutex);
            if (!context->Alive || !context->HasStyle) return false;
            style = context->Style; revision = context->Revision.load();
            return true;
        }

        static void DetachProperties(std::shared_ptr<Entry> const& entry,
                                     winrt::Windows::UI::Xaml::DependencyObject const& element) noexcept
        {
            try { if (entry->ObservingBackground) element.UnregisterPropertyChangedCallback(entry->BackgroundProperty, entry->BackgroundToken); } catch (...) {}
            try { if (entry->ObservingBorder) element.UnregisterPropertyChangedCallback(entry->BorderBrushProperty, entry->BorderToken); } catch (...) {}
            entry->ObservingBackground = entry->ObservingBorder = false;
        }

        static void Revoke(std::shared_ptr<Entry> const& entry) noexcept
        {
            if (entry->Liquid) { entry->Liquid->Stop(); entry->Liquid.reset(); }
            try
            {
                if (auto element = entry->Element.get())
                {
                    DetachProperties(entry, element);
                    if (entry->ObservingLoaded)
                        if (auto framework = element.try_as<winrt::Windows::UI::Xaml::FrameworkElement>()) framework.Loaded(entry->LoadedToken);
                }
            }
            catch (...) {}
            entry->ObservingLoaded = false;
            entry->Queued = false;
        }

        static bool MatchesEffective(std::shared_ptr<Entry> const& entry,
                                     winrt::Windows::UI::Xaml::DependencyObject const& element,
                                     ShellSurfaceStyle const& style)
        {
            return element.GetValue(entry->BackgroundProperty) == entry->ExpectedBackground
                && (UsesSystemShellBorder(style) || element.GetValue(entry->BorderBrushProperty) == entry->ExpectedBorder);
        }

        static void QueueReapply(std::shared_ptr<Entry> const& entry, std::shared_ptr<Context> const& context, bool force)
        {
            ShellSurfaceStyle style; uint64_t revision = 0;
            if (!CurrentStyle(context, style, revision) || style.Accent == ShellAccent::Normal) return;
            entry->PendingRevision = revision;
            entry->Force = entry->Force || force;
            if (entry->Queued) return;
            entry->Queued = true;
            try
            {
                entry->Dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                    [weak = std::weak_ptr<Entry>(entry), state = std::weak_ptr<Context>(context)]
                    {
                        auto target = weak.lock();
                        if (!target) return;
                        target->Queued = false;
                        const bool force = target->Force; target->Force = false;
                        try
                        {
                            auto current = state.lock();
                            ShellSurfaceStyle style; uint64_t revision = 0;
                            if (!current || !CurrentStyle(current, style, revision)
                                || revision != target->PendingRevision || style.Accent == ShellAccent::Normal) return;
                            auto element = target->Element.get();
                            if (element && (force || !MatchesEffective(target, element, style))) ApplyOwned(target, current, revision, false);
                        }
                        catch (...) {}
                    });
            }
            catch (...) { entry->Queued = false; entry->Force = false; }
        }

        static void InstallLoaded(std::shared_ptr<Entry> const& entry, std::shared_ptr<Context> const& context,
                                  winrt::Windows::UI::Xaml::DependencyObject const& element)
        {
            if (entry->ObservingLoaded) return;
            if (auto framework = element.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
            {
                entry->LoadedToken = framework.Loaded(
                    [weak = std::weak_ptr<Entry>(entry), state = std::weak_ptr<Context>(context)](auto const&, auto const&)
                    {
                        try
                        {
                            if (auto current = state.lock()) if (auto target = weak.lock())
                            {
                                ShellSurfaceStyle style; uint64_t revision = 0;
                                if (!CurrentStyle(current, style, revision)) return;
                                if (target->Dispatcher.HasThreadAccess()) ApplyOwned(target, current, revision, false);
                                else QueueReapply(target, current, true);
                            }
                        }
                        catch (...) {}
                    });
                entry->ObservingLoaded = true;
            }
        }

        static void AttachProperties(std::shared_ptr<Entry> const& entry, std::shared_ptr<Context> const& context,
                                     winrt::Windows::UI::Xaml::DependencyObject const& element)
        {
            if (entry->ObservingBackground && entry->ObservingBorder) return;
            const auto changed = [weak = std::weak_ptr<Entry>(entry), state = std::weak_ptr<Context>(context)](auto const& sender, auto const&)
            {
                try
                {
                    auto target = weak.lock(); auto current = state.lock();
                    if (!target || !current || target->Applying) return;
                    ShellSurfaceStyle style; uint64_t revision = 0;
                    if (CurrentStyle(current, style, revision) && style.Accent != ShellAccent::Normal
                        && !MatchesEffective(target, sender, style))
                    {
                        // A low-priority repair allows the template's opaque
                        // brush to be presented during the opening animation.
                        // Repair only owned values, with a nesting-safe guard.
                        if (style.Accent == ShellAccent::LiquidGlass && target->ExpectedBackground
                            && target->Dispatcher.HasThreadAccess())
                        {
                            surface_detail::ApplyingGuard applying(target->Applying);
                            if (sender.GetValue(target->BackgroundProperty) != target->ExpectedBackground)
                                sender.SetValue(target->BackgroundProperty, target->ExpectedBackground);
                            if (target->ExpectedBorder && sender.GetValue(target->BorderBrushProperty) != target->ExpectedBorder)
                                sender.SetValue(target->BorderBrushProperty, target->ExpectedBorder);
                            if (MatchesEffective(target, sender, style)) return;
                        }
                        QueueReapply(target, current, false);
                    }
                }
                catch (...) {}
            };
            try
            {
                if (!entry->ObservingBackground)
                {
                    entry->BackgroundToken = element.RegisterPropertyChangedCallback(entry->BackgroundProperty, changed);
                    entry->ObservingBackground = true;
                }
                if (!entry->ObservingBorder)
                {
                    entry->BorderToken = element.RegisterPropertyChangedCallback(entry->BorderBrushProperty, changed);
                    entry->ObservingBorder = true;
                }
            }
            catch (...) { DetachProperties(entry, element); throw; }
        }

        static void ApplyOwned(std::shared_ptr<Entry> const& entry, std::shared_ptr<Context> const& context,
                               uint64_t expectedRevision, bool defer)
        {
            namespace media = winrt::Windows::UI::Xaml::Media;
            ShellSurfaceStyle style; uint64_t revision = 0;
            if (!CurrentStyle(context, style, revision) || revision != expectedRevision) return;
            auto element = entry->Element.get();
            if (!element) return;
            surface_detail::ApplyingGuard applying(entry->Applying);
            const auto action = surface_detail::SelectBackground(style.Accent, entry->Overlay,
                entry->LiquidOnlyOverlay, entry->LiquidOnlyMaterial, entry->SuppressLiquidBackground);
            if (action == surface_detail::BackgroundAction::Restore)
            {
                if (entry->Liquid) { entry->Liquid->Stop(); entry->Liquid.reset(); }
                // Normal relinquishes the guard as well as the original locals.
                DetachProperties(entry, element);
                entry->ExpectedBackground = entry->ExpectedBorder = nullptr;
                Restore(element, entry->BackgroundProperty, entry->BackgroundLocal);
                Restore(element, entry->BorderBrushProperty, entry->BorderBrushLocal);
                return;
            }
            AttachProperties(entry, context, element);
            media::Brush brush{nullptr};
            if (action == surface_detail::BackgroundAction::Liquid)
            {
                if (!entry->Liquid)
                {
                    const auto framework = element.try_as<winrt::Windows::UI::Xaml::FrameworkElement>();
                    if (framework)
                    {
                        entry->Liquid = std::make_shared<LiquidSurfaceClient>(framework, context->Directory,
                            capture::Kind::Panel, nullptr,
                            [weak = std::weak_ptr<Entry>(entry), state = std::weak_ptr<Context>(context)](auto const& image, bool)
                            {
                                auto target = weak.lock(); auto current = state.lock();
                                ShellSurfaceStyle active; uint64_t revision = 0;
                                if (!target || !current || !CurrentStyle(current, active, revision)
                                    || active.Accent != ShellAccent::LiquidGlass) return;
                                if (auto object = target->Element.get())
                                {
                                    surface_detail::ApplyingGuard guard(target->Applying);
                                    target->ExpectedBackground = image;
                                    if (object.GetValue(target->BackgroundProperty) != image) object.SetValue(target->BackgroundProperty, image);
                                }
                            }, [state = std::weak_ptr<Context>(context)](auto const& framework) -> HWND
                            {
                                if (auto current = state.lock()) return current->Sources.Resolve(framework);
                                return nullptr;
                            });
                    }
                }
                if (entry->Liquid)
                {
                    LiquidAppearance appearance;
                    appearance.Material = {style.Lens, style.Dispersion, style.Noise, style.TintStrength, style.Blur, style.Color};
                    appearance.Specular = style.Specular;
                    entry->Liquid->SetAppearance(appearance);
                    brush = entry->Liquid->CurrentBrush();
                }
                else brush = media::SolidColorBrush(ToColor(0));
            }
            else
            {
                if (entry->Liquid) { entry->Liquid->Stop(); entry->Liquid.reset(); }
                brush = action == surface_detail::BackgroundAction::Clear ? media::SolidColorBrush(ToColor(0)) : MakeBrush(style);
            }
            entry->ExpectedBackground = brush;
            element.SetValue(entry->BackgroundProperty, brush);
            if (UsesSystemShellBorder(style))
            {
                entry->ExpectedBorder = nullptr;
                Restore(element, entry->BorderBrushProperty, entry->BorderBrushLocal);
            }
            else
            {
                const media::Brush line = media::SolidColorBrush(ToColor(0));
                entry->ExpectedBorder = line;
                element.SetValue(entry->BorderBrushProperty, line);
            }
            // Let templates/visual states finish before the first enforcement.
            // Later repairs are event-driven and do not create another timer/job.
            if (defer) QueueReapply(entry, context, true);
        }

        void PruneOnCurrentThread()
        {
            std::erase_if(m_entries, [](std::shared_ptr<Entry> const& entry)
            {
                try { return entry->Dispatcher.HasThreadAccess() && !entry->Element.get(); }
                catch (...) { return true; }
            });
        }

        Host m_host = CurrentHost();
        std::shared_ptr<Context> m_context = std::make_shared<Context>();
        std::vector<std::shared_ptr<Entry>> m_entries;
    };
}
