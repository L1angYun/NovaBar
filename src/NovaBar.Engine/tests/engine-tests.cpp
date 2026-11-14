// Regression tests exercise the engine's real parser and resolver without injection.
#define wmain NovaBarEngineMain
#include "../main.cpp"
#undef wmain
#include <filesystem>
#include <appmodel.h>
#include <set>
#include "../../NovaBar.Shared/BridgeIdentity.h"

namespace {
    int failures = 0;
    void Check(bool condition, const char* name)
    {
        if (!condition) { fprintf(stderr, "FAIL: %s\n", name); ++failures; }
        else { printf("PASS: %s\n", name); }
    }
    void SaveApply(const wchar_t* text)
    {
        FILE* file = nullptr;
        _wfopen_s(&file, ApplyPath().c_str(), L"w, ccs=UTF-8");
        if (!file) { throw std::runtime_error("cannot write fixture"); }
        fputws(text, file);
        fclose(file);
    }
    std::wstring ReadStateForTest(const std::wstring& path)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || !file) return {};
        std::wstring content;
        wint_t value;
        while ((value = fgetwc(file)) != WEOF) content += static_cast<wchar_t>(value);
        fclose(file);
        return content;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) { return 2; }
    g_dataDirOverride = argv[1];
    const std::wstring migrationLiquid = L"accent=liquid;color=#FF123456;line=0;specular=1;tint=0.5";
    const wchar_t* legacyGlobalStates[] = {
        L"state.txt", L"state.start.txt", L"state.search.txt", L"state.notification.txt",
        L"state.v04.txt", L"state.start.v04.txt", L"state.search.v04.txt", L"state.notification.v04.txt",
        L"state.v041.txt", L"state.start.v041.txt", L"state.search.v041.txt", L"state.notification.v041.txt",
        L"state.v042.txt", L"state.start.v042.txt", L"state.search.v042.txt", L"state.notification.v042.txt"};
    for (const auto file : legacyGlobalStates) WriteState(migrationLiquid, DataDir() + L"\\" + file);
    WriteState(migrationLiquid, DataDir() + L"\\state.v043.txt");
    bool retired = RetireLegacyChannels();
    for (const auto file : legacyGlobalStates)
        retired = retired && ReadStateForTest(DataDir() + L"\\" + file) == kNormalSpec;
    Check(retired, "migration retires every v0.3 through v0.4.2 global Liquid channel");
    Check(ReadStateForTest(DataDir() + L"\\state.v043.txt") == migrationLiquid,
          "retiring older global channels preserves v0.4.3 Liquid state");
    {
        const auto root = std::filesystem::path(DataDir()) / L"migration-package";
        const auto previousStage = root / L"bridge-v04-123";
        const auto previous041Stage = root / L"bridge-v041-456";
        const auto previous042Stage = root / L"bridge-v042-789";
        const auto currentStage = root / L"bridge-v043-987";
        std::filesystem::create_directories(previousStage);
        std::filesystem::create_directories(previous041Stage);
        std::filesystem::create_directories(previous042Stage);
        std::filesystem::create_directories(currentStage);
        for (const auto& directory : {root, previousStage, previous041Stage, previous042Stage, currentStage})
            for (const auto file : {L"state.txt", L"state.v04.txt", L"state.v041.txt", L"state.v042.txt"})
                WriteState(migrationLiquid, (directory / file).wstring());
        bool packageRetired = PublishPackageStates(root.wstring(), migrationLiquid);
        bool currentPublished = true;
        for (const auto& directory : {root, previousStage, previous041Stage, previous042Stage, currentStage})
        {
            for (const auto file : {L"state.txt", L"state.v04.txt", L"state.v041.txt", L"state.v042.txt"})
                packageRetired = packageRetired && ReadStateForTest((directory / file).wstring()) == kNormalSpec;
            currentPublished = currentPublished && ReadStateForTest((directory / L"state.v043.txt").wstring()) == migrationLiquid;
        }
        Check(packageRetired, "migration retires v0.3 through v0.4.2 package Liquid channels in stable and staged directories");
        Check(currentPublished, "v0.4.3 package Liquid state remains independent of retired package channels");
    }
    Check(StatePath() == DataDir() + L"\\state.v043.txt",
          "current taskbar state is isolated from a resident legacy hook");
    Check(ReadyMarkerPath(DataDir() + L"\\state.v043.txt", 1234) == DataDir() + L"\\ready.v043.1234.txt",
          "a legacy readiness marker cannot acknowledge the current diagnostics identity");
    Check(std::wstring(kReadyEventName) == L"Local\\NovaBar.Bridge.Ready.v043"
          && std::wstring(kFailedEventName) == L"Local\\NovaBar.Bridge.Failed.v043",
          "readiness events belong to the current bridge identity");
    CLSID parsedSite{};
    const GUID currentSite = {0xc352328e, 0x76ec, 0x4ab1, {0xab, 0x9f, 0x01, 0xe6, 0x8f, 0x04, 0x2a, 0xc8}};
    Check(SUCCEEDED(CLSIDFromString(kSiteClsidValue, &parsedSite))
          && IsEqualGUID(parsedSite, currentSite) && IsEqualGUID(parsedSite, bridge_id::SiteClsid),
          "diagnostics activation and bridge registration share a fresh v0.4.3 class identity");
    if (failures) return 1;
    {
        unsigned enumerations = 0;
        std::vector<ProcessRecord> roster{{L"StartMenuExperienceHost.exe", 101}, {L"ShellHost.exe", 102}};
        std::set<DWORD> live{101, 102};
        ProcessInventory inventory([&] { ++enumerations; return roster; },
            [&](DWORD pid, std::wstring const&) { return live.contains(pid); });
        Check(inventory.Find(L"startmenuexperiencehost.EXE", 1000) == 101,
              "process inventory matches executable names without case sensitivity");
        for (unsigned i = 0; i != 30; ++i)
        {
            inventory.Find(L"ShellHost.exe", 1001 + i);
            inventory.Find(L"AbsentHost.exe", 1001 + i);
        }
        Check(enumerations == 1, "all process-name lookups share one snapshot within a second");
        live.erase(101);
        Check(inventory.Find(L"StartMenuExperienceHost.exe", 1100) == 0 && enumerations == 1,
              "a dead cached host is rejected before the next process snapshot");
        roster.push_back({L"AbsentHost.exe", 103}); live.insert(103);
        Check(inventory.Find(L"AbsentHost.exe", 1999) == 0 && enumerations == 1,
              "negative process lookups remain cached only for the inventory interval");
        Check(inventory.Find(L"AbsentHost.exe", 2000) == 103 && enumerations == 2,
              "new shell hosts are discovered at the one-second refresh deadline");
        std::vector<std::thread> lookups;
        std::atomic<unsigned> matches{0};
        for (unsigned i = 0; i != 8; ++i) lookups.emplace_back([&]
        { if (inventory.Find(L"ShellHost.exe", 3000) == 102) ++matches; });
        for (auto& lookup : lookups) lookup.join();
        Check(matches == 8 && enumerations == 3, "concurrent state and injector lookups refresh a shared inventory once");
        ProcessInventory reusedPid([&] { return std::vector<ProcessRecord>{{L"WrongExecutable.exe", GetCurrentProcessId()}}; }, ProcessMatchesName);
        Check(reusedPid.Find(L"WrongExecutable.exe", 1000) == 0,
              "a reused cached PID with a different live executable is never returned");
        Check(!ProcessMatchesName(MAXDWORD, L"ShellHost.exe"), "nonexistent process IDs cannot pass cached-host validation");
        wchar_t selfPath[32768]{}; GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
        const auto selfName = std::filesystem::path(selfPath).filename().wstring();
        Check(ProcessMatchesName(GetCurrentProcessId(), selfName)
              && FindProcessByName(selfName.c_str()) == GetCurrentProcessId(),
              "production cached discovery accepts the real matching live process");
    }
    {
        unsigned enumerations = 0, foregroundQueries = 0, validations = 0;
        std::wstring foregroundName = L"ShellExperienceHost.exe";
        DWORD foregroundPid = 202;
        bool foregroundLive = true;
        ProcessInventory inventory([&]
        {
            ++enumerations;
            return std::vector<ProcessRecord>{{L"StartMenuExperienceHost.exe", 101}, {L"explorer.exe", 303}};
        }, [](DWORD, std::wstring const&) { return true; });
        Check(inventory.Find(L"ShellExperienceHost.exe", 1000) == 0 && enumerations == 1,
              "cold-host fixture has a cached negative lookup before its foreground window appears");
        const auto find = [&](std::wstring const& name)
        {
            return FindProcessWithForegroundHint(name, [&]
            { ++foregroundQueries; return foregroundPid; }, [&](DWORD pid, std::wstring const& candidate)
            {
                ++validations;
                return foregroundLive && pid == foregroundPid
                    && CompareStringOrdinal(candidate.c_str(), -1, foregroundName.c_str(), -1, TRUE) == CSTR_EQUAL;
            }, [&](std::wstring const& candidate) { return inventory.Find(candidate, 1001); });
        };
        Check(find(L"sHeLlExPeRiEnCeHoSt.ExE") == 202 && enumerations == 1,
              "a validated new foreground shell host bypasses the cached negative result without another snapshot");
        foregroundName = L"StartMenuExperienceHost.exe";
        Check(find(foregroundName) == 202,
              "the actual foreground shell host takes priority over another matching cached process");
        for (const auto host : {L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"ShellHost.exe", L"ShellExperienceHost.exe"})
        {
            foregroundName = host;
            Check(find(host) == 202, "each supported XAML shell host can use validated foreground discovery");
        }
        foregroundName = L"WrongExecutable.exe";
        Check(find(L"StartMenuExperienceHost.exe") == 101 && find(L"ShellExperienceHost.exe") == 0,
              "an unrelated foreground process falls back to the unchanged validated inventory");
        foregroundName = L"ShellExperienceHost.exe"; foregroundLive = false;
        Check(find(foregroundName) == 0, "a dead or reused foreground PID cannot bypass executable and lifetime validation");
        foregroundLive = true; foregroundPid = 0;
        const auto beforeZeroValidation = validations;
        Check(find(foregroundName) == 0 && validations == beforeZeroValidation,
              "an absent foreground owner falls back without validating PID zero");
        foregroundPid = 202; foregroundName = L"explorer.exe";
        const auto beforeOtherQuery = foregroundQueries;
        Check(find(L"explorer.exe") == 303 && foregroundQueries == beforeOtherQuery,
              "ordinary process lookup keeps cached identity semantics instead of choosing a foreground file manager");
        Check(find(L"") == 0 && enumerations == 1,
              "foreground discovery preserves empty-name handling and the shared snapshot limit");
    }
    Check(ContainsNoCase(L"Préfixe CAFÉ suffixe", L"café"),
          "ordinal substring matching ignores accented Latin case");
    Rule unicodeTitle;
    unicodeTitle.Kind = Rule::Kind::Title;
    unicodeTitle.Pattern = L"résumé σίγμα";
    const WindowSnapshot unicodeWindow{L"editor.exe", L"Document RÉSUMÉ ΣΊΓΜΑ", L"ÉditeurΩ", false, true};
    Check(MatchesWindow(unicodeTitle, unicodeWindow), "window title rules ignore Greek and accented Latin case");
    Rule unicodeClass;
    unicodeClass.Kind = Rule::Kind::Class;
    unicodeClass.Pattern = L"éditeurω";
    Check(MatchesWindow(unicodeClass, unicodeWindow), "window class rules use Unicode ordinal case matching");
    Check(ContainsNoCase(L"prefixÉCOLE", L"école") && ContainsNoCase(L"工作窗口：中文标题", L"中文标题"),
          "Unicode matching includes final-position substrings and preserves Chinese titles");
    Check(!ContainsNoCase(L"Résumé", L"resume") && !ContainsNoCase(L"short", L"longer-pattern")
          && !ContainsNoCase(L"", L"a") && !ContainsNoCase(L"a", L""),
          "ordinal matching preserves accents and rejects empty or oversized substrings");
    SaveApply(L"enabled=0\ndesktop=accent=color;color=#FFFF0000\n");
    const Config config = LoadApply();
    Snapshot desktop;
    desktop.State = L"desktop";
    Check(PickSpec(config, desktop).find(L"accent=normal") == 0,
          "disabled surface restores system appearance");
    SaveApply(L"enabled=1\n");
    Check(PickSpec(LoadApply(), desktop).find(L"accent=normal") == 0,
          "missing disabled desktop appearance restores system control");
    SaveApply(L"desktop=accent=color;color=#FFFF0000\nrule=title|document|title\nrule=process|notepad|process\n");
    Snapshot window;
    window.State = L"visible";
    window.Process = L"Notepad.exe";
    window.Title = L"document";
    Check(PickSpec(LoadApply(), window) == L"process", "process rules outrank title rules regardless of order");
    SaveApply(L"desktop=desktop\nvisible=visible\nmaximized=maximized\nrule=process|notepad|common\nrule.visible=process|notepad|visible-rule\nrule.maximized=process|notepad|max-rule\nrule.maximized.inactive=process|notepad|max-inactive\n");
    auto scoped = LoadApply();
    Check(PickSpec(scoped, window) == L"visible-rule", "visible rule stays scoped to a normal window");
    window.State = L"maximized";
    Check(PickSpec(scoped, window) == L"max-rule", "maximized rule remains independent of visible rule");
    window.WindowIsForeground = false;
    Check(PickSpec(scoped, window) == L"max-inactive", "inactive rule attaches to the same state scope");
    window.WindowIsForeground = true;
    scoped.Appearances.erase(L"maximized");
    Check(PickSpec(scoped, window) == L"max-rule", "scoped rules still apply with direct state appearance disabled");
    SaveApply(L"desktop=desktop\nrule=process|notepad|common\nrule.visible=title|document|scoped-title\n");
    window.State = L"visible";
    Check(PickSpec(LoadApply(), window) == L"scoped-title", "matching state scope precedes common process fallback");
    window.State = L"maximized";
    Check(PickSpec(LoadApply(), window) == L"common", "another state cannot consume visible-only rules");
    SaveApply(L"desktop=desktop\nvisible=visible\nvisible.inactive=inactive\nrule=process|reader%7C100%25|active-rule\nrule.inactive=process|reader%7C100%25|inactive-rule\nignore=title|private%0Awindow\n");
    auto parsed = LoadApply();
    Check(!parsed.StartSurface && !parsed.NotificationSurface, "unsupported surfaces are opt-in");
    Snapshot context;
    context.BatterySaver = true;
    context.SearchOpen = true;
    context.HasWindow = true;
    context.IsMaximized = true;
    context.WindowIsForeground = false;
    Check(PickSpec(parsed, context) == L"inactive", "disabled special and maximized states fall through to inactive visible");
    parsed.Appearances[L"search"] = L"search";
    Check(PickSpec(parsed, context) == L"search", "disabled battery falls through to active search");
    context.SearchOpen = false;
    context.Process = L"reader|100%.exe";
    Check(PickSpec(parsed, context) == L"inactive-rule", "escaped rule matches and uses inactive appearance");
    context.WindowIsForeground = true;
    Check(PickSpec(parsed, context) == L"active-rule", "active rule remains backward compatible");
    std::vector<WindowSnapshot> windows{
        {L"ignored.exe", L"private\nwindow", L"Main", false, true},
        {L"editor.exe", L"notes", L"Main", false, false},
    };
    const auto selected = SelectWindow(parsed, windows);
    Check(selected.Process == L"editor.exe" && selected.HasWindow && !selected.WindowIsForeground,
          "ignored foreground does not hide another visible window");
    windows.push_back({L"max.exe", L"maximized", L"Main", true, false});
    Check(SelectWindow(parsed, windows).Process == L"max.exe", "maximized window outranks foreground and z-order");
    windows[0].Title = L"ordinary";
    windows.pop_back();
    Check(SelectWindow(parsed, windows).Process == L"ignored.exe", "foreground outranks z-order among same window state");
    Check(SelectWindow(parsed, {}).State == L"desktop", "empty window set selects desktop");
    SaveApply(L"enabled=1\nsurface.start=1\nsurface.notification=1\ndesktop=desktop\n");
    parsed = LoadApply();
    Check(parsed.Enabled && parsed.StartSurface && parsed.NotificationSurface, "explicit surface opt-in is parsed");
    Check(DecodePattern(L"%257C%7C%0D%0A%2F") == L"%7C|\r\n%2F", "pattern escapes decode once and unknown sequences stay literal");

    const std::wstring visibility = DataDir() + L"\\visibility.start.txt";
    FILE* status = nullptr;
    _wfopen_s(&status, visibility.c_str(), L"w, ccs=UTF-8");
    fwprintf(status, L"1234 1"); fclose(status);
    bool visible = false;
    Check(ReadVisibility(visibility, 1234, visible) && visible, "visibility telemetry accepts the matching live host PID");
    Check(!ReadVisibility(visibility, 4321, visible), "visibility telemetry rejects a stale host PID");
    _wfopen_s(&status, visibility.c_str(), L"w, ccs=UTF-8");
    fwprintf(status, L"1234 4"); fclose(status);
    Check(!ReadVisibility(visibility, 1234, visible), "visibility telemetry rejects malformed visibility");
    Config shellStates;
    shellStates.Appearances = {{L"start", L"start-style"}, {L"search", L"search-style"}, {L"desktop", L"desktop-style"}};
    Snapshot sharedHost;
    sharedHost.StartOpen = sharedHost.SearchOpen = true;
    DisambiguateShellVisibility(sharedHost, true);
    Check(PickSpec(shellStates, sharedHost) == L"start-style", "authoritative Start visibility suppresses shared SearchHost visibility");
    Snapshot standaloneSearch;
    standaloneSearch.SearchOpen = true;
    DisambiguateShellVisibility(standaloneSearch, true);
    Check(PickSpec(shellStates, standaloneSearch) == L"search-style", "standalone Search remains active when Start is hidden");
    const std::wstring readyMarker = ReadyMarkerPath(StatePath(), 1234);
    std::atomic<bool> injectionEnabled{true};
    WriteState(L"1234", DataDir() + L"\\ready.1234.txt");
    WriteState(L"1234", DataDir() + L"\\ready.v04.1234.txt");
    WriteState(L"1234", DataDir() + L"\\ready.v041.1234.txt");
    WriteState(L"1234", DataDir() + L"\\ready.v042.1234.txt");
    Check(WaitForBridgeReady(readyMarker, 1234, nullptr, nullptr, injectionEnabled, 0) == WAIT_TIMEOUT,
          "resident v0.3 through v0.4.2 PID markers cannot acknowledge v0.4.3 readiness");
    WriteState(L"1234\n", readyMarker);
    Check(WaitForBridgeReady(readyMarker, 1234, nullptr, nullptr, injectionEnabled, 0) == WAIT_OBJECT_0,
          "PID readiness file succeeds without cross-integrity events");
    WriteState(L"4321", readyMarker);
    Check(WaitForBridgeReady(readyMarker, 1234, nullptr, nullptr, injectionEnabled, 0) == WAIT_TIMEOUT,
          "readiness file rejects stale PID content");
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    Check(WaitForBridgeReady(readyMarker, 1234, readyEvent, nullptr, injectionEnabled, 0) == WAIT_OBJECT_0,
          "readiness event remains backward compatible");
    CloseHandle(readyEvent);

    const std::wstring restoredOutput = DataDir() + L"\\concurrent-state.txt";
    WriteState(L"desired", restoredOutput);
    Check(PublishState(L"desired", restoredOutput), "unchanged desired state publishes successfully");
    WriteState(kNormalSpec, restoredOutput);
    Check(PublishState(L"desired", restoredOutput) && ReadStateForTest(restoredOutput) == L"desired",
          "same desired appearance repairs another process's restore output");
    std::atomic<unsigned> writeErrors{0};
    auto writeMany = [&](const wchar_t* spec)
    {
        for (unsigned i = 0; i < 300; ++i) if (!WriteState(spec, restoredOutput))
        { fprintf(stderr, "concurrent state write failed: %lu\n", GetLastError()); ++writeErrors; }
    };
    std::thread firstWriter(writeMany, L"first-whole-state");
    std::thread secondWriter(writeMany, L"second-whole-state");
    firstWriter.join(); secondWriter.join();
    const auto finalOutput = ReadStateForTest(restoredOutput);
    Check(writeErrors == 0 && (finalOutput == L"first-whole-state" || finalOutput == L"second-whole-state"),
          "concurrent writers publish complete states using independent temporary files");

    const HRESULT endpointAbsent = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (const unsigned available : {65U, 10000U})
    {
        unsigned scanned = 0;
        bool exactNames = true;
        const HRESULT result = ScanDiagnosticsConnections([&](const wchar_t* name)
        {
            ++scanned;
            exactNames = exactNames && std::wstring(name) == L"VisualDiagConnection" + std::to_wstring(scanned);
            return scanned == available ? S_OK : endpointAbsent;
        }, [] { return true; });
        Check(result == S_OK && scanned == available && exactNames,
              available == 65 ? "diagnostics scan reaches endpoint 65 with exact connection names"
                              : "diagnostics scan reaches endpoint 10000 with exact connection names");
    }
    {
        unsigned scanned = 0;
        const auto began = std::chrono::steady_clock::now();
        const HRESULT result = ScanDiagnosticsConnections([&](const wchar_t*)
        { ++scanned; return endpointAbsent; }, [] { return true; });
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
        printf("INFO: absent diagnostics fake scan: %u calls in %lld microseconds\n", scanned, elapsed);
        Check(result == endpointAbsent && scanned == 10000,
              "exhausted diagnostics scan preserves NOT_FOUND without fabricating success");
    }
    for (const HRESULT terminal : {E_ACCESSDENIED, E_FAIL, S_FALSE})
    {
        unsigned scanned = 0;
        const HRESULT result = ScanDiagnosticsConnections([&](const wchar_t*)
        { return ++scanned == 3 ? terminal : endpointAbsent; }, [] { return true; });
        Check(result == terminal && scanned == 3,
              "diagnostics scan stops immediately at any result other than NOT_FOUND");
    }
    {
        unsigned scanned = 0;
        bool enabled = false;
        const auto initialize = [&](const wchar_t*) { ++scanned; enabled = false; return endpointAbsent; };
        Check(ScanDiagnosticsConnections(initialize, [&] { return enabled; }) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && scanned == 0,
              "cancelled diagnostics scan never invokes initialization");
        enabled = true;
        Check(ScanDiagnosticsConnections(initialize, [&] { return enabled; }) == HRESULT_FROM_WIN32(ERROR_CANCELLED) && scanned == 1,
              "diagnostics scan stops before the next connection after cancellation");
    }
    {
        const DWORD caller = GetCurrentThreadId();
        bool separateThread = true;
        bool freshThreadState = true;
        bool mtaInitialized = true;
        for (unsigned attempt = 0; attempt < 2; ++attempt)
        {
            const HRESULT result = RunDiagnosticsAttempt([&](const wchar_t*)
            {
                static thread_local unsigned localAttempts = 0;
                freshThreadState = freshThreadState && ++localAttempts == 1;
                separateThread = separateThread && GetCurrentThreadId() != caller;
                APTTYPE apartment{};
                APTTYPEQUALIFIER qualifier{};
                mtaInitialized = mtaInitialized && CoGetApartmentType(&apartment, &qualifier) == S_OK && apartment == APTTYPE_MTA;
                return S_OK;
            }, [] { return true; });
            Check(result == S_OK, "diagnostics attempt returns its scanner result after joining");
        }
        Check(separateThread && freshThreadState, "each diagnostics attempt uses a fresh thread without prior TLS state");
        Check(mtaInitialized, "fresh diagnostics attempt initializes COM as MTA before scanning");
    }

    InjectionRetryPolicy retry;
    Check(retry.CanAttempt(10, 1000), "new process can inject immediately");
    retry.Attempted(10, 1000);
    Check(!retry.CanAttempt(10, 15999) && retry.CanAttempt(10, 16000), "normal retry deadline is 15 seconds");
    retry.Failed(10, 1100, false);
    retry.Attempted(10, 16000); retry.Failed(10, 16100, false);
    retry.Attempted(10, 31000); retry.Failed(10, 31100, false);
    Check(!retry.CanAttempt(10, 31101) && !retry.CanAttempt(10, 3631099), "three failures suppress retries for a full hour");
    Check(retry.CanAttempt(10, 3631100) && retry.CanAttempt(11, 31101), "hourly deadline expires exactly and does not affect other PIDs");
    retry.Succeeded(10);
    Check(retry.CanAttempt(10, 32000), "successful injection clears old backoff");
    retry.Attempted(12, 1000);
    retry.Failed(12, 1100, true); retry.Failed(12, 1200, true); retry.Failed(12, 1300, true);
    Check(retry.CanAttempt(12, 16000), "taskbar retries are not suspended for one hour");
    for (unsigned i = 0; i != 5; ++i)
    {
        const ULONGLONG tick = 1000 + i * 15000ULL;
        Check(retry.CanAttempt(13, tick), "uninitialized XAML host remains eligible for normal retry");
        retry.Attempted(13, tick);
        Check(!retry.Failed(13, tick + 100, false, HRESULT_FROM_WIN32(ERROR_NOT_FOUND)),
              "missing diagnostics endpoint does not count as a failed injection");
    }
    retry.Attempted(13, 76000);
    Check(!retry.Failed(13, 76100, false), "first real failure after XAML warmup retains normal retry");

    // Run production stage publication against fake LOCALAPPDATA and DLL files.
    // --once never creates the injection worker, so no live shell process is touched.
    const auto originalLocal = LocalAppData();
    const std::wstring isolatedLocal = DataDir() + L"\\local";
    SetEnvironmentVariableW(L"LOCALAPPDATA", isolatedLocal.c_str());
    const DWORD searchPid = FindProcessByName(L"SearchHost.exe");
    HANDLE searchProcess = searchPid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, searchPid) : nullptr;
    if (searchProcess)
    {
        UINT32 familyLength = 0;
        if (GetPackageFamilyName(searchProcess, &familyLength, nullptr) == ERROR_INSUFFICIENT_BUFFER)
        {
            std::wstring family(familyLength, L'\0');
            if (GetPackageFamilyName(searchProcess, &familyLength, family.data()) == ERROR_SUCCESS)
            {
                family.resize(familyLength - 1);
                const auto searchRoot = isolatedLocal + L"\\Packages\\" + family + L"\\LocalState\\NovaBar";
                std::filesystem::create_directories(searchRoot);
                WriteState(std::to_wstring(searchPid) + L" 1", searchRoot + L"\\visibility.search.txt");
                Check(TakeSnapshot(Config{}).SearchOpen, "Search visibility reads its actual package stage directory");
                WriteState(std::to_wstring(searchPid) + L" 0", searchRoot + L"\\visibility.search.txt");
                Check(!TakeSnapshot(Config{}).SearchOpen, "packaged Search hidden telemetry overrides visible host windows");
            }
        }
        CloseHandle(searchProcess);
    }
    wchar_t testModule[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, testModule, ARRAYSIZE(testModule));
    const auto testDirectory = std::filesystem::path(testModule).parent_path();
    const auto legacyDll = testDirectory / L"NovaBar.XamlBridge.dll";
    Check(!std::filesystem::exists(legacyDll), "legacy DLL fixture has no production module to overwrite");
    if (!std::filesystem::exists(legacyDll))
    {
        { std::ofstream fake(legacyDll, std::ios::binary); fake << "legacy fixture DLL, never loaded"; }
        Check(GetNotificationPaths(testDirectory.wstring()).Directory.empty(),
              "an unversioned legacy DLL cannot satisfy the current bridge deployment");
        std::filesystem::remove(legacyDll);
    }
    const auto fakeDll = testDirectory / novabar::bridge_identity::FileName;
    // The test output directory contains no production bridge DLL.
    Check(!std::filesystem::exists(fakeDll), "stage fixture does not overwrite a real bridge DLL");
    if (!std::filesystem::exists(fakeDll))
    {
        { std::ofstream fake(fakeDll, std::ios::binary); fake << "fixture DLL, never loaded"; }
        const auto firstPaths = GetNotificationPaths(testDirectory.wstring());
        StageNotificationBridge(testDirectory.wstring(), firstPaths);
        WriteState(L"old-state", firstPaths.Directory + L"\\state.txt");
        const auto legacyStage = NotificationStageRoot() + L"\\bridge-legacy-03";
        std::filesystem::create_directories(legacyStage);
        WriteState(L"old-liquid", legacyStage + L"\\state.txt");
        std::filesystem::last_write_time(fakeDll, std::filesystem::last_write_time(fakeDll) + std::chrono::seconds(2));
        const auto secondPaths = GetNotificationPaths(testDirectory.wstring());
        Check(firstPaths.Directory != secondPaths.Directory, "bridge updates retain distinct version directories");
        Check(firstPaths.State == NotificationStageRoot() + L"\\" + bridge_id::PackageState && secondPaths.State == firstPaths.State,
              "notification state path is stable across DLL timestamps");
        Check(std::filesystem::path(firstPaths.Dll).filename() == L"NovaBar.XamlBridge.v043.dll"
              && firstPaths.Directory.find(L"bridge-v043-") != std::wstring::npos,
              "the package stage uses the current file identity and epoch directory");
        SaveApply(L"enabled=0\nsurface.notification=1\n");
        WriteState(L"enabled=1\ndesktop=accent=color;color=#FF102030\nvisible=accent=color;color=#FF102030\nmaximized=accent=color;color=#FF102030\nstart=accent=color;color=#FF102030\nsearch=accent=color;color=#FF102030\ntaskview=accent=color;color=#FF102030\nbattery=accent=color;color=#FF102030\n", DataDir() + L"\\apply.notification.txt");
        wchar_t once[] = L"--once";
        wchar_t* onceArgs[] = {testModule, once};
        for (const auto file : legacyGlobalStates)
            WriteState(L"old-liquid", DataDir() + L"\\" + file);
        g_running = true;
        Check(NovaBarEngineMain(2, onceArgs) == 0, "stage publication uses production once path without injection");
        const std::wstring desired = L"accent=color;color=#FF102030";
        Check(ReadStateForTest(NotificationStageRoot() + L"\\" + bridge_id::PackageState) == desired,
              "current notification state is published at stable root");
        Check(ReadStateForTest(firstPaths.Directory + L"\\" + bridge_id::PackageState) == desired,
              "an already-loaded current-epoch bridge directory receives updated appearance");
        bool localLegacyRetired = true;
        for (const auto file : legacyGlobalStates)
            localLegacyRetired = localLegacyRetired && ReadStateForTest(DataDir() + L"\\" + file) == kNormalSpec;
        Check(localLegacyRetired && ReadStateForTest(DataDir() + L"\\" + bridge_id::NotificationState) == desired,
              "current local Liquid/color state never reactivates any legacy epoch");
        Check(ReadStateForTest(NotificationStageRoot() + L"\\state.txt") == kNormalSpec
              && ReadStateForTest(firstPaths.Directory + L"\\state.txt") == kNormalSpec
              && ReadStateForTest(legacyStage + L"\\state.txt") == kNormalSpec,
              "current package material leaves stable and legacy staging channels retired");
        SaveApply(L"enabled=0\nsurface.notification=0\n");
        g_running = true;
        Check(NovaBarEngineMain(2, onceArgs) == 0
              && ReadStateForTest(NotificationStageRoot() + L"\\" + bridge_id::PackageState) == kNormalSpec
              && ReadStateForTest(firstPaths.Directory + L"\\" + bridge_id::PackageState) == kNormalSpec,
              "disabling notifications restores current and old loaded bridge channels");
        WriteState(desired, NotificationStageRoot() + L"\\" + bridge_id::PackageState);
        WriteState(desired, firstPaths.Directory + L"\\state.txt");
        RestoreStagedNotifications();
        Check(ReadStateForTest(NotificationStageRoot() + L"\\" + bridge_id::PackageState) == kNormalSpec
              && ReadStateForTest(NotificationStageRoot() + L"\\state.txt") == kNormalSpec
              && ReadStateForTest(firstPaths.Directory + L"\\state.txt") == kNormalSpec,
              "restore updates stable and legacy notification paths");
        std::filesystem::remove(fakeDll);
        g_running = true;
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", originalLocal.empty() ? nullptr : originalLocal.c_str());
    return failures == 0 ? 0 : 1;
}
