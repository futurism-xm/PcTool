#define wmain UninstallHelperEntry
#include "../src/installer/uninstall_helper.cpp"
#undef wmain
#include <sddl.h>

static void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Child {
    HANDLE handle{};
    ~Child(){if(handle){if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT)TerminateProcess(handle,0);WaitForSingleObject(handle,5000);CloseHandle(handle);}}
};
struct Identity {std::wstring sid;DWORD session{};DWORD elevated{};};
static Identity ReadIdentity(HANDLE process){
    Handle token;Check(OpenProcessToken(process,TOKEN_QUERY,&token.value)!=FALSE,"read token failed");
    DWORD size{};GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
    Check(GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size)!=FALSE,"read SID failed");
    LPWSTR sid{};Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)!=FALSE,"SID conversion failed");
    Identity result;result.sid=sid;LocalFree(sid);TOKEN_ELEVATION elevation{};
    Check(GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&size)!=FALSE,"read elevation failed");
    Check(GetTokenInformation(token.value,TokenSessionId,&result.session,sizeof(result.session),&size)!=FALSE,"read session failed");
    result.elevated=elevation.TokenIsElevated;return result;
}
static void Ready(const fs::path& root,DWORD pid){
    for(int i=0;i<100&&!fs::exists(root/(std::to_wstring(pid)+L".ready"));++i)Sleep(50);
    Check(fs::exists(root/(std::to_wstring(pid)+L".ready")),"fixture not ready");
}
int wmain(int argc,wchar_t** argv){
    if(argc!=4)return 1;
    try{
        const fs::path root=fs::absolute(argv[3]);fs::create_directories(root/L"modules/archive");
        const auto executable=root/L"UnrelatedHost.exe",dll=root/L"modules/archive/7-zip.dll";
        fs::copy_file(argv[1],executable);fs::copy_file(argv[2],dll);
        auto command=L"\""+executable.wstring()+L"\" \""+root.wstring()+L"\" module";
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION info{};
        Check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&info)!=FALSE,"launch fixture failed");
        Child original{info.hProcess};CloseHandle(info.hThread);Ready(root,info.dwProcessId);
        const auto before=ReadIdentity(original.handle);
        Check(!DeleteFileW(dll.c_str()),"fixture DLL was not locked");
        const auto result=Stop(root);
        Check(result==0,"owned stop failed");
        Check(WaitForSingleObject(original.handle,0)==WAIT_TIMEOUT,"unrelated DLL host was terminated");
        Check(!DeleteFileW(dll.c_str()),"external host unexpectedly released DLL");
        Check(TerminateProcess(original.handle,0)!=FALSE,"fixture release failed");
        Check(WaitForSingleObject(original.handle,5000)==WAIT_OBJECT_0,"fixture did not exit");
        Check(DeleteFileW(dll.c_str())!=FALSE,"DLL still locked after fixture exit");
        std::cout<<"PASS unrelated mapped-DLL host survives owned stop; file removable only after host releases it\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}
