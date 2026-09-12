#include "system_tools/paint.h"
#include <shellapi.h>
#include <string>
void LaunchSystemPaint(HWND owner) {
    wchar_t systemDirectory[MAX_PATH]{}; GetSystemDirectoryW(systemDirectory,MAX_PATH);
    const std::wstring executable=std::wstring(systemDirectory)+L"\\mspaint.exe";
    SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask=SEE_MASK_FLAG_NO_UI; info.hwnd=owner; info.lpVerb=L"open"; info.lpFile=executable.c_str(); info.nShow=SW_SHOWNORMAL;
    if(ShellExecuteExW(&info)) return;
    DWORD error=GetLastError();
    const std::wstring message=(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)?L"未安装系统画图，请先安装 Windows 画图。":L"无法启动系统画图："+std::to_wstring(error);
    MessageBoxW(owner,message.c_str(),L"PcTool",MB_OK|MB_ICONWARNING);
}
