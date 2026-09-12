#pragma once
#include <windows.h>
#include <shellapi.h>
inline void LaunchTaskManager(HWND owner){auto result=ShellExecuteW(owner,L"open",L"taskmgr.exe",nullptr,nullptr,SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(result)<=32)MessageBoxW(owner,L"无法启动任务管理器。",L"PcTool",MB_OK|MB_ICONWARNING);}
