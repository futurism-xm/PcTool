#pragma once
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <set>
#include <stdexcept>

namespace app_storage {
inline std::filesystem::path Root() {
    wchar_t path[32768]{}; const auto n=GetModuleFileNameW(nullptr,path,32768);
    if(!n||n>=32768)throw std::runtime_error("Cannot locate application directory");
    return std::filesystem::path(path).parent_path();
}
inline std::wstring UserSid() {
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("Cannot read user identity");
    DWORD size{};GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> data(size);
    const bool ok=GetTokenInformation(token,TokenUser,data.data(),size,&size)!=FALSE;CloseHandle(token);
    if(!ok)throw std::runtime_error("Cannot read user identity");
    LPWSTR sid{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid))throw std::runtime_error("Cannot read SID");
    std::wstring result=sid;LocalFree(sid);return result;
}
inline std::filesystem::path Directory(const wchar_t* category) {
    auto path=Root()/category;
    for(int level=0;level<2;++level){
        const DWORD attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Data directory cannot be a reparse point");
        std::filesystem::create_directories(path);if(level==0)path/=UserSid();
    }return path;
}
inline std::filesystem::path Data(){return Directory(L"Data");}
inline std::filesystem::path Cache(){return Directory(L"Cache");}
inline std::filesystem::path Unique(const wchar_t* kind,const wchar_t* extension) {
    auto dir=Cache()/kind;std::filesystem::create_directories(dir);GUID id{};
    if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot allocate cache identity");
    wchar_t text[40]{};StringFromGUID2(id,text,40);return dir/(std::wstring(text)+extension);
}
inline void AtomicWrite(const std::filesystem::path& path,const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temp=path;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot write installation data directory");
    DWORD written{};const bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);CloseHandle(file);
    if(!ok||!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw std::runtime_error("Cannot save installation data");}
}
inline HGLOBAL FileDrop(const std::filesystem::path& path) {
    const auto text=path.wstring();const auto bytes=sizeof(DROPFILES)+(text.size()+2)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);if(!memory)return nullptr;
    auto drop=static_cast<DROPFILES*>(GlobalLock(memory));if(!drop){GlobalFree(memory);return nullptr;}
    drop->pFiles=sizeof(DROPFILES);drop->fWide=TRUE;memcpy(reinterpret_cast<BYTE*>(drop)+sizeof(DROPFILES),text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);return memory;
}
inline bool Reparse(const std::filesystem::path& path){const auto a=GetFileAttributesW(path.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT);}
// Only our per-user cache is traversed. Never follow junctions or symlinks.
inline void CleanCache() noexcept {
    try {
        auto root=Root()/L"Cache"/UserSid();if(!std::filesystem::exists(root)||Reparse(Root()/L"Cache")||Reparse(root))return;
        if(!OpenClipboard(nullptr))return;
        std::set<std::wstring> referenced;
        auto files=static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if(!files&&IsClipboardFormatAvailable(CF_HDROP)){CloseClipboard();return;}
        if(files){const auto count=DragQueryFileW(files,0xffffffff,nullptr,0);for(UINT i=0;i<count;++i){wchar_t p[32768]{};DragQueryFileW(files,i,p,32768);referenced.insert(std::filesystem::path(p).lexically_normal().wstring());}}
        CloseClipboard();
        std::error_code error;bool archiveActive=false;
        const auto archive=root/L"SevenZip";
        if(std::filesystem::exists(archive)&&!Reparse(archive))for(const auto& entry:std::filesystem::directory_iterator(archive,error)){
            if(entry.path().filename().wstring().find(L".active-")!=0)continue;
            HANDLE lease=CreateFileW(entry.path().c_str(),DELETE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(lease==INVALID_HANDLE_VALUE){archiveActive=true;break;}CloseHandle(lease);
        }
        for(std::filesystem::recursive_directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,error),end;it!=end;it.increment(error)){
            if(error){error.clear();continue;}const auto path=it->path();
            if(path==archive&&archiveActive){it.disable_recursion_pending();continue;}
            if(Reparse(path)){if(it->is_directory(error))it.disable_recursion_pending();continue;}
            if(!it->is_regular_file(error))continue;
            bool keep=false;for(const auto& item:referenced)if(_wcsicmp(item.c_str(),path.lexically_normal().c_str())==0){keep=true;break;}if(keep)continue;
            // A shared lease on an active file prevents this exclusive open.
            HANDLE file=CreateFileW(path.c_str(),DELETE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(file==INVALID_HANDLE_VALUE)continue;
            FILE_DISPOSITION_INFO dispose{TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&dispose,sizeof(dispose));CloseHandle(file);
        }
    }catch(...){}
}
inline void Initialize(){auto data=Data();Cache();auto probe=data/L"write-check.tmp";AtomicWrite(probe,"ok");DeleteFileW(probe.c_str());}
}
