#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

inline void LaunchRemoteDesktop(HWND owner) {
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        MessageBoxW(owner, L"无法定位远程桌面连接程序。", L"PcTool", MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring executable = std::wstring(systemDirectory) + L"\\mstsc.exe";
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    launch.hwnd = owner;
    launch.lpVerb = L"open";
    launch.lpFile = executable.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&launch)) return;

    const std::wstring message = L"无法启动远程桌面连接。错误代码：" +
        std::to_wstring(GetLastError());
    MessageBoxW(owner, message.c_str(), L"PcTool", MB_OK | MB_ICONWARNING);
}
