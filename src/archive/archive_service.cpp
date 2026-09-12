#include "archive/archive_service.h"
#include <filesystem>
#include <vector>
namespace archive {
std::wstring ModuleDirectory() {
    wchar_t executable[32768]{};
    DWORD length=GetModuleFileNameW(nullptr,executable,32768);
    if(!length || length>=32768)return {};
    return (std::filesystem::path(executable).parent_path()/L"modules"/L"archive").wstring();
}
static std::wstring Quote(const std::wstring& value) {
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){++slashes;continue;}
        out.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;out+=c;
    }
    out.append(slashes*2,L'\\');return out+L'"';
}
bool OpenManager(HWND owner,const std::wstring& path) {
    auto directory=ModuleDirectory();auto executable=(std::filesystem::path(directory)/L"7zFM.exe").wstring();
    std::wstring command=Quote(executable);if(!path.empty())command+=L" "+Quote(path);
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    if(!directory.empty() && CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&startup,&process)) {
        CloseHandle(process.hThread);CloseHandle(process.hProcess);return true;
    }
    DWORD error=GetLastError();wchar_t* systemMessage=nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,reinterpret_cast<wchar_t*>(&systemMessage),0,nullptr);
    std::wstring message=L"无法启动内置 7-Zip，请重新安装 PcTool 修复压缩模块。\n\n"+executable+L"\n"+(systemMessage?systemMessage:std::to_wstring(error));
    if(systemMessage)LocalFree(systemMessage);
    MessageBoxW(owner,message.c_str(),L"PcTool · 7-zip",MB_OK|MB_ICONERROR);return false;
}
}
