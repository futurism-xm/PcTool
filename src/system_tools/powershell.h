#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>

inline void LaunchAdministratorPowerShell(HWND owner) {
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    DWORD error = 0;
    if (length == 0 || length >= MAX_PATH) {
        error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
    } else {
        // Use Windows' own executable rather than searching PATH or the current directory.
        const std::wstring executable = std::wstring(systemDirectory) +
            L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        launch.hwnd = owner;
        launch.lpVerb = L"runas";
        launch.lpFile = executable.c_str();
        launch.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&launch)) return;
        error = GetLastError();
        if (error == ERROR_CANCELLED) return;
    }
    const std::wstring message = L"无法以管理员身份启动 PowerShell。错误代码：" +
        std::to_wstring(error);
    MessageBoxW(owner, message.c_str(), L"PcTool", MB_OK | MB_ICONWARNING);
}
