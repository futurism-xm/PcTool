#pragma once
#include <windows.h>
#include <cstdio>
// Opt-in local timing diagnostics, without metrics or user content.
inline void TraceMonitor(const char* event){
    wchar_t path[32768]{};if(!GetEnvironmentVariableW(L"PCTOOL_MONITOR_TRACE",path,32768))return;
    HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;char line[256]{};int n=sprintf_s(line,"%llu %s\r\n",GetTickCount64(),event);DWORD written{};
    if(n>0)WriteFile(file,line,DWORD(n),&written,nullptr);CloseHandle(file);
}
