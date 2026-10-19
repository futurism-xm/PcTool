#include "app/startup_registration.h"
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace startup_registration {
std::wstring ValueName(const std::filesystem::path& input){
    auto root=std::filesystem::absolute(input).lexically_normal().parent_path().wstring();
    if(!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,root.c_str(),int(root.size()),root.data(),int(root.size()),nullptr,nullptr,0))throw std::runtime_error("Cannot normalize startup path");
    HCRYPTPROV provider{};HCRYPTHASH hash{};BYTE bytes[32]{};DWORD size=sizeof(bytes);
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))throw std::runtime_error("Cannot hash startup path");
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=FALSE;
    if(ok)ok=CryptHashData(hash,reinterpret_cast<const BYTE*>(root.data()),DWORD(root.size()*sizeof(wchar_t)),0)&&CryptGetHashParam(hash,HP_HASHVAL,bytes,&size,0);
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);
    if(!ok)throw std::runtime_error("Cannot hash startup path");
    std::wstring name=L"PcTool-";for(auto b:bytes){name+=L"0123456789ABCDEF"[b>>4];name+=L"0123456789ABCDEF"[b&15];}return name;
}
bool Apply(bool enabled,const std::filesystem::path& input,std::wstring& error,const std::wstring& nameSpace){
    error.clear();
    if(nameSpace!=L"PcTool"&&(nameSpace.size()!=43||nameSpace.substr(0,11)!=L"PcToolTest-"||!std::all_of(nameSpace.begin()+11,nameSpace.end(),[](wchar_t c){return iswxdigit(c)!=0;}))){error=L"无效的启动注册命名空间。";return false;}
    HKEY key{};
    try{
        const auto executable=std::filesystem::absolute(input).lexically_normal();
        const auto name=ValueName(executable),command=L"\""+executable.wstring()+L"\" --background";
        const auto path=nameSpace==L"PcTool"?L"Software\\Microsoft\\Windows\\CurrentVersion\\Run":L"Software\\"+nameSpace+L"\\Run";
        auto check=[](LSTATUS code){if(code!=ERROR_SUCCESS)throw code;};
        if(enabled){const auto a=GetFileAttributesW(executable.c_str());if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_DIRECTORY))throw LSTATUS(ERROR_FILE_NOT_FOUND);}
        auto opened=RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_QUERY_VALUE|KEY_SET_VALUE,&key);
        if(opened==ERROR_FILE_NOT_FOUND&&!enabled)return true;
        if(opened==ERROR_FILE_NOT_FOUND)check(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr));else check(opened);
        auto read=[&](const wchar_t* value,std::wstring& text,bool strict=true){
            DWORD type{},bytes{};auto code=RegQueryValueExW(key,value,nullptr,&type,nullptr,&bytes);
            if(code==ERROR_FILE_NOT_FOUND)return false;check(code);
            if(type!=REG_SZ||bytes>65536){if(!strict)return false;throw LSTATUS(ERROR_INVALID_DATA);}
            std::vector<wchar_t> buffer(bytes/sizeof(wchar_t)+1);check(RegQueryValueExW(key,value,nullptr,&type,reinterpret_cast<BYTE*>(buffer.data()),&bytes));text=buffer.data();return true;
        };
        std::wstring previous;
        if(read(name.c_str(),previous)&&_wcsicmp(previous.c_str(),command.c_str())!=0)throw LSTATUS(ERROR_ALREADY_EXISTS);
        if(enabled){
            check(RegSetValueExW(key,name.c_str(),0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t))));
            std::wstring actual;if(!read(name.c_str(),actual)||actual!=command)throw LSTATUS(ERROR_WRITE_FAULT);
        }else{auto code=RegDeleteValueW(key,name.c_str());if(code!=ERROR_FILE_NOT_FOUND)check(code);std::wstring actual;if(read(name.c_str(),actual))throw LSTATUS(ERROR_WRITE_FAULT);}
        std::wstring legacy;if(read(L"PcTool",legacy,false)&&_wcsicmp(legacy.c_str(),command.c_str())==0)check(RegDeleteValueW(key,L"PcTool"));
        RegCloseKey(key);return true;
    }catch(LSTATUS code){error=L"无法更新当前用户的开机启动设置（错误 "+std::to_wstring(code)+L"）。";}
    catch(...){error=L"无法更新当前用户的开机启动设置。";}
    if(key)RegCloseKey(key);return false;
}
}
