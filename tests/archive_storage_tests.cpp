#include <windows.h>
#include "../third_party/7zip/CPP/Windows/PcToolStorage.h"
#include <iostream>
int main(int argc,char**){
    HKEY hive=PcToolStorage::Hive();if(!hive){std::cerr<<"hive unavailable "<<GetLastError();return 1;}
    HKEY parent=HKEY_CURRENT_USER;
    if(!PcToolStorage::Redirect(parent,L"Software\\7-Zip\\FM")||parent==HKEY_CURRENT_USER||!parent)return 2;
    HKEY key{};if(RegCreateKeyExW(parent,L"Software\\7-Zip\\FM",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr))return 3;
    DWORD value=42,size=sizeof(value);
    if(argc==1){if(RegSetValueExW(key,L"StorageFixture",0,REG_DWORD,reinterpret_cast<BYTE*>(&value),size))return 4;}
    else{value=0;if(RegQueryValueExW(key,L"StorageFixture",nullptr,nullptr,reinterpret_cast<BYTE*>(&value),&size)||value!=42)return 5;}
    RegFlushKey(key);RegCloseKey(key);
    RegCloseKey(parent);RegCloseKey(hive);
    HANDLE released=CreateFileW((PcToolStorage::Directory(L"Data")+L"\\settings.hiv").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(released==INVALID_HANDLE_VALUE)return 8;CloseHandle(released);
    const auto cache=PcToolStorage::Directory(L"Cache");SYSTEMTIME today{};GetLocalTime(&today);wchar_t suffix[40]{};
    wsprintfW(suffix,L"\\%04u-%02u-%02u\\SevenZip",unsigned(today.wYear),unsigned(today.wMonth),unsigned(today.wDay));
    if(cache!=PcToolStorage::Root()+L"\\Cache"+suffix||PcToolStorage::Directory(L"Data")!=PcToolStorage::Root()+L"\\Data\\SevenZip")return 6;
    if(PcToolStorage::Directory(L"Cache")!=cache)return 7;
    std::cout<<"PASS archive private hive and installation cache\n";return 0;
}
