// NovaBar.Engine —— 注入宿主 + 状态机。
//
// 职责：
//   1. 把 XamlBridge.dll 注入 explorer，并在 explorer 重启后自动重挂
//   2. 读 apply.txt（UI 导出），实时判断当前该用哪个外观
//   3. 把结果写进 state.txt，桥接层轮询它并应用（这就是 IPC）
//
// 用法：
//   NovaBar.Engine.exe                 # 常驻，Ctrl+C 退出
//   NovaBar.Engine.exe --once          # 只算一次状态、不常驻（调试用）
//   NovaBar.Engine.exe --restore       # 打印恢复方法

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include "../NovaBar.Shared/BridgeIdentity.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace bridge_id = novabar::bridge_identity;
    constexpr auto& kReadyEventName = bridge_id::ReadyEvent;
    constexpr auto& kFailedEventName = bridge_id::FailedEvent;
    constexpr auto& kSiteClsidValue = bridge_id::ClsidText;

    std::atomic<bool> g_running{true};
    std::atomic<bool> g_injectionEnabled{false};
    std::atomic<bool> g_startSurface{false};
    std::atomic<bool> g_searchSurface{false};
    std::atomic<bool> g_notificationSurface{false};
    HANDLE g_shutdownEvent = nullptr;
    std::mutex g_logMutex;

    constexpr wchar_t kNormalSpec[] = L"accent=normal;color=#00000000;line=0;specular=0;tint=0";

    // 由 --data-dir 覆盖。以 SYSTEM 计划任务运行时，SYSTEM 自己的 %LOCALAPPDATA%
    // 和用户会话不是同一个目录，必须显式指向用户的配置目录，否则两边各看各的。
    std::wstring g_dataDirOverride;

    // ───────────────────────── 路径与配置 ─────────────────────────

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
            if (!g_dataDirOverride.empty())
            {
                CreateDirectoryW(g_dataDirOverride.c_str(), nullptr);
                return g_dataDirOverride;
            }

            const std::wstring path = LocalAppData() + L"\\NovaBar";
            CreateDirectoryW(path.c_str(), nullptr);
            return path;
        }();

        return dir;
    }

    std::wstring ApplyPath() { return DataDir() + L"\\apply.txt"; }
    std::wstring StatePath() { return DataDir() + L"\\" + bridge_id::TaskbarState; }
    std::wstring LogPath() { return DataDir() + L"\\engine.log"; }
    std::wstring RestorePath() { return DataDir() + L"\\restore.txt"; }

    void Log(const wchar_t* format, ...)
    {
        std::lock_guard lock(g_logMutex);
        wchar_t message[1024]{};
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, format, args);
        va_end(args);

        SYSTEMTIME now{};
        GetLocalTime(&now);

        FILE* file = nullptr;
        if (_wfopen_s(&file, LogPath().c_str(), L"a, ccs=UTF-8") == 0 && file)
        {
            fwprintf(file, L"[%02d:%02d:%02d] %ls\n", now.wHour, now.wMinute, now.wSecond, message);
            fclose(file);
        }
    }

    std::wstring Trim(const std::wstring& value)
    {
        const auto first = value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
        {
            return {};
        }

        const auto last = value.find_last_not_of(L" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    struct Rule
    {
        enum class Kind
        {
            Process,
            Title,
            Class,
        } Kind;

        std::wstring Pattern;
        std::wstring Spec;
        std::wstring InactiveSpec;
        std::wstring Scope;
    };

    struct Config
    {
        std::map<std::wstring, std::wstring> Appearances;   // state 名 -> 外观串
        std::map<std::wstring, std::wstring> InactiveAppearances;
        std::vector<Rule> Rules;
        std::vector<Rule> IgnoredWindows;
        bool Enabled = true;
        bool StartSurface = false;
        bool NotificationSurface = false;
        bool Loaded = false;
    };

    std::wstring DecodePattern(const std::wstring& encoded)
    {
        std::wstring decoded;
        for (size_t i = 0; i < encoded.size(); ++i)
        {
            if (encoded[i] == L'%' && i + 2 < encoded.size())
            {
                const std::wstring code = encoded.substr(i + 1, 2);
                wchar_t value = 0;
                if (code == L"25") value = L'%';
                else if (code == L"7C" || code == L"7c") value = L'|';
                else if (code == L"0D" || code == L"0d") value = L'\r';
                else if (code == L"0A" || code == L"0a") value = L'\n';
                if (value != 0)
                {
                    decoded += value;
                    i += 2;
                    continue;
                }
            }
            decoded += encoded[i];
        }
        return decoded;
    }

    void ParseApplyLine(Config& config, const std::wstring& input)
    {
        const std::wstring line = Trim(input);
        if (line.empty() || line[0] == L'#') return;
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) return;
        const std::wstring key = Trim(line.substr(0, eq));
        const std::wstring value = Trim(line.substr(eq + 1));
        if (key == L"enabled") { config.Enabled = value != L"0"; return; }
        if (key == L"surface.start") { config.StartSurface = value == L"1"; return; }
        if (key == L"surface.notification") { config.NotificationSurface = value == L"1"; return; }
        const bool scopedRule = key == L"rule.visible" || key == L"rule.maximized"
            || key == L"rule.visible.inactive" || key == L"rule.maximized.inactive";
        if (key == L"rule" || key == L"rule.inactive" || scopedRule || key == L"ignore")
        {
            const auto p1 = value.find(L'|');
            const auto p2 = p1 == std::wstring::npos ? std::wstring::npos : value.find(L'|', p1 + 1);
            if (p1 == std::wstring::npos || (key != L"ignore" && p2 == std::wstring::npos)) return;
            const std::wstring kind = value.substr(0, p1);
            if (kind != L"process" && kind != L"title" && kind != L"class") return;
            Rule rule;
            rule.Kind = kind == L"title" ? Rule::Kind::Title
                      : kind == L"class" ? Rule::Kind::Class : Rule::Kind::Process;
            if (scopedRule) rule.Scope = key.starts_with(L"rule.visible") ? L"visible" : L"maximized";
            rule.Pattern = DecodePattern(value.substr(p1 + 1,
                key == L"ignore" ? std::wstring::npos : p2 - p1 - 1));
            if (rule.Pattern.empty()) return;
            if (key == L"ignore") { config.IgnoredWindows.push_back(std::move(rule)); return; }
            const auto existing = std::find_if(config.Rules.begin(), config.Rules.end(), [&](const Rule& item)
            { return item.Kind == rule.Kind && item.Pattern == rule.Pattern && item.Scope == rule.Scope; });
            const size_t index = static_cast<size_t>(existing - config.Rules.begin());
            if (existing == config.Rules.end()) config.Rules.push_back(std::move(rule));
            Rule& target = config.Rules[index];
            if (key.ends_with(L".inactive")) target.InactiveSpec = value.substr(p2 + 1);
            else target.Spec = value.substr(p2 + 1);
            return;
        }
        const std::wstring suffix = L".inactive";
        if (key.size() > suffix.size() && key.ends_with(suffix))
        {
            config.InactiveAppearances[key.substr(0, key.size() - suffix.size())] = value;
        }
        else config.Appearances[key] = value;
    }

    /// UTF-8 文件通道：旧的状态 / rule 行保持兼容，忽略和失焦外观按需附加。
    Config LoadApply(const std::wstring& path = ApplyPath())
    {
        Config config;

        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || !file)
        {
            return config;
        }

        std::wstring line;
        wint_t character;
        while ((character = fgetwc(file)) != WEOF)
        {
            if (character == L'\n') { ParseApplyLine(config, line); line.clear(); }
            else line += static_cast<wchar_t>(character);
        }
        ParseApplyLine(config, line);

        fclose(file);
        config.Loaded = true;
        return config;
    }

    std::wstring TemporaryPath(const std::wstring& path)
    {
        static std::atomic<unsigned long long> sequence{0};
        return path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"."
            + std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(++sequence);
    }

    bool CommitTemporary(const std::wstring& temporary, const std::wstring& path)
    {
        DWORD error = ERROR_WRITE_FAULT;
        for (unsigned attempt = 0; attempt < 10; ++attempt)
        {
            if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) break;
            if (attempt != 9) Sleep(1);
        }
        DeleteFileW(temporary.c_str());
        SetLastError(error);
        return false;
    }

    bool WriteState(const std::wstring& spec, const std::wstring& path = StatePath())
    {
        const std::wstring temp = TemporaryPath(path);
        FILE* file = nullptr;
        if (_wfopen_s(&file, temp.c_str(), L"w, ccs=UTF-8") == 0 && file)
        {
            const bool written = fwprintf(file, L"%ls", spec.c_str()) >= 0;
            const bool closed = fclose(file) == 0;
            if (written && closed) return CommitTemporary(temp, path);
        }
        DeleteFileW(temp.c_str());
        return false;
    }

    bool ReadStateText(const std::wstring& path, std::wstring& text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || length.QuadPart > 65536)
        { CloseHandle(file); return false; }
        std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
        DWORD read = 0;
        const bool complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE
            && read == bytes.size();
        CloseHandle(file);
        if (!complete) return false;
        if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
        if (bytes.empty()) { text.clear(); return true; }
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (count <= 0) return false;
        text.resize(count);
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), count) == count;
    }

    bool PublishState(const std::wstring& spec, const std::wstring& path = StatePath())
    {
        // Another --restore/--once process may replace the file while our desired
        // appearance stays unchanged. Its actual contents are the authority.
        std::wstring actual;
        return (ReadStateText(path, actual) && actual == spec) || WriteState(spec, path);
    }

    bool RetireLegacyChannels()
    {
        bool published = true;
        for (const auto name : bridge_id::LegacyGlobalStates)
            published = PublishState(kNormalSpec, DataDir() + L"\\" + name) && published;
        return published;
    }

    unsigned long long FileStamp(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
        return (static_cast<unsigned long long>(info.ftLastWriteTime.dwHighDateTime) << 32)
             | info.ftLastWriteTime.dwLowDateTime;
    }

    bool RequestRestore()
    {
        return WriteState(std::to_wstring(FileStamp(ApplyPath())), RestorePath());
    }

    bool RestoreRequested(unsigned long long applyStamp)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, RestorePath().c_str(), L"r, ccs=UTF-8") != 0 || !file) return false;
        unsigned long long requestedStamp = 0;
        const bool valid = fwscanf_s(file, L"%llu", &requestedStamp) == 1;
        fclose(file);
        if (valid && requestedStamp == applyStamp) return true;
        DeleteFileW(RestorePath().c_str());
        return false;
    }

    struct NotificationPaths { std::wstring Directory; std::wstring Dll; std::wstring State; };

    std::wstring NotificationStageRoot()
    {
        return LocalAppData() + L"\\Packages\\Microsoft.Windows.ShellExperienceHost_cw5n1h2txyewy\\LocalState\\NovaBar";
    }

    std::wstring PackageStageRoot(DWORD pid)
    {
        if (!pid) return {};
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return {};
        UINT32 length = 0;
        std::wstring family;
        if (GetPackageFamilyName(process, &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 1)
        {
            family.resize(length);
            if (GetPackageFamilyName(process, &length, family.data()) == ERROR_SUCCESS)
                family.resize(length - 1);
            else family.clear();
        }
        CloseHandle(process);
        return family.empty() ? std::wstring() : LocalAppData() + L"\\Packages\\" + family + L"\\LocalState\\NovaBar";
    }

    NotificationPaths GetPackagePaths(const std::wstring& exeDir, const std::wstring& root)
    {
        if (root.empty()) return {};
        const auto stamp = FileStamp(exeDir + L"\\" + bridge_id::FileName);
        if (stamp == 0) return {};
        const std::wstring directory = root + L"\\" + bridge_id::StagePrefix + std::to_wstring(stamp);
        return {directory, directory + L"\\" + bridge_id::FileName, root + L"\\" + bridge_id::PackageState};
    }

    NotificationPaths GetNotificationPaths(const std::wstring& exeDir)
    {
        return GetPackagePaths(exeDir, NotificationStageRoot());
    }

    bool StageNotificationBridge(const std::wstring& exeDir, const NotificationPaths& paths)
    {
        if (paths.Directory.empty()) return false;
        std::error_code error;
        // New files inherit the package's existing capabilities; no ACLs are modified.
        std::filesystem::create_directories(paths.Directory, error);
        if (error) return false;
        if (std::filesystem::exists(paths.Dll, error) && !error) return true;
        const std::filesystem::path temporary = TemporaryPath(paths.Dll);
        std::ifstream input(std::filesystem::path(exeDir + L"\\" + bridge_id::FileName), std::ios::binary);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!input || !output) return false;
        output << input.rdbuf();
        output.flush();
        const bool written = input.good() && output.good();
        output.close();
        if (written) return CommitTemporary(temporary.wstring(), paths.Dll);
        DeleteFileW(temporary.c_str());
        return false;
    }

    bool PublishPackageStates(const std::wstring& root, const std::wstring& spec)
    {
        if (root.empty()) return true;
        std::error_code error;
        if (!std::filesystem::exists(root, error)) return !error;
        const auto publishDirectory = [&](const std::filesystem::path& directory)
        {
            bool published = true;
            // Resident v0.3 and v0.4 hooks stay retired while v0.4.1 owns Liquid.
            for (const auto name : bridge_id::LegacyPackageStates)
                published = PublishState(kNormalSpec, (directory / name).wstring()) && published;
            return PublishState(spec, (directory / bridge_id::PackageState).wstring()) && published;
        };
        bool published = publishDirectory(root);
        std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator it(root, error); !error && it != end; it.increment(error))
        {
            const auto& entry = *it;
            if (entry.is_directory(error) && entry.path().filename().wstring().starts_with(L"bridge-"))
            {
                published = publishDirectory(entry.path()) && published;
            }
        }
        return published && !error;
    }

    void RestoreStagedNotifications()
    {
        PublishPackageStates(NotificationStageRoot(), kNormalSpec);
    }

    // ───────────────────────── 系统状态 ─────────────────────────

    bool ProcessHasVisibleWindow(DWORD pid)
    {
        struct Context
        {
            DWORD Pid;
            bool Found;
        } context{pid, false};

        EnumWindows([](HWND window, LPARAM param) -> BOOL
        {
            auto* ctx = reinterpret_cast<Context*>(param);

            DWORD windowPid = 0;
            GetWindowThreadProcessId(window, &windowPid);
            if (windowPid != ctx->Pid)
            {
                return TRUE;
            }

            if (!IsWindowVisible(window))
            {
                return TRUE;
            }

            // 只要有一个真正可见的窗口就算打开
            if (GetWindowTextLengthW(window) > 0)
            {
                ctx->Found = true;
                return FALSE;
            }

            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));

        return context.Found;
    }

    struct ProcessRecord { std::wstring Name; DWORD Id = 0; };
    bool ProcessMatchesName(DWORD pid, std::wstring const& name)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process) return false;
        wchar_t path[32768]{};
        DWORD length = ARRAYSIZE(path);
        const bool live = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        const bool queried = live && QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        if (!queried) return false;
        const wchar_t* leaf = wcsrchr(path, L'\\');
        return CompareStringOrdinal(leaf ? leaf + 1 : path, -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    std::vector<ProcessRecord> EnumerateProcesses()
    {
        std::vector<ProcessRecord> records;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return records;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) do
            records.push_back({entry.szExeFile, entry.th32ProcessID});
        while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return records;
    }

    class ProcessInventory
    {
        struct NameLess
        {
            bool operator()(std::wstring const& left, std::wstring const& right) const
            { return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN; }
        };
        std::function<std::vector<ProcessRecord>()> m_enumerate;
        std::function<bool(DWORD, std::wstring const&)> m_validate;
        std::map<std::wstring, std::vector<DWORD>, NameLess> m_processes;
        std::mutex m_mutex;
        ULONGLONG m_refreshed = 0;
        bool m_loaded = false;
    public:
        ProcessInventory(std::function<std::vector<ProcessRecord>()> enumerate,
                         std::function<bool(DWORD, std::wstring const&)> validate)
            : m_enumerate(std::move(enumerate)), m_validate(std::move(validate)) {}
        DWORD Find(std::wstring const& name, ULONGLONG now)
        {
            if (name.empty()) return 0;
            std::lock_guard lock(m_mutex);
            if (!m_loaded || now - m_refreshed >= 1000)
            {
                std::map<std::wstring, std::vector<DWORD>, NameLess> next;
                for (auto const& process : m_enumerate()) next[process.Name].push_back(process.Id);
                m_processes = std::move(next); m_refreshed = now; m_loaded = true;
            }
            const auto found = m_processes.find(name);
            if (found == m_processes.end()) return 0;
            // Validate every cached candidate cheaply. A dead host or a PID
            // reused by another executable must never become an injection target.
            for (const DWORD pid : found->second) if (m_validate(pid, name)) return pid;
            return 0;
        }
    };

    template<typename ForegroundOwner, typename Validate, typename CachedLookup>
    DWORD FindProcessWithForegroundHint(std::wstring const& name, ForegroundOwner&& foregroundOwner,
                                       Validate&& validate, CachedLookup&& cachedLookup)
    {
        if (name.empty()) return 0;
        // These hosts can be created lazily by the first shell-open gesture.
        // Their real foreground owner is already observable while the shared
        // process snapshot can still hold a one-second negative lookup.
        for (const auto host : {L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"ShellHost.exe", L"ShellExperienceHost.exe"})
        {
            if (CompareStringOrdinal(name.c_str(), -1, host, -1, TRUE) != CSTR_EQUAL) continue;
            const DWORD pid = foregroundOwner();
            if (pid && validate(pid, name)) return pid;
            break;
        }
        return cachedLookup(name);
    }

    DWORD FindProcessByName(const wchar_t* name)
    {
        // State resolution and injection share one process snapshot per second.
        static ProcessInventory inventory(EnumerateProcesses, ProcessMatchesName);
        return name ? FindProcessWithForegroundHint(name, []
        {
            DWORD pid = 0;
            const HWND window = GetForegroundWindow();
            return window && GetWindowThreadProcessId(window, &pid) ? pid : 0;
        }, ProcessMatchesName, [&](std::wstring const& candidate)
        { return inventory.Find(candidate, GetTickCount64()); }) : 0;
    }

    DWORD FindProcessByName(const wchar_t* name);

    bool IsShellWindow(HWND window)
    {
        if (!window || window == GetShellWindow())
        {
            return true;
        }

        wchar_t className[256]{};
        GetClassNameW(window, className, ARRAYSIZE(className));

        // 已知的 shell 界面类：任务栏、桌面、开始菜单、搜索、任务视图、托盘浮窗…
        static const wchar_t* kShellClasses[] =
        {
            L"Progman",
            L"WorkerW",
            L"Shell_TrayWnd",
            L"Shell_SecondaryTrayWnd",
            L"MultitaskingViewFrame",
            L"TopLevelWindowForOverflowXamlIsland",
            L"XamlExplorerHostIslandWindow",
            L"ForegroundStaging",
            L"TaskListThumbnailWnd",
            L"tooltips_class32",
            L"#32768",
            L"Shell_InputSwitchTopLevelWindow",
            L"Xaml_WindowedPopupClass",
        };

        for (const wchar_t* candidate : kShellClasses)
        {
            if (wcscmp(className, candidate) == 0)
            {
                return true;
            }
        }

        // 属于 explorer 但不是文件管理器窗口的，一律按 shell UI 处理。
        // 这样微软以后新增浮窗类名也不会误判成「有窗口」。
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);

        static DWORD shellPid = 0;
        static ULONGLONG lastCheck = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastCheck > 5000)
        {
            shellPid = FindProcessByName(L"explorer.exe");
            lastCheck = now;
        }

        return pid == shellPid && pid != 0
            && _wcsicmp(className, L"CabinetWClass") != 0
            && _wcsicmp(className, L"ExploreWClass") != 0;
    }

    struct Snapshot
    {
        std::wstring State;
        std::wstring Process;
        std::wstring Title;
        std::wstring Class;
        bool HasWindow = false;
        bool IsMaximized = false;
        bool WindowIsForeground = true;
        bool BatterySaver = false;
        bool TaskViewOpen = false;
        bool SearchOpen = false;
        bool StartOpen = false;
    };

    struct WindowSnapshot
    {
        std::wstring Process;
        std::wstring Title;
        std::wstring Class;
        bool Maximized = false;
        bool Foreground = false;
    };

    /// 不区分大小写的子串匹配。Windows 的进程名大小写不固定（Notepad.exe），
    /// 规则里写 notepad.exe 必须也能命中。
    bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle)
    {
        // CompareStringOrdinal accepts signed 32-bit UTF-16 lengths. Compare
        // fixed-size windows so matching stays independent of the C locale.
        if (needle.empty() || needle.size() > haystack.size() || needle.size() > 0x7fffffffU)
        {
            return false;
        }

        const int count = static_cast<int>(needle.size());
        const size_t last = haystack.size() - needle.size();
        for (size_t offset = 0; offset <= last; ++offset)
        {
            if (CompareStringOrdinal(haystack.data() + offset, count, needle.data(), count, TRUE) == CSTR_EQUAL)
                return true;
        }
        return false;
    }

    bool MatchesWindow(const Rule& rule, const WindowSnapshot& window)
    {
        const std::wstring& value = rule.Kind == Rule::Kind::Process ? window.Process
                                  : rule.Kind == Rule::Kind::Title ? window.Title : window.Class;
        return ContainsNoCase(value, rule.Pattern);
    }

    bool IsIgnored(const Config& config, const WindowSnapshot& window)
    {
        return std::any_of(config.IgnoredWindows.begin(), config.IgnoredWindows.end(),
            [&](const Rule& rule) { return MatchesWindow(rule, window); });
    }

    // EnumWindows 提供 Z 序。先最大化，再前台，最后保留最靠上的窗口。
    Snapshot SelectWindow(const Config& config, const std::vector<WindowSnapshot>& windows)
    {
        Snapshot snapshot;
        snapshot.State = L"desktop";
        int best = -1;
        for (const auto& window : windows)
        {
            if (IsIgnored(config, window)) continue;
            const int score = (window.Maximized ? 2 : 0) + (window.Foreground ? 1 : 0);
            if (score <= best) continue;
            best = score;
            snapshot.HasWindow = true;
            snapshot.IsMaximized = window.Maximized;
            snapshot.WindowIsForeground = window.Foreground;
            snapshot.Process = window.Process;
            snapshot.Title = window.Title;
            snapshot.Class = window.Class;
            snapshot.State = window.Maximized ? L"maximized" : L"visible";
        }
        return snapshot;
    }

    bool IsCloaked(HWND window)
    {
        using GetAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
        static HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        static auto get = module ? reinterpret_cast<GetAttribute>(GetProcAddress(module, "DwmGetWindowAttribute")) : nullptr;
        DWORD cloaked = 0;
        return get && SUCCEEDED(get(window, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked != 0;
    }

    bool ReadVisibility(const std::wstring& path, DWORD livePid, bool& visible)
    {
        if (!livePid) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        char buffer[128]{};
        DWORD bytes = 0;
        const bool read = ReadFile(file, buffer, sizeof(buffer) - 1, &bytes, nullptr) != FALSE;
        CloseHandle(file);
        if (!read) return false;
        const unsigned char* raw = reinterpret_cast<const unsigned char*>(buffer);
        const char* text = bytes >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? buffer + 3 : buffer;
        DWORD pid = 0;
        unsigned value = 2;
        const bool valid = sscanf_s(text, "%lu %u", &pid, &value) == 2 && pid == livePid && value <= 1;
        if (valid) visible = value == 1;
        return valid;
    }

    void DisambiguateShellVisibility(Snapshot& snapshot, bool startKnown)
    {
        // SearchHost also exposes a visible XAML window while Start is open.
        // The Start host's own visibility distinguishes that shared shell UI.
        if (startKnown && snapshot.StartOpen) snapshot.SearchOpen = false;
    }

    std::wstring WindowProcessName(HWND window)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return {};
        wchar_t image[32768]{};
        DWORD size = ARRAYSIZE(image);
        const bool queried = QueryFullProcessImageNameW(process, 0, image, &size) != FALSE;
        CloseHandle(process);
        if (!queried) return {};
        const std::wstring path(image, size);
        const auto slash = path.find_last_of(L'\\');
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    Snapshot TakeSnapshot(const Config& config)
    {
        struct Context { const Config* Configuration; HWND Foreground; std::vector<WindowSnapshot> Windows; }
            context{&config, GetForegroundWindow(), {}};
        EnumWindows([](HWND window, LPARAM param) -> BOOL
        {
            auto& ctx = *reinterpret_cast<Context*>(param);
            if (!IsWindowVisible(window) || IsIconic(window) || IsCloaked(window) || IsShellWindow(window)) return TRUE;
            const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            if ((style & WS_EX_TOOLWINDOW) != 0 || (style & WS_EX_NOACTIVATE) != 0) return TRUE;
            RECT rect{};
            if (!GetWindowRect(window, &rect) || IsRectEmpty(&rect)) return TRUE;
            WindowSnapshot candidate;
            candidate.Process = WindowProcessName(window);
            if (_wcsicmp(candidate.Process.c_str(), L"StartMenuExperienceHost.exe") == 0
                || _wcsicmp(candidate.Process.c_str(), L"SearchHost.exe") == 0
                || _wcsicmp(candidate.Process.c_str(), L"ShellHost.exe") == 0) return TRUE;
            std::vector<wchar_t> title(static_cast<size_t>(GetWindowTextLengthW(window)) + 1);
            GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
            wchar_t className[256]{};
            GetClassNameW(window, className, ARRAYSIZE(className));
            candidate.Title = title.data();
            candidate.Class = className;
            candidate.Maximized = IsZoomed(window) != FALSE;
            candidate.Foreground = window == ctx.Foreground;
            if (!IsIgnored(*ctx.Configuration, candidate)) ctx.Windows.push_back(std::move(candidate));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));

        Snapshot snapshot = SelectWindow(config, context.Windows);
        const DWORD startPid = FindProcessByName(L"StartMenuExperienceHost.exe");
        const DWORD searchPid = FindProcessByName(L"SearchHost.exe");
        snapshot.StartOpen = startPid && ProcessHasVisibleWindow(startPid);
        snapshot.SearchOpen = searchPid && ProcessHasVisibleWindow(searchPid);
        const bool startKnown = ReadVisibility(DataDir() + L"\\visibility.start.txt", startPid, snapshot.StartOpen);
        const auto searchStage = PackageStageRoot(searchPid);
        if (searchStage.empty() || !ReadVisibility(searchStage + L"\\visibility.search.txt", searchPid, snapshot.SearchOpen))
            ReadVisibility(DataDir() + L"\\visibility.search.txt", searchPid, snapshot.SearchOpen);
        DisambiguateShellVisibility(snapshot, startKnown);
        const HWND taskView = FindWindowW(L"MultitaskingViewFrame", nullptr);
        snapshot.TaskViewOpen = taskView && IsWindowVisible(taskView);
        SYSTEM_POWER_STATUS power{};
        snapshot.BatterySaver = GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1;
        return snapshot;
    }

    struct Resolution { std::wstring State; std::wstring Spec; };

    Resolution Resolve(const Config& config, const Snapshot& snapshot)
    {
        if (!config.Enabled) return {L"disabled", kNormalSpec};
        const std::pair<const wchar_t*, bool> special[] = {
            {L"battery", snapshot.BatterySaver}, {L"taskview", snapshot.TaskViewOpen},
            {L"search", snapshot.SearchOpen}, {L"start", snapshot.StartOpen},
        };
        for (const auto& [state, active] : special)
        {
            if (!active && snapshot.State != state) continue;
            if (const auto it = config.Appearances.find(state); it != config.Appearances.end() && !it->second.empty())
                return {state, it->second};
        }

        const bool hasWindow = snapshot.HasWindow || snapshot.State == L"visible" || snapshot.State == L"maximized";
        const bool maximized = snapshot.IsMaximized || snapshot.State == L"maximized";
        if (hasWindow)
        {
            const WindowSnapshot window{snapshot.Process, snapshot.Title, snapshot.Class, maximized, snapshot.WindowIsForeground};
            if (!IsIgnored(config, window))
            {
                const std::wstring state = maximized ? L"maximized" : L"visible";
                for (const auto& scope : {state, std::wstring()})
                {
                    for (const auto kind : {Rule::Kind::Process, Rule::Kind::Title, Rule::Kind::Class})
                    {
                        for (const auto& rule : config.Rules)
                        {
                            if (rule.Scope != scope || rule.Kind != kind || rule.Spec.empty() || !MatchesWindow(rule, window)) continue;
                            return {state, !snapshot.WindowIsForeground && !rule.InactiveSpec.empty() ? rule.InactiveSpec : rule.Spec};
                        }
                    }
                }
                for (const auto candidate : {L"maximized", L"visible"})
                {
                    if (wcscmp(candidate, L"maximized") == 0 && !maximized) continue;
                    const auto it = config.Appearances.find(candidate);
                    if (it == config.Appearances.end() || it->second.empty()) continue;
                    const auto inactive = config.InactiveAppearances.find(candidate);
                    return {candidate, !snapshot.WindowIsForeground && inactive != config.InactiveAppearances.end()
                        ? inactive->second : it->second};
                }
            }
        }
        const auto fallback = config.Appearances.find(L"desktop");
        return {L"desktop", fallback != config.Appearances.end() ? fallback->second : kNormalSpec};
    }

    std::wstring PickSpec(const Config& config, const Snapshot& snapshot)
    {
        return Resolve(config, snapshot).Spec;
    }

    void RefreshApply(Config& config, unsigned long long& loadedStamp, const std::wstring& path)
    {
        const auto stamp = FileStamp(path);
        if (config.Loaded && stamp == loadedStamp) return;
        const Config next = LoadApply(path);
        if (next.Loaded) { config = next; loadedStamp = stamp; }
    }

    class ResidentInstance
    {
        HANDLE m_mutex = nullptr;
        bool m_owned = false;
    public:
        enum class Result { Owner, Duplicate, Error };
        ResidentInstance() = default;
        ResidentInstance(const ResidentInstance&) = delete;
        ResidentInstance& operator=(const ResidentInstance&) = delete;
        ~ResidentInstance()
        {
            if (m_owned) ReleaseMutex(m_mutex);
            if (m_mutex) CloseHandle(m_mutex);
        }
        Result Acquire(const std::wstring& directory)
        {
            std::wstring canonical = directory;
            HANDLE folder = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (folder != INVALID_HANDLE_VALUE)
            {
                const DWORD length = GetFinalPathNameByHandleW(folder, nullptr, 0, FILE_NAME_NORMALIZED);
                if (length)
                {
                    std::wstring resolved(length, L'\0');
                    const DWORD written = GetFinalPathNameByHandleW(folder, resolved.data(), length, FILE_NAME_NORMALIZED);
                    if (written && written < length) { resolved.resize(written); canonical = std::move(resolved); }
                }
                CloseHandle(folder);
            }
            else
            {
                const DWORD length = GetFullPathNameW(directory.c_str(), 0, nullptr, nullptr);
                std::wstring resolved(length, L'\0');
                if (length)
                {
                    const DWORD written = GetFullPathNameW(directory.c_str(), length, resolved.data(), nullptr);
                    if (written && written < length) { resolved.resize(written); canonical = std::move(resolved); }
                }
            }
            while (canonical.size() > 3 && (canonical.back() == L'\\' || canonical.back() == L'/')) canonical.pop_back();
            unsigned long long hash = 14695981039346656037ULL;
            for (const auto character : canonical)
            { hash ^= static_cast<unsigned long long>(towlower(character)); hash *= 1099511628211ULL; }
            // Local already isolates Windows sessions; the canonical directory
            // distinguishes user installations, aliases and independent tests.
            const std::wstring name = L"Local\\NovaBar.Engine." + std::to_wstring(hash);
            m_mutex = CreateMutexW(nullptr, FALSE, name.c_str());
            if (!m_mutex) return Result::Error;
            const DWORD wait = WaitForSingleObject(m_mutex, 0);
            m_owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
            return m_owned ? Result::Owner : wait == WAIT_TIMEOUT ? Result::Duplicate : Result::Error;
        }
    };

    // ───────────────────────── 注入 ─────────────────────────

    class InjectionRetryPolicy
    {
        struct Attempt { ULONGLONG Next = 0; unsigned Failures = 0; };
        std::map<DWORD, Attempt> m_attempts;
    public:
        bool CanAttempt(DWORD pid, ULONGLONG now) const
        {
            const auto found = m_attempts.find(pid);
            return found == m_attempts.end() || now >= found->second.Next;
        }
        void Attempted(DWORD pid, ULONGLONG now) { m_attempts[pid].Next = now + 15000ULL; }
        bool Failed(DWORD pid, ULONGLONG now, bool primary, HRESULT result = E_FAIL)
        {
            // A shell host can exist long before it creates its first XAML
            // diagnostics endpoint. Keep polling until the user opens it.
            if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            {
                m_attempts[pid].Failures = 0;
                return false;
            }
            if (primary) return false;
            auto& attempt = m_attempts[pid];
            if (++attempt.Failures < 3) return false;
            attempt.Failures = 0;
            attempt.Next = now + 3600000ULL;
            return true;
        }
        void Succeeded(DWORD pid) { m_attempts.erase(pid); }
    };

    template<typename Initialize, typename Continue>
    HRESULT ScanDiagnosticsConnections(Initialize&& initialize, Continue&& shouldContinue)
    {
        HRESULT result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        // XAML Core instances increment the endpoint identifier for the process.
        for (unsigned connection = 1; connection <= 10000; ++connection)
        {
            if (!shouldContinue()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            const std::wstring name = L"VisualDiagConnection" + std::to_wstring(connection);
            result = initialize(name.c_str());
            if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
        }
        return result;
    }

    template<typename Initialize, typename Continue>
    HRESULT RunDiagnosticsAttempt(Initialize&& initialize, Continue&& shouldContinue)
    {
        HRESULT result = E_FAIL;
        // Diagnostics retains thread-local initialization state. One target
        // attempt scans all endpoints on a fresh thread, then joins before retry.
        std::thread attempt([&]
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(com)) { result = com; return; }
            result = ScanDiagnosticsConnections(initialize, shouldContinue);
            CoUninitialize();
        });
        attempt.join();
        return result;
    }

    HANDLE CreateSharedEvent(const wchar_t* name)
    {
        SECURITY_DESCRIPTOR descriptor{};
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorDacl(&descriptor, TRUE, nullptr, FALSE))
        {
            return nullptr;
        }

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;

        return CreateEventW(&attributes, TRUE, FALSE, name);
    }

    bool IsBridgeLoaded(DWORD pid, const wchar_t* dllName)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        bool found = false;
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, dllName) == 0)
                {
                    found = true;
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return found;
    }

    std::wstring ReadyMarkerPath(const std::wstring& statePath, DWORD pid)
    {
        const auto separator = statePath.find_last_of(L"\\/");
        return (separator == std::wstring::npos ? std::wstring() : statePath.substr(0, separator + 1))
            + bridge_id::ReadyPrefix + std::to_wstring(pid) + L".txt";
    }

    DWORD WaitForBridgeReady(const std::wstring& marker, DWORD pid, HANDLE ready, HANDLE failed,
                             const std::atomic<bool>& enabled, DWORD timeout)
    {
        const ULONGLONG deadline = GetTickCount64() + timeout;
        while (g_running && enabled)
        {
            std::wstring content;
            if ((ReadStateText(marker, content) && Trim(content) == std::to_wstring(pid))
                || (ready && WaitForSingleObject(ready, 0) == WAIT_OBJECT_0)) return WAIT_OBJECT_0;
            if (failed && WaitForSingleObject(failed, 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + 1;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return WAIT_TIMEOUT;
            const DWORD delay = static_cast<DWORD>(std::min<ULONGLONG>(50, deadline - now));
            if (g_shutdownEvent)
            {
                if (WaitForSingleObject(g_shutdownEvent, delay) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + 2;
            }
            else Sleep(delay);
        }
        return WAIT_OBJECT_0 + 2;
    }

    /// 由独立工作线程调用：诊断入口直接把 TAP DLL 载入目标，无需窗口钩子。
    bool InjectIntoProcess(const std::wstring& dllPath, DWORD pid, const wchar_t* label,
                           const std::wstring& statePath, const std::atomic<bool>& enabled)
    {
        static InjectionRetryPolicy retry;
        static const wchar_t* kDllName = bridge_id::FileName;
        if (!pid || !enabled) return false;
        if (IsBridgeLoaded(pid, kDllName)) { retry.Succeeded(pid); return true; }
        const ULONGLONG now = GetTickCount64();
        if (!retry.CanAttempt(pid, now)) return false;
        retry.Attempted(pid, now);
        const bool primary = wcscmp(label, L"任务栏") == 0;

        using InitializeFn = HRESULT(WINAPI*)(PCWSTR, DWORD, PCWSTR, PCWSTR, CLSID, PCWSTR);
        static HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        static auto initialize = xaml ? reinterpret_cast<InitializeFn>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
        CLSID site{};
        HRESULT result = CLSIDFromString(kSiteClsidValue, &site);
        if (!initialize || GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            result = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

        const std::wstring readyName = std::wstring(kReadyEventName) + L"." + std::to_wstring(pid);
        const std::wstring failedName = std::wstring(kFailedEventName) + L"." + std::to_wstring(pid);
        HANDLE ready = CreateSharedEvent(readyName.c_str());
        HANDLE failed = CreateSharedEvent(failedName.c_str());
        if (ready) ResetEvent(ready);
        if (failed) ResetEvent(failed);
        const std::wstring marker = ReadyMarkerPath(statePath, pid);
        if (!DeleteFileW(marker.c_str()))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) result = HRESULT_FROM_WIN32(error);
        }

        if (SUCCEEDED(result))
        {
            result = RunDiagnosticsAttempt([&](const wchar_t* name)
            { return initialize(name, pid, nullptr, dllPath.c_str(), site, statePath.c_str()); },
                [&] { return g_running.load() && enabled.load(); });
        }
        if (SUCCEEDED(result))
        {
            const DWORD wait = WaitForBridgeReady(marker, pid, ready, failed, enabled, 2000);
            if (wait == WAIT_OBJECT_0)
            {
                retry.Succeeded(pid);
                Log(L"%ls 桥接层挂载成功（pid %lu）", label, pid);
                if (ready) CloseHandle(ready);
                if (failed) CloseHandle(failed);
                return true;
            }
            result = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : E_FAIL;
        }
        if (ready) CloseHandle(ready);
        if (failed) CloseHandle(failed);
        if (!g_running || !enabled) return false;
        Log(L"%ls 桥接层挂载失败（pid %lu，0x%08X）", label, pid, static_cast<unsigned>(result));
        if (retry.Failed(pid, GetTickCount64(), primary, result))
            Log(L"%ls 暂停注入一小时（pid %lu，连续失败 3 次）", label, pid);
        return false;
    }

    void EnsureInjected(const std::wstring& exeDir)
    {
        if (!g_injectionEnabled && !g_startSurface && !g_searchSurface && !g_notificationSurface) return;
        const std::wstring dll = exeDir + L"\\" + bridge_id::FileName;
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (taskbar && g_injectionEnabled)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(taskbar, &pid);
            InjectIntoProcess(dll, pid, L"任务栏", StatePath(), g_injectionEnabled);
        }
        if (g_running && g_startSurface)
            InjectIntoProcess(dll, FindProcessByName(L"StartMenuExperienceHost.exe"), L"开始菜单",
                DataDir() + L"\\" + bridge_id::StartState, g_startSurface);
        if (g_running && g_searchSurface)
        {
            const DWORD searchPid = FindProcessByName(L"SearchHost.exe");
            const auto searchPaths = GetPackagePaths(exeDir, PackageStageRoot(searchPid));
            if (StageNotificationBridge(exeDir, searchPaths))
                InjectIntoProcess(searchPaths.Dll, searchPid, L"搜索状态", searchPaths.State, g_searchSurface);
        }
        if (g_running && g_notificationSurface)
        {
            InjectIntoProcess(dll, FindProcessByName(L"ShellHost.exe"), L"快捷设置",
                DataDir() + L"\\" + bridge_id::NotificationState, g_notificationSurface);
            const auto staged = GetNotificationPaths(exeDir);
            if (g_running && g_notificationSurface && StageNotificationBridge(exeDir, staged))
                InjectIntoProcess(staged.Dll, FindProcessByName(L"ShellExperienceHost.exe"), L"通知中心",
                    staged.State, g_notificationSurface);
        }
    }

    // The broker is an ordinary full-trust child beside this engine. Its unnamed
    // job is private to this resident instance and guarantees cleanup on exit.
    class CaptureBroker
    {
        HANDLE m_process = nullptr, m_job = nullptr;
        unsigned long long m_started = 0, m_nextStart = 0;
        DWORD m_backoff = 1000;
        static std::wstring Quote(const std::wstring& value)
        {
            std::wstring result = L"\"";
            size_t slashes = 0;
            for (wchar_t character : value)
            {
                if (character == L'\\') { ++slashes; continue; }
                if (character == L'\"') { result.append(slashes * 2 + 1, L'\\'); result += character; }
                else { result.append(slashes, L'\\'); result += character; }
                slashes = 0;
            }
            result.append(slashes * 2, L'\\'); result += L'\"';
            return result;
        }
    public:
        ~CaptureBroker() { Stop(); }
        void Stop()
        {
            if (m_job) { CloseHandle(m_job); m_job = nullptr; }
            if (m_process)
            {
                if (WaitForSingleObject(m_process, 500) == WAIT_TIMEOUT)
                    TerminateProcess(m_process, 0); // only the child created by this instance
                CloseHandle(m_process); m_process = nullptr;
            }
        }
        void Tick(const std::wstring& exeDir)
        {
            const unsigned long long now = GetTickCount64();
            if (m_process)
            {
                if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) return;
                DWORD code = 0;
                GetExitCodeProcess(m_process, &code);
                CloseHandle(m_process); m_process = nullptr;
                if (m_job) { CloseHandle(m_job); m_job = nullptr; }
                if (now - m_started > 30000) m_backoff = 1000;
                m_nextStart = now + m_backoff;
                Log(L"捕获进程退出（%lu），%lu ms 后重试", code, m_backoff);
                m_backoff = std::min<DWORD>(m_backoff * 2, 30000);
            }
            if (now < m_nextStart) return;
            const std::wstring executable = exeDir + L"\\NovaBar.Capture.exe";
            if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
            { m_nextStart = now + 30000; return; }
            std::wstring command = Quote(executable) + L" --parent " + std::to_wstring(GetCurrentProcessId())
                + L" --watch-dir " + Quote(DataDir()) + L" --watch-dir " + Quote(NotificationStageRoot());
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION child{};
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                { CloseHandle(job); job = nullptr; }
            }
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, exeDir.c_str(), &startup, &child))
            {
                if (job) CloseHandle(job);
                m_nextStart = now + m_backoff;
                m_backoff = std::min<DWORD>(m_backoff * 2, 30000);
                return;
            }
            if (job && !AssignProcessToJobObject(job, child.hProcess)) { CloseHandle(job); job = nullptr; }
            m_job = job; m_process = child.hProcess; m_started = now;
            ResumeThread(child.hThread); CloseHandle(child.hThread);
            Log(L"捕获进程已启动（pid %lu）", child.dwProcessId);
        }
    };

    BOOL WINAPI OnConsoleCtrl(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
        {
            g_running = false;
            if (g_shutdownEvent) SetEvent(g_shutdownEvent);
            return TRUE;
        }

        return FALSE;
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    bool once = false;
    bool restoreRequested = false;
    bool noInject = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];

        if (arg == L"--once")
        {
            once = true;
        }
        else if (arg == L"--no-inject")
        {
            noInject = true;
        }
        else if (arg == L"--data-dir" && i + 1 < argc)
        {
            g_dataDirOverride = argv[++i];
        }
        else if (arg == L"--restore")
        {
            // 延后执行：--data-dir 可能写在 --restore 后面
            restoreRequested = true;
        }
        else
        {
            wprintf(L"unknown argument: %ls\n", arg.c_str());
            return 2;
        }
    }

    if (restoreRequested)
    {
        if (!RequestRestore() || !RetireLegacyChannels() || !WriteState(kNormalSpec)
            || !WriteState(kNormalSpec, DataDir() + L"\\" + bridge_id::StartState)
            || !WriteState(kNormalSpec, DataDir() + L"\\" + bridge_id::NotificationState)
            || !WriteState(kNormalSpec, DataDir() + L"\\" + bridge_id::SearchState))
        {
            fwprintf(stderr, L"[engine] failed to persist restore request\n");
            return 1;
        }
        if (!noInject) RestoreStagedNotifications();
        wprintf(L"[engine] restored default appearance\n");
        return 0;
    }

    ResidentInstance instance;
    if (!once)
    {
        const auto ownership = instance.Acquire(DataDir());
        if (ownership == ResidentInstance::Result::Duplicate)
        {
            wprintf(L"[engine] already running for this data directory\n");
            return 0;
        }
        if (ownership == ResidentInstance::Result::Error)
        {
            fwprintf(stderr, L"[engine] failed to acquire resident instance lock\n");
            return 1;
        }
    }

    wchar_t modulePath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    std::wstring exeDir(modulePath);
    const auto slash = exeDir.find_last_of(L'\\');
    exeDir = slash == std::wstring::npos ? L"." : exeDir.substr(0, slash);

    wprintf(L"NovaBar Engine\n");
    wprintf(L"  apply : %ls\n", ApplyPath().c_str());
    wprintf(L"  state : %ls\n", StatePath().c_str());

    Config config;
    Config startConfig;
    Config notificationConfig;
    std::wstring lastSpec;
    std::wstring lastState;
    unsigned long long loadedStamp = 0;
    unsigned long long startStamp = 0;
    unsigned long long notificationStamp = 0;
    int exitCode = 0;

    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_shutdownEvent) return 1;
    if (!RetireLegacyChannels())
    {
        CloseHandle(g_shutdownEvent); g_shutdownEvent = nullptr;
        fwprintf(stderr, L"[engine] failed to retire legacy state channels\n");
        return 1;
    }
    std::thread injectionWorker;
    // --once 是状态机诊断，不把短命进程变成另一个注入器。
    if (!noInject && !once)
    {
        RestoreStagedNotifications();
        const auto oldSearchRoot = PackageStageRoot(FindProcessByName(L"SearchHost.exe"));
        if (!oldSearchRoot.empty()) PublishPackageStates(oldSearchRoot, kNormalSpec);
        // Give the resident legacy UI dispatcher a chance to detach its
        // property guards before the new diagnostics identity takes ownership.
        WaitForSingleObject(g_shutdownEvent, 300);
        injectionWorker = std::thread([exeDir]
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            while (g_running)
            {
                EnsureInjected(exeDir);
                if (WaitForSingleObject(g_shutdownEvent, 200) == WAIT_OBJECT_0) break;
            }
            if (SUCCEEDED(com)) CoUninitialize();
        });
    }

    int tick = 0;
    CaptureBroker captureBroker;

    while (g_running)
    {
        const unsigned long long applyStamp = FileStamp(ApplyPath());
        RefreshApply(config, loadedStamp, ApplyPath());
        RefreshApply(startConfig, startStamp, DataDir() + L"\\apply.start.txt");
        RefreshApply(notificationConfig, notificationStamp, DataDir() + L"\\apply.notification.txt");

        const bool restored = RestoreRequested(applyStamp);
        const bool captureEnabled = !restored && ((config.Loaded && config.Enabled)
            || (config.StartSurface && startConfig.Loaded && startConfig.Enabled)
            || (config.NotificationSurface && notificationConfig.Loaded && notificationConfig.Enabled));
        if (!once && !noInject && captureEnabled) captureBroker.Tick(exeDir);
        else captureBroker.Stop();
        if (!config.Loaded && !restored)
        {
            if (once)
            {
                wprintf(L"[engine] no apply.txt yet - waiting\n");
                exitCode = 3;
                break;
            }
        }

        const Snapshot snapshot = TakeSnapshot(config);
        const Resolution resolution = restored ? Resolution{L"restored", kNormalSpec} : Resolve(config, snapshot);
        const std::wstring& spec = resolution.Spec;

        if (tick % 25 == 0)
        {
            Log(L"心跳: state=%ls proc=%ls class=%ls title=%ls",
                resolution.State.c_str(), snapshot.Process.c_str(),
                snapshot.Class.c_str(), snapshot.Title.c_str());
        }

        if (!PublishState(spec))
        {
            Log(L"写入 state.txt 失败，下一拍重试");
            if (once) { exitCode = 1; break; }
            Sleep(200);
            continue;
        }
        if (spec != lastSpec)
        {
            lastSpec = spec;

            if (resolution.State != lastState || once)
            {
                wprintf(L"[engine] state=%ls -> %ls\n", resolution.State.c_str(), spec.c_str());
                Log(L"状态 %ls -> %ls", resolution.State.c_str(), spec.c_str());
                lastState = resolution.State;
            }

        }

        const bool startEnabled = config.StartSurface && startConfig.Loaded && startConfig.Enabled && !restored;
        const bool notificationEnabled = config.NotificationSurface && notificationConfig.Loaded && notificationConfig.Enabled && !restored;
        const std::wstring startSpec = startEnabled ? PickSpec(startConfig, TakeSnapshot(startConfig)) : kNormalSpec;
        const std::wstring notificationSpec = notificationEnabled ? PickSpec(notificationConfig, TakeSnapshot(notificationConfig)) : kNormalSpec;
        bool published = RetireLegacyChannels()
            && PublishState(startSpec, DataDir() + L"\\" + bridge_id::StartState)
            && PublishState(notificationSpec, DataDir() + L"\\" + bridge_id::NotificationState)
            && PublishState(kNormalSpec, DataDir() + L"\\" + bridge_id::SearchState);
        if (!noInject)
        {
            const auto notificationPaths = GetNotificationPaths(exeDir);
            if (notificationEnabled) StageNotificationBridge(exeDir, notificationPaths);
            published = PublishPackageStates(NotificationStageRoot(), notificationSpec) && published;
            if (config.Loaded && config.Enabled && !restored && config.Appearances.contains(L"search"))
            {
                const auto searchRoot = PackageStageRoot(FindProcessByName(L"SearchHost.exe"));
                const auto searchPaths = GetPackagePaths(exeDir, searchRoot);
                if (!searchPaths.Directory.empty() && StageNotificationBridge(exeDir, searchPaths))
                    published = PublishPackageStates(searchRoot, kNormalSpec) && published;
            }
        }
        // Publish the initial state before exposing opt-ins to the injector thread.
        g_injectionEnabled = config.Loaded && config.Enabled && !restored;
        g_startSurface = startEnabled || (g_injectionEnabled && config.Appearances.contains(L"start"));
        g_searchSurface = g_injectionEnabled && config.Appearances.contains(L"search");
        g_notificationSurface = notificationEnabled;
        if (!published)
        {
            Log(L"写入 Shell 界面外观失败，下一拍重试");
            if (once) { exitCode = 1; break; }
        }

        ++tick;

        if (once)
        {
            break;
        }

        Sleep(200);
    }

    g_running = false;
    captureBroker.Stop();
    SetEvent(g_shutdownEvent);
    if (injectionWorker.joinable()) injectionWorker.join();
    SetConsoleCtrlHandler(OnConsoleCtrl, FALSE);
    CloseHandle(g_shutdownEvent);
    g_shutdownEvent = nullptr;
    if (once) return exitCode;

    wprintf(L"[engine] stopped\n");

    // 正常退出时把外观交还系统，避免留下一个半透明任务栏没人管
    RetireLegacyChannels();
    WriteState(kNormalSpec);
    WriteState(kNormalSpec, DataDir() + L"\\" + bridge_id::StartState);
    WriteState(kNormalSpec, DataDir() + L"\\" + bridge_id::NotificationState);
    WriteState(kNormalSpec, DataDir() + L"\\" + bridge_id::SearchState);
    if (!noInject) RestoreStagedNotifications();
    Log(L"引擎退出，已恢复默认外观");
    return 0;
}
