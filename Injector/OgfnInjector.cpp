// OgfnInjector.cpp — offline private-server launcher helper.
// Starts FortniteClient-Win64-Shipping.exe suspended, injects the gameserver DLL, resumes.
// Rank-and-file Win32: CreateProcessW(CREATE_SUSPENDED) -> VirtualAllocEx ->
// WriteProcessMemory -> CreateRemoteThread(LoadLibraryW) -> ResumeThread.
// Legit use only: your own private server, your own legally obtained client build,
// offline, no matchmaking. Boost/dependencies: none. x64, C++17, static CRT.

#include <windows.h>
#include <stdio.h>
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

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"OgfnInjector — offline private-server injector\n\n");
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

    // Command line for the suspended game: exe + any passthrough args (e.g. -log for readable logs).
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    for (int i = 3; i < argc; ++i)
    {
        cmdLine += L" ";
        cmdLine += argv[i];
    }
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::wstring gameDir = DirOf(exePath);

    if (!CreateProcessW(exePath.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, gameDir.c_str(), &si, &pi))
        Fail("CreateProcessW(CREATE_SUSPENDED)", GetLastError());

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
        fprintf(stderr, "[ogfn] remote LoadLibraryW returned NULL — blocked or bad path.\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 2;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    wprintf(L"[ogfn] injected: DLL mapped at 0x%lx in pid %lu — game resuming.\n",
            (unsigned long)hmod, (unsigned long)pi.dwProcessId);
    wprintf(L"[ogfn] next you should see the gameserver's own log lines (or -log window).\n");
    return 0;
}
