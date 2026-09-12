#include "shared/platform/app_storage.h"
#include "translation/source_config.h"
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <fstream>
#include <set>
#include <algorithm>
namespace translation {
using namespace winrt::Windows::Data::Json;
namespace {
std::wstring Protect(const std::wstring& text){
    if(text.empty())return {};
    DATA_BLOB input{DWORD(text.size()*sizeof(wchar_t)),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(text.data()))},output{};
    winrt::check_bool(CryptProtectData(&input,L"PcTool Translation",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output));
    DWORD length{};CryptBinaryToStringW(output.pbData,output.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&length);
    std::wstring encoded(length,L'\0');const BOOL ok=CryptBinaryToStringW(output.pbData,output.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded.data(),&length);
    LocalFree(output.pbData);winrt::check_bool(ok);encoded.resize(length);return encoded;
}
std::wstring Unprotect(const std::wstring& text){
    if(text.empty())return {};
    DWORD size{};winrt::check_bool(CryptStringToBinaryW(text.c_str(),DWORD(text.size()),CRYPT_STRING_BASE64,nullptr,&size,nullptr,nullptr));
    std::vector<BYTE> bytes(size);winrt::check_bool(CryptStringToBinaryW(text.c_str(),DWORD(text.size()),CRYPT_STRING_BASE64,bytes.data(),&size,nullptr,nullptr));
    DATA_BLOB input{size,bytes.data()},output{};
    winrt::check_bool(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output));
    std::wstring value(reinterpret_cast<wchar_t*>(output.pbData),output.cbData/sizeof(wchar_t));
    SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);return value;
}
void Put(JsonObject& object,const wchar_t* key,const std::wstring& value){object.SetNamedValue(key,JsonValue::CreateStringValue(value));}
}
const wchar_t* SourceName(SourceKind kind){switch(kind){case SourceKind::OpenAI:return L"OpenAI 兼容";case SourceKind::DeepL:return L"DeepL 翻译";case SourceKind::Azure:return L"微软 Azure 翻译";default:return L"百度通用翻译";}}
SourceConfig NewSource(SourceKind kind){SourceConfig s;s.kind=kind;s.name=SourceName(kind);GUID id{};winrt::check_hresult(CoCreateGuid(&id));wchar_t value[40]{};StringFromGUID2(id,value,40);s.id=value;if(kind==SourceKind::OpenAI)s.endpoint=L"https://api.openai.com/v1";return s;}
bool ContainsHan(const std::wstring& text){
    for(size_t i=0;i<text.size();++i){unsigned code=text[i];if(code>=0xD800&&code<=0xDBFF&&i+1<text.size()){const unsigned low=text[++i];code=0x10000+((code-0xD800)<<10)+(low-0xDC00);}
        if((code>=0x3400&&code<=0x9FFF)||(code>=0xF900&&code<=0xFAFF)||(code>=0x20000&&code<=0x323AF))return true;
    }return false;
}
std::wstring ValidateConfig(const SourceConfig& s){
    auto empty=[](const std::wstring& v){return v.find_first_not_of(L" \t\r\n")==std::wstring::npos;};
    if(empty(s.name))return L"请输入名称";
    if(empty(s.key))return L"请输入密钥";
    if(s.key.find_first_of(L"\r\n")!=std::wstring::npos||s.region.find_first_of(L"\r\n")!=std::wstring::npos)return L"密钥或区域不能包含换行";
    if(s.kind==SourceKind::OpenAI){if(empty(s.model))return L"请输入模型名称";
        URL_COMPONENTS url{sizeof(url)};url.dwHostNameLength=url.dwUserNameLength=url.dwPasswordLength=url.dwExtraInfoLength=DWORD(-1);
        if(!WinHttpCrackUrl(s.endpoint.c_str(),0,0,&url)||!url.dwHostNameLength||url.dwUserNameLength||url.dwPasswordLength||url.dwExtraInfoLength)return L"请输入有效 API 基础地址，不含用户名、查询参数或片段";
        if(url.nScheme!=INTERNET_SCHEME_HTTPS&&url.nScheme!=INTERNET_SCHEME_HTTP)return L"请求地址须使用 HTTP 或 HTTPS";
    }
    if(s.kind==SourceKind::Azure&&empty(s.region))return L"请输入 Azure 区域";
    if(s.kind==SourceKind::Baidu&&empty(s.appId))return L"请输入 App ID";
    return {};
}
SourceStore::SourceStore(std::filesystem::path dir):directory_(std::move(dir)){
    if(directory_.empty()){directory_=app_storage::Data()/L"Translation";}
}
std::vector<SourceConfig> SourceStore::Load(){
    const auto path=directory_/L"sources.json";if(!std::filesystem::exists(path))return {};
    std::ifstream file(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(file)),{});
    auto root=JsonObject::Parse(winrt::to_hstring(data));if(root.GetNamedNumber(L"version")!=1)throw winrt::hresult_error(E_FAIL,L"翻译源配置版本不受支持");
    std::vector<SourceConfig> result;std::set<std::wstring> ids;
    for(const auto& value:root.GetNamedArray(L"sources")){
        auto obj=value.GetObject();SourceConfig s;auto str=[&](const wchar_t* k){return std::wstring(obj.GetNamedString(k,L""));};
        const int kind=int(obj.GetNamedNumber(L"kind"));if(kind<0||kind>3)throw winrt::hresult_error(E_FAIL,L"翻译源类型无效");s.kind=SourceKind(kind);
        s.id=str(L"id");if(s.id.empty()||!ids.insert(s.id).second)throw winrt::hresult_error(E_FAIL,L"翻译源标识无效");
        s.name=str(L"name");s.endpoint=str(L"endpoint");s.model=str(L"model");s.region=str(L"region");s.appId=str(L"appId");s.key=Unprotect(str(L"secret"));
        s.pro=obj.GetNamedBoolean(L"pro",false);s.verified=obj.GetNamedBoolean(L"verified",false);s.enabled=s.verified&&obj.GetNamedBoolean(L"enabled",false);
        result.push_back(std::move(s));
    }return result;
}
void SourceStore::Save(const std::vector<SourceConfig>& sources){
    JsonObject root;root.SetNamedValue(L"version",JsonValue::CreateNumberValue(1));JsonArray list;
    for(const auto& s:sources){JsonObject obj;Put(obj,L"id",s.id);Put(obj,L"name",s.name);Put(obj,L"endpoint",s.endpoint);Put(obj,L"model",s.model);Put(obj,L"region",s.region);Put(obj,L"appId",s.appId);Put(obj,L"secret",Protect(s.key));
        obj.SetNamedValue(L"kind",JsonValue::CreateNumberValue(int(s.kind)));obj.SetNamedValue(L"pro",JsonValue::CreateBooleanValue(s.pro));obj.SetNamedValue(L"enabled",JsonValue::CreateBooleanValue(s.enabled&&s.verified));obj.SetNamedValue(L"verified",JsonValue::CreateBooleanValue(s.verified));list.Append(obj);}
    root.SetNamedValue(L"sources",list);const auto text=winrt::to_string(root.Stringify());std::filesystem::create_directories(directory_);
    const auto destination=directory_/L"sources.json",temp=directory_/L"sources.tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);winrt::check_bool(file!=INVALID_HANDLE_VALUE);
    DWORD written{};BOOL ok=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file);DWORD error=GetLastError();CloseHandle(file);
    if(!ok){DeleteFileW(temp.c_str());throw winrt::hresult_error(HRESULT_FROM_WIN32(error));}
    if(!MoveFileExW(temp.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error=GetLastError();DeleteFileW(temp.c_str());throw winrt::hresult_error(HRESULT_FROM_WIN32(error));}
}
}
