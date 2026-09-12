#include <windows.h>
#include "../third_party/7zip/CPP/Windows/PcToolStorage.h"
#include <iostream>
int main(int argc,char**){
    HKEY hive=PcToolStorage::Hive();if(!hive){std::cerr<<"hive unavailable "<<GetLastError();return 1;}
    HKEY parent=HKEY_CURRENT_USER;
    if(!PcToolStorage::Redirect(parent,L"Software\\7-Zip\\FM")||parent!=hive)return 2;
    HKEY key{};if(RegCreateKeyExW(parent,L"Software\\7-Zip\\FM",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr))return 3;
    DWORD value=42,size=sizeof(value);
    if(argc==1){if(RegSetValueExW(key,L"StorageFixture",0,REG_DWORD,reinterpret_cast<BYTE*>(&value),size))return 4;}
    else{value=0;if(RegQueryValueExW(key,L"StorageFixture",nullptr,nullptr,reinterpret_cast<BYTE*>(&value),&size)||value!=42)return 5;}
    RegFlushKey(key);RegCloseKey(key);
    if(PcToolStorage::Directory(L"Cache").empty())return 6;
    std::cout<<"PASS archive private hive and installation cache\n";return 0;
}
