#pragma once

#include <windows.h>
#undef GetCurrentTime
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>

namespace novabar
{
    // Register on the window's UI thread. Events retain only weak state; a
    // diagnostics site can go away without keeping a shell window or watcher alive.
    class ShellVisibility
    {
        struct WindowEntry
        {
            winrt::weak_ref<winrt::Windows::UI::Xaml::Window> Window;
            winrt::Windows::UI::Core::CoreDispatcher Dispatcher{nullptr};
            winrt::event_token Token{};
            winrt::event_token ClosedToken{};
            bool Visible = false;
        };

        struct State : std::enable_shared_from_this<State>
        {
            explicit State(std::wstring const& statePath)
            {
                wchar_t executable[MAX_PATH * 4]{};
                const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
                if (!length || length >= ARRAYSIZE(executable)) return;
                const auto slash = wcsrchr(executable, L'\\');
                const wchar_t* name = slash ? slash + 1 : executable;
                const wchar_t* marker = nullptr;
                if (_wcsicmp(name, L"StartMenuExperienceHost.exe") == 0) marker = L"visibility.start.txt";
                else if (_wcsicmp(name, L"SearchHost.exe") == 0) marker = L"visibility.search.txt";
                if (!marker || statePath.empty()) return;
                const auto directory = statePath.find_last_of(L"\\/");
                if (directory == std::wstring::npos) return;
                Path = statePath.substr(0, directory + 1) + marker;
            }

            ~State()
            {
                // Revoke each handler on the dispatcher that owns its Window.
                // If that dispatcher has exited, the weak callback is harmless.
                for (auto const& [identity, entry] : Windows)
                {
                    try
                    {
                        if (entry.Dispatcher.HasThreadAccess()) Revoke(entry);
                        else entry.Dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                            [entry] { Revoke(entry); });
                    }
                    catch (winrt::hresult_error const&) {}
                }
                if (!Windows.empty()) Write(false);
            }

            static void Revoke(WindowEntry const& entry) noexcept
            {
                try
                {
                    if (auto window = entry.Window.get())
                    {
                        window.VisibilityChanged(entry.Token);
                        window.Closed(entry.ClosedToken);
                    }
                }
                catch (winrt::hresult_error const&) {}
            }

            bool Register(winrt::Windows::Foundation::IInspectable const& object, std::wstring const& type)
            {
                if (Path.empty() || type != L"Windows.UI.Xaml.Window") return false;
                auto window = object.try_as<winrt::Windows::UI::Xaml::Window>();
                if (!window) return false;
                void* identity = winrt::get_abi(window);
                std::lock_guard lock(Mutex);
                if (Windows.find(identity) != Windows.end()) return true;

                std::weak_ptr<State> weak = shared_from_this();
                const auto token = window.VisibilityChanged(
                    [weak, identity](auto const&, winrt::Windows::UI::Core::VisibilityChangedEventArgs const& args)
                    {
                        if (auto state = weak.lock()) state->Changed(identity, args.Visible());
                    });
                const auto closedToken = window.Closed([weak, identity](auto const&, auto const&)
                {
                    if (auto state = weak.lock()) state->Closed(identity);
                });
                Windows.emplace(identity, WindowEntry{winrt::make_weak(window), window.Dispatcher(), token, closedToken, window.Visible()});
                PublishLocked();
                return true;
            }

            void Changed(void* identity, bool visible)
            {
                std::lock_guard lock(Mutex);
                const auto found = Windows.find(identity);
                if (found == Windows.end()) return;
                found->second.Visible = visible;
                PublishLocked();
            }

            void Closed(void* identity)
            {
                WindowEntry removed;
                {
                    std::lock_guard lock(Mutex);
                    const auto found = Windows.find(identity);
                    if (found == Windows.end()) return;
                    removed = found->second;
                    Windows.erase(found);
                    PublishLocked();
                }
                Revoke(removed);
            }

            void PublishLocked()
            {
                bool visible = false;
                for (auto const& [identity, entry] : Windows)
                    visible = visible || entry.Visible;
                Write(visible);
            }

            void Write(bool visible) const noexcept
            {
                if (Path.empty()) return;
                try
                {
                    const auto temporary = Path + L".tmp." + std::to_wstring(GetCurrentProcessId())
                        + L"." + std::to_wstring(GetCurrentThreadId());
                    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file == INVALID_HANDLE_VALUE) return;
                    char buffer[64]{};
                    const int length = snprintf(buffer, sizeof(buffer), "%lu %u\n", GetCurrentProcessId(), visible ? 1u : 0u);
                    DWORD written = 0;
                    const bool complete = length > 0 && WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr)
                        && written == static_cast<DWORD>(length);
                    CloseHandle(file);
                    bool moved = false;
                    if (complete)
                    {
                        for (int attempt = 0; attempt < 3 && !moved; ++attempt)
                        {
                            moved = MoveFileExW(temporary.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
                            if (!moved && (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED))
                                Sleep(1);
                            else break;
                        }
                    }
                    if (!moved) DeleteFileW(temporary.c_str());
                }
                catch (...) {}
            }

            std::wstring Path;
            std::mutex Mutex;
            std::map<void*, WindowEntry> Windows;
        };

    public:
        explicit ShellVisibility(std::wstring const& statePath) : m_state(std::make_shared<State>(statePath)) {}
        ShellVisibility(ShellVisibility const&) = delete;
        ShellVisibility& operator=(ShellVisibility const&) = delete;
        ShellVisibility(ShellVisibility&&) = default;
        ShellVisibility& operator=(ShellVisibility&&) = default;

        bool Register(winrt::Windows::Foundation::IInspectable const& object, std::wstring const& type)
        {
            return m_state && m_state->Register(object, type);
        }

    private:
        std::shared_ptr<State> m_state;
    };
}
