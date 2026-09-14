#include "shared/platform/app_storage.h"
#include "app/app_settings.h"
#include <iostream>
#include <thread>
static void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
static void Modified(const std::filesystem::path& path,const SYSTEMTIME& date){
    FILETIME local{},utc{};Check(SystemTimeToFileTime(&date,&local)&&LocalFileTimeToFileTime(&local,&utc),"invalid fixture date");
    HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"cannot timestamp fixture");const bool ok=SetFileTime(file,nullptr,nullptr,&utc)!=FALSE;CloseHandle(file);Check(ok,"timestamp failed");
}
int main(){
 try{
    app_storage::Initialize();
    auto data=app_storage::Data(),cache=app_storage::CacheRoot();
    Check(data==app_storage::Root()/L"Data"&&cache==app_storage::Root()/L"Cache","SID layer remains in storage path");
    const auto legacyData=data/app_storage::UserSid(),legacyCache=cache/app_storage::UserSid();
    Check(!std::filesystem::exists(legacyData)&&!std::filesystem::exists(legacyCache),"startup created SID directory");
    const auto original=std::filesystem::current_path();std::filesystem::current_path(std::filesystem::temp_directory_path());Check(app_storage::Data()==data,"working directory changed storage root");std::filesystem::current_path(original);
    AppSettings settings;settings.SetTranslationEnabled(true);settings.SetAutoStartEnabled(false);auto keys=hotkeys::Defaults;keys[7]={MOD_CONTROL,VK_F11};settings.SetHotkeys(keys);Check(settings.Save(),"save failed");
    AppSettings restored;restored.Load();Check(restored.TranslationEnabled()&&!restored.AutoStartEnabled()&&restored.Hotkeys()==keys,"settings reload mismatch");
    auto settingsFile=data/L"settings.ini";
    SetFileAttributesW(settingsFile.c_str(),FILE_ATTRIBUTE_READONLY);
    const bool refused=!settings.Save();SetFileAttributesW(settingsFile.c_str(),FILE_ATTRIBUTE_NORMAL);
    Check(refused,"read-only settings silently accepted");restored.Load();Check(restored.Hotkeys()==keys,"failed save damaged original settings");
    std::filesystem::create_directories(legacyData);
    Check(MoveFileW(settingsFile.c_str(),(legacyData/L"settings.ini").c_str())!=FALSE,"cannot prepare old settings");
    app_storage::AtomicWrite(legacyData/L"Translation/migration-fixture.bin","encrypted bytes unchanged");
    app_storage::Data();restored.Load();Check(restored.Hotkeys()==keys,"legacy settings migration lost values");
    std::ifstream migrated(data/L"Translation/migration-fixture.bin",std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(migrated)),{});migrated.close();
    Check(bytes=="encrypted bytes unchanged"&&!std::filesystem::exists(legacyData),"legacy data migration failed");
    app_storage::AtomicWrite(legacyData/L"settings.ini","conflict fixture");app_storage::Data();restored.Load();
    Check(restored.Hotkeys()==keys&&std::filesystem::exists(legacyData/L"settings.ini"),"migration overwrote existing settings");
    DeleteFileW((legacyData/L"settings.ini").c_str());RemoveDirectoryW(legacyData.c_str());
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
    const auto oldReference=legacyCache/L"2026-03-01/Screenshots/referenced.png";
    app_storage::AtomicWrite(oldReference,"old clipboard reference");
    Check(OpenClipboard(owner)!=FALSE,"clipboard busy");EmptyClipboard();Check(SetClipboardData(CF_HDROP,app_storage::FileDrop(oldReference))!=nullptr,"old clipboard fixture failed");CloseClipboard();
    app_storage::CleanCache(today);Check(std::filesystem::exists(oldReference),"old SID clipboard path was moved or deleted");
    Check(OpenClipboard(owner)!=FALSE,"clipboard busy");EmptyClipboard();CloseClipboard();
    app_storage::AtomicWrite(legacyCache/L"2026-03-02/keep.txt","seven days");app_storage::CleanCache(today);
    Check(!std::filesystem::exists(oldReference)&&std::filesystem::exists(legacyCache/L"2026-03-02/keep.txt"),"legacy SID retention boundary failed");
    SYSTEMTIME tomorrow=today;tomorrow.wDay=10;app_storage::CleanCache(tomorrow);
    Check(!std::filesystem::exists(legacyCache),"empty old SID directory survived cleanup");
    auto external=app_storage::Root()/L"external-fixture";std::filesystem::create_directories(external);app_storage::AtomicWrite(external/L"keep.txt","keep");
    std::filesystem::create_directories(expired);auto link=expired/L"junction-fixture";
    if(CreateSymbolicLinkW(link.c_str(),external.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|2)){
        app_storage::CleanCache(today);Check(std::filesystem::exists(external/L"keep.txt"),"cleanup followed symlink");RemoveDirectoryW(link.c_str());std::cout<<"PASS reparse isolation\n";
    }else std::cout<<"NOT RUN symlink creation unavailable\n";
    ULONGLONG leap{},next{};Check(app_storage::ParseDate(L"2024-02-29",leap)&&app_storage::ParseDate(L"2024-03-01",next)&&next-leap==1,"leap calendar failed");
    Check(!app_storage::ParseDate(L"2025-02-29",next),"invalid leap date accepted");
    Check(app_storage::ParseDate(L"2025-12-31",leap)&&app_storage::ParseDate(L"2026-01-01",next)&&next-leap==1,"year boundary failed");
    DestroyWindow(owner);std::cout<<"PASS flat storage, legacy configuration migration, dated cache, 7/8-day boundary, legacy SID clipboard paths, leases and cleanup\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
