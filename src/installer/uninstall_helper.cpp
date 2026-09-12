#define UNICODE
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
static bool Equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
static std::wstring Canonical(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return {};
    wchar_t text[32768];
    DWORD count = GetFinalPathNameByHandleW(file.value, text, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return count && count < 32768 ? std::wstring(text, count) : std::wstring();
}
static std::wstring Image(HANDLE process) {
    wchar_t image[32768]; DWORD count = 32768;
    return QueryFullProcessImageNameW(process, 0, image, &count) ? Canonical(std::wstring(image, count)) : L"";
}
struct Process { DWORD id; HANDLE handle; };
static std::vector<Process> Owned(const std::vector<std::wstring>& images) {
    std::vector<Process> result;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{sizeof(entry)};
    if (snapshot.value == INVALID_HANDLE_VALUE) throw GetLastError();
    for (BOOL more = Process32FirstW(snapshot.value, &entry); more; more = Process32NextW(snapshot.value, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!query.value) continue;
        auto image = Image(query.value);
        if (image.empty() || std::none_of(images.begin(), images.end(), [&](auto& item) { return Equal(image, item); })) continue;
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!handle) { std::wcerr << L"Cannot open owned process " << entry.th32ProcessID << L" error " << GetLastError() << L"\n"; throw DWORD(ERROR_ACCESS_DENIED); }
        // Revalidate identity on the handle we will terminate; never trust a stale PID.
        if (!Equal(Image(handle), image)) { CloseHandle(handle); continue; }
        result.push_back({entry.th32ProcessID, handle});
    }
    return result;
}
static BOOL CALLBACK CloseOwnedWindow(HWND window, LPARAM data) {
    // A console can host multiple clients. Closing it may terminate unrelated jobs.
    wchar_t className[128]{}; GetClassNameW(window, className, 128);
    if (Equal(className, L"ConsoleWindowClass") || Equal(className, L"PseudoConsoleWindow")) return TRUE;
    auto& processes = *reinterpret_cast<std::vector<Process>*>(data);
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    for (auto& process : processes) if (process.id == pid) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}
static DWORD Stop(const fs::path& root) {
    Handle marker(CreateFileW((root / L".pctool-uninstalling").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (marker.value == INVALID_HANDLE_VALUE) return GetLastError();
    std::vector<std::wstring> images;
    for (auto relative : {L"PcTool.exe", L"modules/archive/7z.exe", L"modules/archive/7zG.exe", L"modules/archive/7zFM.exe"}) {
        auto name = Canonical(root / relative); if (!name.empty()) images.push_back(name);
    }
    DWORD failure = 0;
    // Only the initial batch has a grace period. New arrivals are not allowed to prolong uninstall.
    for (int pass = 0; pass != 3; ++pass) {
        auto processes = Owned(images);
        if (processes.empty()) return failure;
        if (!pass) {
            EnumWindows(CloseOwnedWindow, reinterpret_cast<LPARAM>(&processes));
            const ULONGLONG deadline = GetTickCount64() + 2000;
            for (auto& process : processes) {
                auto now = GetTickCount64();
                WaitForSingleObject(process.handle, now < deadline ? static_cast<DWORD>(deadline - now) : 0);
            }
        }
        for (auto& process : processes) {
            if (WaitForSingleObject(process.handle, 0) == WAIT_TIMEOUT) {
                std::wcout << L"Terminating owned process " << process.id << L"\n";
                if (!TerminateProcess(process.handle, ERROR_INSTALL_USEREXIT) && WaitForSingleObject(process.handle, 0) != WAIT_OBJECT_0) failure = GetLastError();
            }
        }
        const ULONGLONG deadline = GetTickCount64() + 3000;
        for (auto& process : processes) {
            auto now = GetTickCount64();
            if (WaitForSingleObject(process.handle, now < deadline ? static_cast<DWORD>(deadline - now) : 0) != WAIT_OBJECT_0) failure = ERROR_TIMEOUT;
            CloseHandle(process.handle);
        }
    }
    auto remaining = Owned(images);
    if (!remaining.empty()) failure = ERROR_BUSY;
    for (auto& process : remaining) CloseHandle(process.handle);
    return failure;
}
static bool HasExtension(DWORD pid, const std::wstring& archive) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    MODULEENTRY32W module{sizeof(module)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    for (BOOL more = Module32FirstW(snapshot.value, &module); more; more = Module32NextW(snapshot.value, &module)) {
        auto name = Canonical(module.szExePath);
        if (name.size() <= archive.size() || _wcsnicmp(name.c_str(), archive.c_str(), archive.size()) || name[archive.size()] != L'\\') continue;
        auto file = fs::path(name).filename().wstring();
        if (Equal(file, L"7-zip.dll") || Equal(file, L"7-zip32.dll") || file.find(L"7-zip.dll.") == 0 || file.find(L"7-zip32.dll.") == 0) return true;
    }
    return false;
}
static DWORD ReleaseShell(const fs::path& root) {
    const auto archive = Canonical(root / L"modules/archive");
    if (archive.empty()) return 0;
    wchar_t windows[32768]; GetWindowsDirectoryW(windows, 32768);
    const fs::path explorer = fs::path(windows) / L"explorer.exe";
    auto candidates = Owned({Canonical(explorer)});
    DWORD result = 0, currentSession{}; ProcessIdToSessionId(GetCurrentProcessId(), &currentSession);
    for (auto& process : candidates) {
        Handle processHandle(process.handle);
        if (!HasExtension(process.id, archive)) continue;
        DWORD session{}; ProcessIdToSessionId(process.id, &session);
        // Never start an elevated replacement shell or disturb another logged-in desktop.
        if (session != currentSession) { result = ERROR_SUCCESS_REBOOT_REQUIRED; continue; }
        Handle token; Handle primary;
        if (!OpenProcessToken(process.handle, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &token.value) ||
            !DuplicateTokenEx(token.value, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary.value)) {
            result = ERROR_SUCCESS_REBOOT_REQUIRED; continue;
        }
        TOKEN_ELEVATION elevation{}; DWORD bytes{};
        if (!GetTokenInformation(primary.value, TokenElevation, &elevation, sizeof(elevation), &bytes) || elevation.TokenIsElevated) {
            result = ERROR_SUCCESS_REBOOT_REQUIRED; continue;
        }
        std::wcout << L"Restarting Explorer extension host " << process.id << L"\n";
        if (!TerminateProcess(process.handle, 0) || WaitForSingleObject(process.handle, 5000) != WAIT_OBJECT_0) {
            result = ERROR_SUCCESS_REBOOT_REQUIRED; continue;
        }
        // Windows may have already restarted its shell. Do not launch a duplicate.
        for (int i = 0; i < 20 && !GetShellWindow(); ++i) Sleep(100);
        if (GetShellWindow()) continue;
        STARTUPINFOW startup{sizeof(startup)}; startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
        PROCESS_INFORMATION replacement{};
        std::wstring command = L"\"" + explorer.wstring() + L"\"";
        void* environment{}; CreateEnvironmentBlock(&environment, primary.value, FALSE);
        BOOL started = CreateProcessWithTokenW(primary.value, 0, explorer.c_str(), command.data(),
            CREATE_UNICODE_ENVIRONMENT, environment, windows, &startup, &replacement);
        if (environment) DestroyEnvironmentBlock(environment);
        if (started) { CloseHandle(replacement.hThread); CloseHandle(replacement.hProcess); }
        else { std::wcerr << L"Explorer restart failed: " << GetLastError() << L"\n"; result = ERROR_RESTART_APPLICATION; }
    }
    return result;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return ERROR_INVALID_PARAMETER;
    try {
        auto root = fs::absolute(argv[2]).lexically_normal();
        for (auto ancestor = root; !ancestor.empty(); ancestor = ancestor.parent_path()) {
            DWORD attr = GetFileAttributesW(ancestor.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_BAD_PATHNAME;
            if (ancestor == ancestor.root_path()) break;
        }
        DWORD attributes = GetFileAttributesW(root.c_str());
        if (root == root.root_path() || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            (!fs::exists(root / L"PcTool.exe") && !fs::exists(root / L".pctool-managed-files.json"))) return ERROR_BAD_PATHNAME;
        if (Equal(argv[1], L"stop")) return static_cast<int>(Stop(root));
        if (Equal(argv[1], L"release-shell") && fs::exists(root / L".pctool-uninstalling")) return static_cast<int>(ReleaseShell(root));
        return ERROR_INVALID_PARAMETER;
    } catch (DWORD error) { return static_cast<int>(error); }
    catch (...) { return ERROR_UNHANDLED_EXCEPTION; }
}
