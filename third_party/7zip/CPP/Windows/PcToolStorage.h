// PcTool distribution: module-local data, never process-global TEMP overrides.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <map>
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
inline std::wstring LegacyData(const std::wstring& root) {
  HANDLE token=0;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return L"";
  DWORD size=0;GetTokenInformation(token,TokenUser,0,0,&size);std::vector<BYTE> bytes(size);
  BOOL ok=GetTokenInformation(token,TokenUser,&bytes[0],size,&size);CloseHandle(token);if(!ok)return L"";
  LPWSTR sid=0;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(&bytes[0])->User.Sid,&sid))return L"";
  std::wstring path=root+L"\\Data\\"+sid;LocalFree(sid);return path;
}
struct CacheLease {HANDLE value;CacheLease():value(0){}~CacheLease(){if(value)CloseHandle(value);}private:CacheLease(const CacheLease&);CacheLease& operator=(const CacheLease&);};
inline std::wstring Directory(const wchar_t* category) {
  if(Uninstalling())return L"";
  std::wstring root=Root();if(root.empty())return L"";
  std::wstring path=root+L"\\"+category;if(!Mkdir(path))return L"";
  if(wcscmp(category,L"Cache")==0){
    SYSTEMTIME today={};GetLocalTime(&today);wchar_t date[16]={};
    wsprintfW(date,L"\\%04u-%02u-%02u",unsigned(today.wYear),unsigned(today.wMonth),unsigned(today.wDay));
    path+=date;if(!Mkdir(path))return L"";
  }
  path+=L"\\SevenZip";
  if(wcscmp(category,L"Data")==0 && GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES){
    const std::wstring legacy=LegacyData(root),source=legacy+L"\\SevenZip";
    const DWORD a=GetFileAttributesW(source.c_str()),parent=GetFileAttributesW(legacy.c_str());
    if(!legacy.empty()&&a!=INVALID_FILE_ATTRIBUTES){
      if((a&FILE_ATTRIBUTE_REPARSE_POINT)||(parent&FILE_ATTRIBUTE_REPARSE_POINT))return L"";
      if(!MoveFileExW(source.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))return L"";
      RemoveDirectoryW(legacy.c_str());
    }
  }
  if(!Mkdir(path))return L"";
  if(wcscmp(category,L"Cache")==0){
    // Keep a lease for every date used by this process, including across midnight.
    struct Lock {CRITICAL_SECTION value;Lock(){InitializeCriticalSection(&value);}~Lock(){DeleteCriticalSection(&value);}};
    static Lock lock;static std::map<std::wstring,CacheLease> leases;
    struct Guard {CRITICAL_SECTION* p;Guard(CRITICAL_SECTION* value):p(value){EnterCriticalSection(p);}~Guard(){LeaveCriticalSection(p);}} guard(&lock.value);
    HANDLE& lease=leases[path].value;
    if(!lease){wchar_t name[64];wsprintfW(name,L"\\.active-%lu",GetCurrentProcessId());lease=CreateFileW((path+name).c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(lease==INVALID_HANDLE_VALUE){lease=0;return L"";}}
  }return path;
}
inline HKEY Hive() {
  // Caller owns this root handle. Child keys retain the hive only while in use.
  const std::wstring dir=Directory(L"Data");if(dir.empty()){
    if(!Root().empty()){static bool warned=false;if(!warned){warned=true;MessageBoxW(0,L"Cannot write PcTool Data directory. Check installation permissions.",L"PcTool - 7-zip",MB_OK|MB_ICONERROR);}}
    return 0;
  }
  const std::wstring file=dir+L"\\settings.hiv";
  HKEY loaded=0;
  if(RegLoadAppKeyW(file.c_str(),&loaded,KEY_ALL_ACCESS,0,0)!=ERROR_SUCCESS)return 0;
  return loaded;
}
inline bool Redirect(HKEY& parent,LPCTSTR name) {
  if(parent!=HKEY_CURRENT_USER||!name)return true;
  const TCHAR prefix[]=TEXT("Software\\7-Zip");
  const size_t n=sizeof(prefix)/sizeof(TCHAR)-1;
  if(_tcsnicmp(name,prefix,n)!=0||(name[n]!=0&&name[n]!=TEXT('\\')))return true;
  parent=Hive();return parent!=0;
}
struct ScopedRedirect {
  HKEY key;bool valid;bool owned;
  ScopedRedirect(HKEY parent,LPCTSTR name):key(parent),valid(Redirect(key,name)),owned(valid&&key!=parent){}
  ~ScopedRedirect(){if(owned)RegCloseKey(key);}
private:
  ScopedRedirect(const ScopedRedirect&);
  ScopedRedirect& operator=(const ScopedRedirect&);
};
}
#endif
