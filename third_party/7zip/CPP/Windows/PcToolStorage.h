// PcTool distribution: module-local data, never process-global TEMP overrides.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <tchar.h>
#include <string>
#include <vector>
extern "C" WINADVAPI LONG WINAPI RegLoadAppKeyW(LPCWSTR,PHKEY,REGSAM,DWORD,DWORD);
namespace PcToolStorage {
inline std::wstring ModuleDirectory() {
  HMODULE module=0;wchar_t path[32768]={};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ModuleDirectory),&module);
  DWORD n=GetModuleFileNameW(module,path,32768);if(!n||n>=32768)return L"";
  std::wstring result(path);return result.substr(0,result.find_last_of(L"\\/"));
}
inline std::wstring Root() {
  std::wstring path=ModuleDirectory();
  for(int i=0;i<4&&!path.empty();++i){
    if(GetFileAttributesW((path+L"\\PcTool.exe").c_str())!=INVALID_FILE_ATTRIBUTES ||
       GetFileAttributesW((path+L"\\.pctool-uninstalling").c_str())!=INVALID_FILE_ATTRIBUTES)return path;
    size_t slash=path.find_last_of(L"\\/");if(slash==std::wstring::npos)break;path.resize(slash);
  }return L"";
}
inline bool Mkdir(const std::wstring& p){DWORD a=GetFileAttributesW(p.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;return CreateDirectoryW(p.c_str(),0)||GetLastError()==ERROR_ALREADY_EXISTS;}
inline bool Uninstalling() {
  const std::wstring root=Root();
  return !root.empty() && GetFileAttributesW((root+L"\\.pctool-uninstalling").c_str())!=INVALID_FILE_ATTRIBUTES;
}
inline std::wstring Directory(const wchar_t* category) {
  if(Uninstalling())return L"";
  std::wstring root=Root();if(root.empty())return L"";
  HANDLE token=0;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return L"";
  DWORD size=0;GetTokenInformation(token,TokenUser,0,0,&size);std::vector<BYTE> bytes(size);
  BOOL ok=GetTokenInformation(token,TokenUser,&bytes[0],size,&size);CloseHandle(token);if(!ok)return L"";
  LPWSTR sid=0;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(&bytes[0])->User.Sid,&sid))return L"";
  std::wstring path=root+L"\\"+category;bool made=Mkdir(path);path+=L"\\";path+=sid;LocalFree(sid);
  if(!made)return L"";made=Mkdir(path);if(!made)return L"";path+=L"\\SevenZip";made=Mkdir(path);if(!made)return L"";
  if(wcscmp(category,L"Cache")==0){
    static HANDLE lease=INVALID_HANDLE_VALUE;
    if(lease==INVALID_HANDLE_VALUE){wchar_t name[64];wsprintfW(name,L"\\.active-%lu",GetCurrentProcessId());lease=CreateFileW((path+name).c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(lease==INVALID_HANDLE_VALUE)return L"";}
  }return path;
}
inline HKEY Hive() {
  // Windows shares the backing hive across module/process instances.
  static HKEY hive=0;if(hive)return hive;
  const std::wstring dir=Directory(L"Data");if(dir.empty()){
    if(!Root().empty()){static bool warned=false;if(!warned){warned=true;MessageBoxW(0,L"Cannot write PcTool Data directory. Check installation permissions.",L"PcTool - 7-zip",MB_OK|MB_ICONERROR);}}
    return 0;
  }
  const std::wstring file=dir+L"\\settings.hiv";
  HKEY loaded=0;
  if(RegLoadAppKeyW(file.c_str(),&loaded,KEY_ALL_ACCESS,0,0)!=ERROR_SUCCESS)return 0;
  auto previous=InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&hive),loaded,0);
  if(previous)RegCloseKey(loaded);return hive;
}
inline bool Redirect(HKEY& parent,LPCTSTR name) {
  if(parent!=HKEY_CURRENT_USER||!name)return true;
  const TCHAR prefix[]=TEXT("Software\\7-Zip");
  const size_t n=sizeof(prefix)/sizeof(TCHAR)-1;
  if(_tcsnicmp(name,prefix,n)!=0||(name[n]!=0&&name[n]!=TEXT('\\')))return true;
  parent=Hive();return parent!=0;
}
}
#endif
