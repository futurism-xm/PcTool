#include "shared/platform/app_storage.h"
#include "app/app_settings.h"
#include <iostream>
static void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int main(){
 try{
    app_storage::Initialize();
    auto data=app_storage::Data(),cache=app_storage::Cache();
    Check(data.parent_path().parent_path()==app_storage::Root(),"data escaped installation");
    const auto original=std::filesystem::current_path();std::filesystem::current_path(std::filesystem::temp_directory_path());Check(app_storage::Data()==data,"working directory changed storage root");std::filesystem::current_path(original);
    AppSettings settings;settings.SetTranslationEnabled(true);settings.SetAutoStartEnabled(false);auto keys=hotkeys::Defaults;keys[7]={MOD_CONTROL,VK_F11};settings.SetHotkeys(keys);Check(settings.Save(),"save failed");
    AppSettings restored;restored.Load();Check(restored.TranslationEnabled()&&!restored.AutoStartEnabled()&&restored.Hotkeys()==keys,"settings reload mismatch");
    auto settingsFile=data/L"settings.ini";
    SetFileAttributesW(settingsFile.c_str(),FILE_ATTRIBUTE_READONLY);
    const bool refused=!settings.Save();SetFileAttributesW(settingsFile.c_str(),FILE_ATTRIBUTE_NORMAL);
    Check(refused,"read-only settings silently accepted");restored.Load();Check(restored.Hotkeys()==keys,"failed save damaged original settings");
    auto held=app_storage::Unique(L"Screenshots",L".png"),dead=app_storage::Unique(L"Recordings",L".mp4"),busy=app_storage::Unique(L"Gif",L".gif");
    app_storage::AtomicWrite(held,"fixture");app_storage::AtomicWrite(dead,"fixture");app_storage::AtomicWrite(busy,"fixture");
    HANDLE lease=CreateFileW(busy.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HWND owner=CreateWindowExW(0,L"STATIC",L"Clipboard test",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(OpenClipboard(owner)!=FALSE,"clipboard busy");EmptyClipboard();auto drop=app_storage::FileDrop(held);Check(SetClipboardData(CF_HDROP,drop)!=nullptr,"file clipboard failed");CloseClipboard();
    app_storage::CleanCache();Check(std::filesystem::exists(held)&&std::filesystem::exists(busy)&&!std::filesystem::exists(dead),"cache cleanup lost active data or retained obsolete file");
    Check(OpenClipboard(owner)!=FALSE,"clipboard busy");EmptyClipboard();CloseClipboard();CloseHandle(lease);
    app_storage::CleanCache();Check(!std::filesystem::exists(held)&&!std::filesystem::exists(busy),"expired cache survived");
    auto external=app_storage::Root()/L"external-fixture";std::filesystem::create_directories(external);app_storage::AtomicWrite(external/L"keep.txt","keep");
    auto link=cache/L"junction-fixture";
    if(CreateSymbolicLinkW(link.c_str(),external.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|2)){
        app_storage::CleanCache();Check(std::filesystem::exists(external/L"keep.txt"),"cleanup followed symlink");RemoveDirectoryW(link.c_str());std::cout<<"PASS reparse isolation\n";
    }else std::cout<<"NOT RUN symlink creation unavailable\n";
    DestroyWindow(owner);std::cout<<"PASS local paths, configuration persistence, clipboard reference, active file and obsolete cleanup\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
