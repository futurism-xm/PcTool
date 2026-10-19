#include "shared/platform/app_storage.h"
#include "app/app_settings.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <cwctype>
static void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
static void Modified(const std::filesystem::path& path,const SYSTEMTIME& date){
    FILETIME local{},utc{};Check(SystemTimeToFileTime(&date,&local)&&LocalFileTimeToFileTime(&local,&utc),"invalid fixture date");
    HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"cannot timestamp fixture");const bool ok=SetFileTime(file,nullptr,nullptr,&utc)!=FALSE;CloseHandle(file);Check(ok,"timestamp failed");
}
// These fixtures avoid the process-wide clipboard and installation registry.
static void CacheReferenceTests(){
    GUID id{};Check(SUCCEEDED(CoCreateGuid(&id)),"cannot allocate fixture identity");
    wchar_t suffix[40]{};StringFromGUID2(id,suffix,40);
    const auto fixture=app_storage::Root()/(std::wstring(L"storage-core-")+suffix);
    Check(CreateDirectoryW(fixture.c_str(),nullptr)!=FALSE,"cannot create storage fixture");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{fixture};
    SYSTEMTIME today{};today.wYear=2026;today.wMonth=3;today.wDay=9;
    ULONGLONG day{};Check(app_storage::CalendarDay(today,day),"invalid cleanup date");
    for(int variant=0;variant<4;++variant){
        const auto root=fixture/std::to_wstring(variant);
        const auto directory=root/L"2026-03-01/Screenshots";
        const auto held=directory/L"keep.png",sibling=root/L"2026-03-01/Screenshots-other/remove.png";
        app_storage::AtomicWrite(held,"clipboard directory reference");
        app_storage::AtomicWrite(sibling,"unreferenced sibling");
        std::wstring reference=directory.wstring();
        if(variant==1)reference+=L"\\";
        if(variant==2)reference=directory.generic_wstring()+L"/./";
        if(variant==3){reference+=L"\\";std::transform(reference.begin(),reference.end(),reference.begin(),[](wchar_t c){return std::towupper(c);});}
        const std::set<std::wstring> references{std::filesystem::path(reference).lexically_normal().wstring()};
        app_storage::detail::CleanRoot(root,references,day);
        Check(std::filesystem::exists(held),"cache cleanup deleted a referenced directory with a trailing separator");
        Check(!std::filesystem::exists(sibling),"directory reference protected a sibling with the same prefix");
    }
    std::cout<<"PASS cache directory references, trailing separators, normalized paths, case and sibling boundaries\n";
}
int main(int argc,char** argv){
 try{
    if(argc==2&&std::string(argv[1])=="--core"){CacheReferenceTests();return 0;}
    app_storage::Initialize();
    auto data=app_storage::Data(),cache=app_storage::CacheRoot();
    Check(data==app_storage::Root()/L"Data"/app_storage::UserSid()&&cache==app_storage::Root()/L"Cache"/app_storage::UserSid(),"storage is not per-user within installation");
    Check(PcToolUserDirectory::Private(data.wstring(),app_storage::UserSid()),"user data ACL is not private");
    const auto original=std::filesystem::current_path();std::filesystem::current_path(std::filesystem::temp_directory_path());Check(app_storage::Data()==data,"working directory changed storage root");std::filesystem::current_path(original);
    AppSettings settings;settings.SetTranslationEnabled(true);settings.SetAutoStartEnabled(false);auto keys=hotkeys::Defaults;keys[7]={MOD_CONTROL,VK_F11};settings.SetHotkeys(keys);Check(settings.Save(),"save failed");
    AppSettings restored;restored.Load();Check(restored.TranslationEnabled()&&!restored.AutoStartEnabled()&&restored.Hotkeys()==keys,"settings reload mismatch");
    Check(restored.IsItemVisible(MonitorItem::GpuUsage),"new settings missing GPU default");
    Check(restored.ToggleItem(MonitorItem::GpuUsage)&&restored.Save(),"GPU disable save failed");
    AppSettings gpuDisabled;gpuDisabled.Load();Check(!gpuDisabled.IsItemVisible(MonitorItem::GpuUsage),"GPU disabled state lost");
    Check(gpuDisabled.ToggleItem(MonitorItem::GpuUsage)&&gpuDisabled.Save(),"GPU enable save failed");
    AppSettings gpuEnabled;gpuEnabled.Load();Check(gpuEnabled.IsItemVisible(MonitorItem::GpuUsage),"GPU enabled state lost");
    auto settingsFile=data/L"settings.ini";
    SetFileAttributesW(settingsFile.c_str(),FILE_ATTRIBUTE_READONLY);
    const bool refused=!settings.Save();SetFileAttributesW(settingsFile.c_str(),FILE_ATTRIBUTE_NORMAL);
    Check(refused,"read-only settings silently accepted");restored.Load();Check(restored.Hotkeys()==keys,"failed save damaged original settings");
    SYSTEMTIME realToday{};GetLocalTime(&realToday);
    for(const auto* kind:{L"Screenshots",L"Recordings",L"Gif",L"SevenZip"}){
        const auto one=app_storage::Unique(kind,L".tmp"),two=app_storage::Unique(kind,L".tmp");
        Check(one!=two&&one.parent_path()==cache/app_storage::DateName(realToday)/kind,"unique cache is not grouped by local date/type");
    }
    SYSTEMTIME today{};today.wYear=2026;today.wMonth=3;today.wDay=9;
    const auto expired=cache/L"2026-03-01";
    auto held=expired/L"Screenshots/held.png",dead=expired/L"Recordings/dead.mp4",busy=expired/L"Gif/busy.gif";
    app_storage::AtomicWrite(held,"fixture");app_storage::AtomicWrite(dead,"fixture");app_storage::AtomicWrite(busy,"fixture");
    for(const auto* date:{L"2026-03-02",L"2026-03-03",L"2026-03-09",L"2026-03-10",L"2026-02-30",L"not-a-date"})app_storage::AtomicWrite(cache/date/L"keep.txt","keep");
    app_storage::AtomicWrite(cache/L"2026-02-28/old.txt","old");
    const auto legacyOld=cache/L"Screenshots/old.png",legacyNew=cache/L"Screenshots/recent.png";
    app_storage::AtomicWrite(legacyOld,"old");app_storage::AtomicWrite(legacyNew,"new");
    SYSTEMTIME modified=today;modified.wDay=1;Modified(legacyOld,modified);modified.wDay=2;Modified(legacyNew,modified);
    const auto archive=expired/L"SevenZip";app_storage::AtomicWrite(archive/L".active-test","lease");app_storage::AtomicWrite(archive/L"task.tmp","active task");
    HANDLE archiveLease=CreateFileW((archive/L".active-test").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(archiveLease!=INVALID_HANDLE_VALUE,"archive lease failed");
    HANDLE lease=CreateFileW(busy.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(lease!=INVALID_HANDLE_VALUE,"file lease failed");
    HWND owner=CreateWindowExW(0,L"STATIC",L"Clipboard test",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(OpenClipboard(owner)!=FALSE,"clipboard busy");EmptyClipboard();auto drop=app_storage::FileDrop(held);Check(SetClipboardData(CF_HDROP,drop)!=nullptr,"file clipboard failed");CloseClipboard();
    app_storage::CleanCache(today);Check(std::filesystem::exists(held)&&std::filesystem::exists(busy)&&!std::filesystem::exists(dead),"cache cleanup lost active data or retained obsolete file");
    Check(std::filesystem::exists(archive/L"task.tmp"),"active archive directory removed");
    for(const auto* date:{L"2026-03-02",L"2026-03-03",L"2026-03-09",L"2026-03-10",L"2026-02-30",L"not-a-date"})Check(std::filesystem::exists(cache/date/L"keep.txt"),"recent/future/unknown directory removed");
    Check(!std::filesystem::exists(cache/L"2026-02-28")&&!std::filesystem::exists(legacyOld)&&std::filesystem::exists(legacyNew),"date boundary or legacy retention failed");
    Check(OpenClipboard(owner)!=FALSE,"clipboard busy");EmptyClipboard();CloseClipboard();CloseHandle(lease);
    CloseHandle(archiveLease);
    app_storage::CleanCache(today);Check(!std::filesystem::exists(expired),"expired folder survived after leases released");
    // If another thread owns the clipboard, cleanup must conservatively retain files.
    app_storage::AtomicWrite(dead,"clipboard inaccessible");
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);bool opened=false;
    std::thread locker([&]{
        HWND lockWindow=CreateWindowExW(0,L"STATIC",L"Clipboard lock fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        opened=OpenClipboard(lockWindow)!=FALSE;SetEvent(ready);WaitForSingleObject(release,10000);if(opened)CloseClipboard();DestroyWindow(lockWindow);
    });
    WaitForSingleObject(ready,10000);app_storage::CleanCache(today);const bool kept=std::filesystem::exists(dead);
    SetEvent(release);locker.join();CloseHandle(ready);CloseHandle(release);Check(opened&&kept,"busy clipboard did not protect cache");
    app_storage::CleanCache(today);Check(!std::filesystem::exists(expired),"retry did not remove expired folder");
    const auto otherUser=app_storage::Root()/L"Cache/S-1-5-21-999-999-999-999/2026-03-01/keep.txt";
    app_storage::AtomicWrite(otherUser,"other user");app_storage::CleanCache(today);
    Check(std::filesystem::exists(otherUser),"cleanup crossed user boundary");
    auto external=app_storage::Root()/L"external-fixture";std::filesystem::create_directories(external);app_storage::AtomicWrite(external/L"keep.txt","keep");
    std::filesystem::create_directories(expired);auto link=expired/L"junction-fixture";
    if(CreateSymbolicLinkW(link.c_str(),external.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|2)){
        app_storage::CleanCache(today);Check(std::filesystem::exists(external/L"keep.txt"),"cleanup followed symlink");RemoveDirectoryW(link.c_str());std::cout<<"PASS reparse isolation\n";
    }else std::cout<<"NOT RUN symlink creation unavailable\n";
    ULONGLONG leap{},next{};Check(app_storage::ParseDate(L"2024-02-29",leap)&&app_storage::ParseDate(L"2024-03-01",next)&&next-leap==1,"leap calendar failed");
    Check(!app_storage::ParseDate(L"2025-02-29",next),"invalid leap date accepted");
    Check(app_storage::ParseDate(L"2025-12-31",leap)&&app_storage::ParseDate(L"2026-01-01",next)&&next-leap==1,"year boundary failed");
    DestroyWindow(owner);std::cout<<"PASS private per-user installation storage, preferences, dated cache, 7/8-day boundary, user isolation, leases and cleanup\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
