#pragma once
#include <guiddef.h>

namespace novabar::bridge_identity
{
    // XAML diagnostics caches TAP factories for the session. A new implementation
    // gets a fresh identity while all earlier epochs retire on separate channels.
    inline constexpr wchar_t FileName[] = L"NovaBar.XamlBridge.v043.dll";
    inline constexpr wchar_t ClsidText[] = L"{C352328E-76EC-4AB1-AB9F-01E68F042AC8}";
    inline constexpr GUID SiteClsid =
        {0xc352328e, 0x76ec, 0x4ab1, {0xab, 0x9f, 0x01, 0xe6, 0x8f, 0x04, 0x2a, 0xc8}};
    inline constexpr wchar_t ReadyEvent[] = L"Local\\NovaBar.Bridge.Ready.v043";
    inline constexpr wchar_t FailedEvent[] = L"Local\\NovaBar.Bridge.Failed.v043";
    inline constexpr wchar_t ReadyPrefix[] = L"ready.v043.";
    inline constexpr wchar_t TaskbarState[] = L"state.v043.txt";
    inline constexpr wchar_t StartState[] = L"state.start.v043.txt";
    inline constexpr wchar_t SearchState[] = L"state.search.v043.txt";
    inline constexpr wchar_t NotificationState[] = L"state.notification.v043.txt";
    inline constexpr wchar_t PackageState[] = L"state.v043.txt";
    inline constexpr wchar_t StagePrefix[] = L"bridge-v043-";

    inline constexpr const wchar_t* LegacyGlobalStates[] = {
        L"state.txt", L"state.start.txt", L"state.search.txt", L"state.notification.txt",
        L"state.v04.txt", L"state.start.v04.txt", L"state.search.v04.txt", L"state.notification.v04.txt",
        L"state.v041.txt", L"state.start.v041.txt", L"state.search.v041.txt", L"state.notification.v041.txt",
        L"state.v042.txt", L"state.start.v042.txt", L"state.search.v042.txt", L"state.notification.v042.txt"};
    inline constexpr const wchar_t* LegacyPackageStates[] = {L"state.txt", L"state.v04.txt", L"state.v041.txt", L"state.v042.txt"};
}
