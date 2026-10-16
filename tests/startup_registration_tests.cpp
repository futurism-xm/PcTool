#include "app/startup_registration.h"
#include "shared/platform/app_storage.h"
#include <fstream>
#include <iostream>

int wmain(int argc,wchar_t** argv){
    if(argc==2&&std::wstring(argv[1])==L"--background"){
        HANDLE token{};TOKEN_ELEVATION elevation{};DWORD size{},session{};
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return 2;
        const bool ok=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size)!=FALSE;CloseHandle(token);
        ProcessIdToSessionId(GetCurrentProcessId(),&session);
        std::filesystem::create_directories(app_storage::Root()/L"Data");
        std::wofstream file(app_storage::Root()/L"Data/startup-token.txt");
        file<<app_storage::UserSid()<<L"\n"<<session<<L"\n"<<elevation.TokenIsElevated<<L"\n";
        return ok&&elevation.TokenIsElevated?0:3;
    }
    if(argc!=4)return 2;
    std::wstring error;const bool ok=startup_registration::Apply(std::wstring(argv[3])==L"enable",argv[2],error,argv[1]);
    if(!ok){const int size=WideCharToMultiByte(CP_UTF8,0,error.c_str(),int(error.size()),nullptr,0,nullptr,nullptr);std::string message(size,'\0');WideCharToMultiByte(CP_UTF8,0,error.c_str(),int(error.size()),message.data(),size,nullptr,nullptr);std::cerr<<message<<"\n";}
    return ok?0:1;
}
