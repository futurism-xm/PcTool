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
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Data directory cannot be a reparse point");
    std::filesystem::create_directories(path);return path;
}
// Upgrade only this user's old configuration. Never overwrite a flat-layout value.
inline void MergeLegacyData(const std::filesystem::path& source,const std::filesystem::path& target) {
    if(source==target)return;
    const auto attributes=GetFileAttributesW(source.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)return;
    if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Legacy data cannot be a reparse point");
    const auto targetAttributes=GetFileAttributesW(target.c_str());
    if(targetAttributes!=INVALID_FILE_ATTRIBUTES&&(targetAttributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Data target cannot be a reparse point");
    if(targetAttributes==INVALID_FILE_ATTRIBUTES) {
        if(!MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot migrate legacy data; close programs using it");
        return;
    }
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)||!(targetAttributes&FILE_ATTRIBUTE_DIRECTORY))return;
    for(const auto& entry:std::filesystem::directory_iterator(source))MergeLegacyData(entry.path(),target/entry.path().filename());
    RemoveDirectoryW(source.c_str());
}
inline std::filesystem::path Data(){auto root=Directory(L"Data");MergeLegacyData(root/UserSid(),root);return root;}
inline std::filesystem::path CacheRoot(){return Directory(L"Cache");}
inline bool CalendarDay(SYSTEMTIME date,ULONGLONG& day) {
    date.wHour=date.wMinute=date.wSecond=date.wMilliseconds=0;
    FILETIME file{};SYSTEMTIME checked{};
    if(!SystemTimeToFileTime(&date,&file)||!FileTimeToSystemTime(&file,&checked)||
       checked.wYear!=date.wYear||checked.wMonth!=date.wMonth||checked.wDay!=date.wDay)return false;
    day=((ULONGLONG(file.dwHighDateTime)<<32)|file.dwLowDateTime)/864000000000ULL;return true;
}
inline std::wstring DateName(const SYSTEMTIME& date) {
    wchar_t name[16]{};swprintf_s(name,L"%04u-%02u-%02u",unsigned(date.wYear),unsigned(date.wMonth),unsigned(date.wDay));return name;
}
inline bool ParseDate(const std::wstring& name,ULONGLONG& day) {
    if(name.size()!=10||name[4]!=L'-'||name[7]!=L'-')return false;
    for(size_t i=0;i<name.size();++i)if(i!=4&&i!=7&&(name[i]<L'0'||name[i]>L'9'))return false;
    SYSTEMTIME date{};date.wYear=WORD(std::stoi(name.substr(0,4)));date.wMonth=WORD(std::stoi(name.substr(5,2)));date.wDay=WORD(std::stoi(name.substr(8,2)));
    return CalendarDay(date,day);
}
inline std::filesystem::path CacheChild(const std::filesystem::path& parent,const wchar_t* name) {
    const std::wstring component=name;
    if(component.empty()||component==L"."||component==L".."||component.find_first_of(L"/\\:")!=std::wstring::npos)throw std::runtime_error("Invalid cache category");
    const auto path=parent/component;const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Cache directory cannot be a reparse point");
    std::filesystem::create_directories(path);return path;
}
inline std::filesystem::path Cache(){SYSTEMTIME today{};GetLocalTime(&today);return CacheChild(CacheRoot(),DateName(today).c_str());}
inline std::filesystem::path Unique(const wchar_t* kind,const wchar_t* extension) {
    auto dir=CacheChild(Cache(),kind);GUID id{};
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
// Only managed cache directories are traversed. Never follow junctions or symlinks.
namespace detail {
inline bool Referenced(const std::filesystem::path& path,const std::set<std::wstring>& referenced) {
    const auto name=path.lexically_normal().wstring();
    for(const auto& item:referenced) {
        if(_wcsicmp(item.c_str(),name.c_str())==0)return true;
        // A clipboard directory reference protects its descendants too.
        if(name.size()>item.size()&&_wcsnicmp(name.c_str(),item.c_str(),item.size())==0&&name[item.size()]==L'\\')return true;
    }return false;
}
inline bool ExpiredFile(HANDLE file,ULONGLONG today) {
    FILETIME modified{},local{};SYSTEMTIME date{};ULONGLONG day{};
    return GetFileTime(file,nullptr,nullptr,&modified)&&FileTimeToLocalFileTime(&modified,&local)&&
        FileTimeToSystemTime(&local,&date)&&CalendarDay(date,day)&&today>day&&today-day>7;
}
inline void CleanDirectory(const std::filesystem::path& directory,const std::set<std::wstring>& referenced,ULONGLONG today,bool legacy) {
    if(Reparse(directory)||Referenced(directory,referenced))return;
    std::error_code error;
    if(directory.filename()==L"SevenZip") {
        // An archive process can use several files without holding each one open.
        for(const auto& entry:std::filesystem::directory_iterator(directory,error)) {
            if(entry.path().filename().wstring().find(L".active-")!=0)continue;
            HANDLE lease=CreateFileW(entry.path().c_str(),DELETE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(lease==INVALID_HANDLE_VALUE)return;CloseHandle(lease);
        }
        if(error)return;
    }
    for(std::filesystem::directory_iterator it(directory,error),end;it!=end;it.increment(error)) {
        if(error)return;const auto path=it->path();
        if(Reparse(path)||Referenced(path,referenced))continue;
        if(it->is_directory(error)){CleanDirectory(path,referenced,today,legacy);continue;}
        if(!it->is_regular_file(error))continue;
        HANDLE file=CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file==INVALID_HANDLE_VALUE)continue;
        if(!legacy||ExpiredFile(file,today)){
            FILE_DISPOSITION_INFO dispose{TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&dispose,sizeof(dispose));
        }CloseHandle(file);
    }
    // Empty only: protected files, failed deletions and reparse points retain the folder.
    RemoveDirectoryW(directory.c_str());
}
inline void CleanRoot(const std::filesystem::path& root,const std::set<std::wstring>& referenced,ULONGLONG today) {
    if(Reparse(root)||!std::filesystem::exists(root))return;
    std::error_code error;
    for(std::filesystem::directory_iterator it(root,error),end;it!=end;it.increment(error)){
        if(error)return;const auto path=it->path();
        if(Reparse(path)||!it->is_directory(error))continue;
        const auto name=path.filename().wstring();ULONGLONG day{};
        if(ParseDate(name,day)) {
            if(today>day&&today-day>7)CleanDirectory(path,referenced,today,false);
        } else if(name==L"Screenshots"||name==L"Recordings"||name==L"Gif"||name==L"SevenZip") {
            CleanDirectory(path,referenced,today,true);
        }
    }
}
}
inline void CleanCache(const SYSTEMTIME& localToday) noexcept {
    try {
        ULONGLONG today{};if(!CalendarDay(localToday,today))return;
        auto root=Root()/L"Cache";if(!std::filesystem::exists(root)||Reparse(root))return;
        if(!OpenClipboard(nullptr))return;
        std::set<std::wstring> referenced;
        auto files=static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if(!files&&IsClipboardFormatAvailable(CF_HDROP)){CloseClipboard();return;}
        if(files){const auto count=DragQueryFileW(files,0xffffffff,nullptr,0);for(UINT i=0;i<count;++i){wchar_t p[32768]{};DragQueryFileW(files,i,p,32768);referenced.insert(std::filesystem::path(p).lexically_normal().wstring());}}
        CloseClipboard();
        detail::CleanRoot(root,referenced,today);
        // Preserve old clipboard paths until expiry; no new SID directory is created.
        const auto legacy=root/UserSid();detail::CleanRoot(legacy,referenced,today);
        if(!Reparse(legacy))RemoveDirectoryW(legacy.c_str());
    }catch(...){}
}
inline void CleanCache() noexcept {SYSTEMTIME today{};GetLocalTime(&today);CleanCache(today);}
inline void Initialize(){auto data=Data();Cache();auto probe=data/L"write-check.tmp";AtomicWrite(probe,"ok");DeleteFileW(probe.c_str());}
}
