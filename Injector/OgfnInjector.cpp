// OgfnInjector.cpp -- offline private-server launcher helper.
// Starts FortniteClient-Win64-Shipping.exe, waits for it to unpack its compressed
// sections (Epic packing compresses .text at rest; scanning a suspended, still-packed
// image is what mass-failed Reboot's address finders), then injects the gameserver DLL.
// Rank-and-file Win32: CreateProcessW(suspend) -> ResumeThread -> WaitForInputIdle ->
// VirtualAllocEx -> WriteProcessMemory -> CreateRemoteThread(LoadLibraryW).
// Legit use only: your own private server, your own legally obtained client build,
// offline, no matchmaking. Boost/dependencies: none. x64, C++17, static CRT.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

static void Fail(const char* stage, DWORD err)
{
    fprintf(stderr, "[ogfn] %s failed (Win32 error %lu)\n", stage, (unsigned long)err);
    fflush(stderr);
    ExitProcess(2);
}

static std::wstring DirOf(const std::wstring& path)
{
    size_t cut = path.find_last_of(L"\\/");
    return (cut == std::wstring::npos) ? std::wstring(L".") : path.substr(0, cut);
}

static bool IsAbsolute(const std::wstring& p)
{
    return (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
           (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
}

// UE's client log lives in %LOCALAPPDATA%\FortniteGame\Saved\Logs. Boot gate: the engine
// prints "Game Engine Initialized" only once GEngine/world exist; injecting before it
// makes Reboot's server setup run against a null world context (observed 2026-10-17
// 21:25: 16x GetWorldFromContextObject null warnings, then AV in map-open).
static std::wstring ClientLogPath()
{
    wchar_t buf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        return L"";
    std::wstring p(buf);
    return p + L"\\FortniteGame\\Saved\\Logs\\FortniteGame.log";
}

static std::string ReadTail(const std::wstring& path, size_t bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};

    std::string out;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz))
    {
        LARGE_INTEGER start{};
        start.QuadPart = sz.QuadPart - (long long)bytes;
        if (start.QuadPart < 0) start.QuadPart = 0;
        if (SetFilePointerEx(h, start, nullptr, FILE_BEGIN))
        {
            out.resize((size_t)(sz.QuadPart - start.QuadPart));
            DWORD read = 0;
            if (ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr))
                out.resize(read);
        }
    }
    CloseHandle(h);
    return out;
}

static bool FileIsNewerThan(const std::wstring& path, const FILETIME& ref)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return false;
    return CompareFileTime(&fad.ftLastWriteTime, &ref) >= 0;
}

// Waits for the FRESH log (written after bootStart) to contain the engine-ready line.
static bool WaitForEngineInit(const FILETIME& bootStart, DWORD capMs)
{
    const std::wstring log = ClientLogPath();
    if (log.empty())
        return false;

    ULONGLONG t0 = GetTickCount64();
    bool fresh = false;
    while (GetTickCount64() - t0 < capMs)
    {
        if (!fresh)
            fresh = FileIsNewerThan(log, bootStart);

        if (fresh)
        {
            std::string tail = ReadTail(log, 65536);
            if (!tail.empty() &&
                tail.find("Game Engine Initialized") != std::string::npos)
                return true;
        }
        Sleep(250);
    }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"OgfnInjector -- offline private-server injector\n\n");
        wprintf(L"usage: OgfnInjector.exe <full\\path\\to\\Reboot.dll> "
                L"<full\\path\\to\\FortniteClient-Win64-Shipping.exe> [client args...]\n\n");
        wprintf(L"e.g.:  OgfnInjector.exe C:\\ogfn\\Reboot.dll C:\\ogfn\\3.5\\FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe -log\n");
        return 1;
    }

    const std::wstring dllPath = argv[1];
    const std::wstring exePath = argv[2];

    if (!IsAbsolute(dllPath) || !IsAbsolute(exePath))
    {
        fprintf(stderr, "[ogfn] both paths must be absolute.\n");
        return 1;
    }

    DWORD attrD = GetFileAttributesW(dllPath.c_str());
    DWORD attrE = GetFileAttributesW(exePath.c_str());
    if (attrD == INVALID_FILE_ATTRIBUTES || attrE == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "[ogfn] dll or exe not found at the given path.\n");
        return 1;
    }

    // Settle time after input-idle so the packed .text/.rdata are fully expanded and
    // executing. Default 1500 ms; override with /wait:<ms> anywhere after the exe path.
    DWORD settleMs = 1500;

    // Command line for the game: exe + passthrough args (e.g. -log), except /wait:N.
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    for (int i = 3; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a.rfind(L"/wait:", 0) == 0)
        {
            settleMs = (DWORD)_wtoi(a.c_str() + 6);
            continue;
        }
        cmdLine += L" ";
        cmdLine += a;
    }
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::wstring gameDir = DirOf(exePath);

    FILETIME bootStart{};
    GetSystemTimeAsFileTime(&bootStart);

    if (!CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, gameDir.c_str(), &si, &pi))
        Fail("CreateProcessW(CREATE_SUSPENDED)", GetLastError());

    // Resume immediately: the exe's own loader stub has to run to unpack the
    // compressed sections before any byte-pattern scanning can succeed.
    if (ResumeThread(pi.hThread) == (DWORD)-1)
        Fail("ResumeThread", GetLastError());

    // Engine-ready gate (matches the official launcher's wait on the client log).
    fprintf(stdout, "[ogfn] waiting for Game Engine Initialized (up to 60s)\n");
    fflush(stdout);
    bool engineReady = WaitForEngineInit(bootStart, 60000);
    fprintf(stdout, "[ogfn] engine gate: %s; settling %lu ms\n",
            engineReady ? "READY" : "TIMED OUT, injecting anyway", (unsigned long)settleMs);
    fflush(stdout);
    Sleep(settleMs);

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote) Fail("VirtualAllocEx", GetLastError());

    if (!WriteProcessMemory(pi.hProcess, remote, dllPath.c_str(), bytes, nullptr))
        Fail("WriteProcessMemory", GetLastError());

    FARPROC loadLibraryW = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!loadLibraryW) Fail("GetProcAddress(LoadLibraryW)", GetLastError());

    HANDLE th = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)loadLibraryW, remote, 0, nullptr);
    if (!th) Fail("CreateRemoteThread", GetLastError());

    DWORD wait = WaitForSingleObject(th, 30000);
    if (wait != WAIT_OBJECT_0) Fail("waiting on remote LoadLibraryW", WAIT_TIMEOUT);

    DWORD hmod = 0;
    GetExitCodeThread(th, &hmod);
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);

    if (hmod == 0)
    {
        fprintf(stderr, "[ogfn] remote LoadLibraryW returned NULL -- blocked or bad path.\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 2;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    wprintf(L"[ogfn] injected: DLL mapped at 0x%lx in pid %lu -- game already running.\n",
            (unsigned long)hmod, (unsigned long)pi.dwProcessId);
    wprintf(L"[ogfn] next you should see the gameserver's own log lines (or -log window).\n");
    return 0;
}
