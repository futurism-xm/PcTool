#pragma once
#include <windows.h>
#include <string>
namespace archive {
// Always launches the PcTool-packaged original 7-Zip file manager.
bool OpenManager(HWND owner, const std::wstring& path = {});
std::wstring ModuleDirectory();
}
