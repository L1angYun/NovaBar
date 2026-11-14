#pragma once
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <mutex>
#include <vector>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace novabar
{
    class XamlSourceLocator
    {
        struct Entry
        {
            DWORD Thread;
            winrt::weak_ref<winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource> Source;
        };
        std::mutex m_mutex;
        std::vector<Entry> m_sources;
    public:
        void Register(winrt::Windows::Foundation::IInspectable const& object)
        {
            if (auto source = object.try_as<winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource>())
            {
                std::lock_guard lock(m_mutex);
                m_sources.push_back({GetCurrentThreadId(), winrt::make_weak(source)});
            }
        }
        HWND Resolve(winrt::Windows::UI::Xaml::FrameworkElement const& element)
        {
            std::vector<Entry> sources;
            { std::lock_guard lock(m_mutex); sources = m_sources; }
            for (auto const& entry : sources)
            {
                if (entry.Thread != GetCurrentThreadId()) continue;
                try
                {
                    const auto source = entry.Source.get();
                    if (!source) continue;
                    const auto content = source.Content();
                    for (winrt::Windows::UI::Xaml::DependencyObject node = element; node;
                        node = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(node))
                    {
                        if (node != content) continue;
                        HWND window = nullptr;
                        if (auto native = source.try_as<IDesktopWindowXamlSourceNative>();
                            native && SUCCEEDED(native->get_WindowHandle(&window))) return window;
                    }
                }
                catch (...) {}
            }
            return nullptr;
        }
    };
}
