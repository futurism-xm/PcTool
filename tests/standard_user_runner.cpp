#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <string>
#include <iostream>
#include <vector>
static std::wstring Quote(const std::wstring& value){std::wstring out=L"\"";size_t slashes=0;for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}out.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');return out+L'"';}
int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    bool interactive=false;int first=1;std::wstring log;
    while(first<argc){const std::wstring option=argv[first];if(option==L"--interactive"){interactive=true;++first;}else if(option==L"--log"&&first+1<argc){log=argv[first+1];first+=2;}else break;}
    if(argc<=first)return 2;
    HANDLE token{},restricted{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ALL_ACCESS,&token))return GetLastError();
    BYTE admin[SECURITY_MAX_SID_SIZE]{};DWORD adminSize=sizeof(admin);CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin,&adminSize);
    SID_AND_ATTRIBUTES disabled{admin,0};
    BOOL ok=CreateRestrictedToken(token,LUA_TOKEN|DISABLE_MAX_PRIVILEGE,1,&disabled,0,nullptr,0,nullptr,&restricted);DWORD error=GetLastError();CloseHandle(token);if(!ok)return error;
    PSID medium{};ConvertStringSidToSidW(L"S-1-16-8192",&medium);TOKEN_MANDATORY_LABEL label{};label.Label.Sid=medium;label.Label.Attributes=SE_GROUP_INTEGRITY;
    ok=SetTokenInformation(restricted,TokenIntegrityLevel,&label,DWORD(sizeof(label)+GetLengthSid(medium)));error=GetLastError();LocalFree(medium);if(!ok){CloseHandle(restricted);return error;}
    DWORD bytes{};GetTokenInformation(restricted,TokenUser,nullptr,0,&bytes);std::vector<BYTE> user(bytes);
    if(!GetTokenInformation(restricted,TokenUser,user.data(),bytes,&bytes)){CloseHandle(restricted);return GetLastError();}
    LPWSTR sid{};ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid);
    std::wstring security=L"D:(A;;GA;;;SY)(A;;GA;;;"+std::wstring(sid)+L")S:(ML;;NW;;;ME)";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor{};ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(),SDDL_REVISION_1,&descriptor,nullptr);
    BOOL present{},defaulted{};PACL dacl{};GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted);
    TOKEN_DEFAULT_DACL tokenDacl{dacl};SetTokenInformation(restricted,TokenDefaultDacl,&tokenDacl,sizeof(tokenDacl));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    std::wstring desktopName=L"PcToolTest-"+std::to_wstring(GetCurrentProcessId());
    HWINSTA previous=GetProcessWindowStation(),station=CreateWindowStationW(desktopName.c_str(),0,WINSTA_ALL_ACCESS,&attributes);
    if(!station){LocalFree(descriptor);CloseHandle(restricted);return GetLastError();}
    SetProcessWindowStation(station);
    HDESK desktop=CreateDesktopW(L"Default",nullptr,nullptr,0,GENERIC_ALL,&attributes);LocalFree(descriptor);
    SetProcessWindowStation(previous);
    if(!desktop){CloseWindowStation(station);CloseHandle(restricted);return GetLastError();}
    std::wstring command;for(int i=first;i<argc;++i){if(i>first)command+=L" ";command+=Quote(argv[i]);}
    STARTUPINFOEXW extended{};auto& startup=extended.StartupInfo;startup.cb=sizeof(extended);PROCESS_INFORMATION process{};
    HANDLE output{};SIZE_T attributeSize{};std::vector<BYTE> attributeBytes;
    DWORD flags=CREATE_NO_WINDOW;
    if(!log.empty()){
        SECURITY_ATTRIBUTES inheritance{sizeof(inheritance),nullptr,TRUE};
        output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&inheritance,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(output==INVALID_HANDLE_VALUE){CloseHandle(restricted);CloseDesktop(desktop);CloseWindowStation(station);return GetLastError();}
        startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=startup.hStdError=output;
        InitializeProcThreadAttributeList(nullptr,1,0,&attributeSize);attributeBytes.resize(attributeSize);
        extended.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBytes.data());
        InitializeProcThreadAttributeList(extended.lpAttributeList,1,0,&attributeSize);
        UpdateProcThreadAttribute(extended.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&output,sizeof(output),nullptr,nullptr);flags|=EXTENDED_STARTUPINFO_PRESENT;
    }else startup.cb=sizeof(STARTUPINFOW);
    std::wstring desktopPath=interactive?L"winsta0\\default":desktopName+L"\\Default";startup.lpDesktop=desktopPath.data();
    ok=CreateProcessAsUserW(restricted,argv[first],command.data(),nullptr,nullptr,output!=nullptr,flags,nullptr,nullptr,&startup,&process);error=GetLastError();CloseHandle(restricted);
    if(output)CloseHandle(output);if(extended.lpAttributeList)DeleteProcThreadAttributeList(extended.lpAttributeList);
    if(!ok){CloseDesktop(desktop);CloseWindowStation(station);std::cerr<<"Cannot launch medium-integrity fixture: "<<error<<"\n";return error;}
    WaitForSingleObject(process.hProcess,120000);DWORD result{};
    if(!GetExitCodeProcess(process.hProcess,&result)||result==STILL_ACTIVE){TerminateProcess(process.hProcess,ERROR_TIMEOUT);result=ERROR_TIMEOUT;}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseDesktop(desktop);CloseWindowStation(station);return int(result);
}
