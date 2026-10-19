// Shared by PcTool and its bundled 7-Zip: private per-user data inside the install.
#pragma once
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <string>
#include <vector>
namespace PcToolUserDirectory {
inline std::wstring Sid(){
  HANDLE token=0;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return L"";
  DWORD size=0;GetTokenInformation(token,TokenUser,0,0,&size);std::vector<BYTE> data(size);
  BOOL ok=GetTokenInformation(token,TokenUser,&data[0],size,&size);CloseHandle(token);if(!ok)return L"";
  LPWSTR sid=0;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(&data[0])->User.Sid,&sid))return L"";
  std::wstring result=sid;LocalFree(sid);return result;
}
inline bool SafeDirectory(const std::wstring& path){
  DWORD a=GetFileAttributesW(path.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);
}
inline bool Private(const std::wstring& path,const std::wstring& user){
  PACL acl=0;PSID owner=0;PSECURITY_DESCRIPTOR descriptor=0;
  if(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,0,&acl,0,&descriptor)!=ERROR_SUCCESS)return false;
  LPWSTR text=0;bool ok=owner&&ConvertSidToStringSidW(owner,&text);
  if(ok){std::wstring sid=text;LocalFree(text);ok=sid==user||sid==L"S-1-5-18"||sid==L"S-1-5-32-544";}
  if(!acl)ok=false;
  for(DWORD i=0;ok&&i<acl->AceCount;++i){
    void* raw=0;if(!GetAce(acl,i,&raw)){ok=false;break;}
    ACE_HEADER* header=static_cast<ACE_HEADER*>(raw);if(header->AceFlags&INHERIT_ONLY_ACE)continue;
    if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
    if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){ok=false;break;}
    ACCESS_ALLOWED_ACE* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);text=0;
    if(!ConvertSidToStringSidW(&ace->SidStart,&text)){ok=false;break;}
    std::wstring sid=text;LocalFree(text);if(ace->Mask&&sid!=user&&sid!=L"S-1-5-18"&&sid!=L"S-1-5-32-544")ok=false;
  }
  LocalFree(descriptor);return ok;
}
inline std::wstring Get(const std::wstring& root,const wchar_t* category){
  const std::wstring sid=Sid();if(sid.empty()||!SafeDirectory(root))return L"";
  const std::wstring base=root+L"\\"+category;
  if(!CreateDirectoryW(base.c_str(),0)&&GetLastError()!=ERROR_ALREADY_EXISTS)return L"";
  if(!SafeDirectory(base))return L"";
  const std::wstring path=base+L"\\"+sid;
  const std::wstring sddl=L"O:"+sid+L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;"+sid+L")";
  PSECURITY_DESCRIPTOR descriptor=0;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,0))return L"";
  SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
  BOOL made=CreateDirectoryW(path.c_str(),&attributes);DWORD error=GetLastError();LocalFree(descriptor);
  if(!made&&error!=ERROR_ALREADY_EXISTS)return L"";
  return SafeDirectory(path)&&Private(path,sid)?path:L"";
}
}
