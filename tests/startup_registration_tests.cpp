#include "app/startup_registration.h"
#include "app/app_settings.h"
#include "shared/platform/app_storage.h"
#include <fstream>
#include <iostream>

int wmain(int argc,wchar_t** argv){
    if(argc==2&&std::wstring(argv[1])==L"--background"){
        HANDLE token{};TOKEN_ELEVATION elevation{};DWORD size{},session{};
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return 2;
        const bool ok=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size)!=FALSE;
        GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&size);std::vector<BYTE> integrity(size);
        if(!GetTokenInformation(token,TokenIntegrityLevel,integrity.data(),size,&size)){CloseHandle(token);return 4;}
        auto integritySid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
        DWORD level=*GetSidSubAuthority(integritySid,*GetSidSubAuthorityCount(integritySid)-1);CloseHandle(token);
        BYTE adminBuffer[SECURITY_MAX_SID_SIZE]{};DWORD adminSize=sizeof(adminBuffer);BOOL admin{};
        CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,adminBuffer,&adminSize);CheckTokenMembership(nullptr,adminBuffer,&admin);
        ProcessIdToSessionId(GetCurrentProcessId(),&session);
        std::wofstream file(app_storage::Data()/L"startup-token.txt");
        file<<app_storage::UserSid()<<L"\n"<<session<<L"\n"<<elevation.TokenIsElevated<<L"\n"<<level<<L"\n"<<admin<<L"\n";
        return ok?0:3;
    }
    if(argc==3&&std::wstring(argv[1])==L"--name"){std::wcout<<startup_registration::ValueName(argv[2]);return 0;}
    if(argc==3&&std::wstring(argv[1])==L"--exercise"){
        try{
            auto check=[](bool ok,const char* text){if(!ok)throw std::runtime_error(text);};
            app_storage::Initialize();
            const auto file=app_storage::Data()/L"settings.ini";
            app_storage::AtomicWrite(file,"[PcTool]\r\nVersion=1\r\nAutoStart=0\r\nTranslationEnabled=0\r\nVisibleItems=3\r\n");
            AppSettings settings;settings.Load();std::wstring error;
            check(settings.InitializeStartup(error,argv[2]),"startup migration failed");check(settings.AutoStartEnabled(),"upgrade did not enable startup");
            AppSettings migrated;migrated.Load();check(migrated.AutoStartEnabled()&&!migrated.TranslationEnabled(),"migration lost settings");
            migrated.SetAutoStartEnabled(false);check(migrated.ApplyAutoStart(&error,argv[2])&&migrated.Save(),"disable failed");
            AppSettings repeated;repeated.Load();check(repeated.InitializeStartup(error,argv[2])&&!repeated.AutoStartEnabled(),"upgrade reset disabled preference twice");
            app_storage::AtomicWrite(file,"[PcTool]\r\nVersion=1\r\nAutoStart=0\r\n");
            HANDLE locked=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            check(locked!=INVALID_HANDLE_VALUE,"lock fixture failed");AppSettings blocked;blocked.Load();bool saved=blocked.InitializeStartup(error,argv[2]);CloseHandle(locked);
            check(!saved,"save failure reported success");AppSettings retry;retry.Load();check(retry.InitializeStartup(error,argv[2]),"migration retry failed");
            retry.SetAutoStartEnabled(false);check(retry.ApplyAutoStart(&error,argv[2])&&retry.Save(),"final cleanup failed");
            std::cout<<"PASS per-user writes, one-time startup migration, user preference, save failure rollback and retry\n";return 0;
        }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    }
    if(argc!=4)return 2;
    std::wstring error;const bool ok=startup_registration::Apply(std::wstring(argv[3])==L"enable",argv[2],error,argv[1]);
    if(!ok){const int size=WideCharToMultiByte(CP_UTF8,0,error.c_str(),int(error.size()),nullptr,0,nullptr,nullptr);std::string message(size,'\0');WideCharToMultiByte(CP_UTF8,0,error.c_str(),int(error.size()),message.data(),size,nullptr,nullptr);std::cerr<<message<<"\n";}
    return ok?0:1;
}
