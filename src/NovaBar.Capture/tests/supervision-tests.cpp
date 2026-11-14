// Exercise the production supervisor using a fixture helper that never captures
// or injects, and writes only within the supplied test directory.
#define wmain NovaBarEngineMain
#include "../../NovaBar.Engine/main.cpp"
#undef wmain

namespace
{
    int failures = 0;
    void Check(bool condition, char const* description)
    { printf("%s: %s\n", condition ? "PASS" : "FAIL", description); if (!condition) ++failures; }
    DWORD FixturePid(std::wstring const& directory)
    {
        std::ifstream input(std::filesystem::path(directory) / L"capture-fixture-pid.txt");
        DWORD pid = 0; input >> pid; return pid;
    }
    DWORD WaitForPid(std::wstring const& directory, DWORD previous = 0)
    {
        const auto deadline = GetTickCount64() + 2000;
        while (GetTickCount64() < deadline)
        {
            const DWORD pid = FixturePid(directory);
            if (pid && pid != previous) return pid;
            Sleep(10);
        }
        return 0;
    }
}
int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"--parent")
    {
        // This executable is copied to NovaBar.Capture.exe within the fixture.
        if (argc < 5 || std::wstring(argv[3]) != L"--watch-dir") return 2;
        std::ofstream output(std::filesystem::path(argv[4]) / L"capture-fixture-pid.txt", std::ios::trunc);
        output << GetCurrentProcessId(); output.close();
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(argv[2], nullptr, 10));
        if (!parent) return 2;
        WaitForSingleObject(parent, 10000); CloseHandle(parent); return 0;
    }
    if (argc != 2) return 2;
    const std::wstring directory = argv[1];
    std::filesystem::create_directories(directory);
    g_dataDirOverride = directory;
    CaptureBroker supervisor;
    supervisor.Tick(directory);
    Check(FixturePid(directory) == 0, "missing helper is never launched");
    // A distinct supervisor has no pending missing-file retry deadline.
    CaptureBroker present;
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    std::filesystem::copy_file(module, std::filesystem::path(directory) / L"NovaBar.Capture.exe");
    present.Tick(directory);
    const DWORD first = WaitForPid(directory);
    Check(first != 0, "resident supervisor launches its own helper beside engine");
    HANDLE firstProcess = first ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, first) : nullptr;
    if (firstProcess)
    {
        present.Tick(directory); Sleep(30);
        Check(FixturePid(directory) == first, "healthy helper is reused without duplicate launch");
        TerminateProcess(firstProcess, 17); WaitForSingleObject(firstProcess, 2000);
        present.Tick(directory);
        Sleep(100); present.Tick(directory);
        Check(FixturePid(directory) == first, "crash restart honors throttled backoff");
        Sleep(1000); present.Tick(directory);
        const DWORD second = WaitForPid(directory, first);
        Check(second && second != first, "crashed helper restarts after backoff");
        HANDLE secondProcess = second ? OpenProcess(SYNCHRONIZE, FALSE, second) : nullptr;
        present.Stop();
        Check(secondProcess && WaitForSingleObject(secondProcess, 2000) == WAIT_OBJECT_0,
            "engine stop tears down only its supervised child");
        if (secondProcess) CloseHandle(secondProcess);
        CloseHandle(firstProcess);
    }
    else { Check(false, "open owned fixture child for supervision test"); present.Stop(); }
    printf("Capture supervision tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
