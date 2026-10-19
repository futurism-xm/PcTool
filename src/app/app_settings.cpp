#include "shared/platform/app_storage.h"
#include "app/app_settings.h"
#include "app/startup_registration.h"

#include <windows.h>

#include <array>
#include <string>

void AppSettings::Load() {
    const auto path=app_storage::Data()/L"settings.ini";
    if(!std::filesystem::exists(path))return;
    if(GetPrivateProfileIntW(L"PcTool",L"Version",0,path.c_str())!=1)throw std::runtime_error("Unsupported settings version");
    auto read=[&](const wchar_t* name,DWORD fallback){return GetPrivateProfileIntW(L"PcTool",name,fallback,path.c_str());};
    auto value=read(L"VisibleItems",visibleItems_);if(value&kAllMonitorItems)visibleItems_=value&kAllMonitorItems;
    autoStartEnabled_=read(L"AutoStart",autoStartEnabled_)!=0;
    startupSchema_=read(L"StartupSchema",0);
    translationEnabled_=read(L"TranslationEnabled",translationEnabled_)!=0;
    screenshotEnabled_=read(L"ScreenshotEnabled",screenshotEnabled_)!=0;
    monitorInTaskbar_=read(L"MonitorInTaskbar",monitorInTaskbar_)!=0;
    auto bindings=hotkeys::Defaults;
    for(size_t i=0;i<bindings.size();++i){const auto value=read((L"Hotkey"+std::to_wstring(i)).c_str(),MAKELONG(bindings[i].key,bindings[i].modifiers));bindings[i]={HIWORD(value),LOWORD(value)};}
    if(hotkeys::Valid(bindings))hotkeys_=bindings;
}
bool AppSettings::Save() const {
    try {
        std::string text="[PcTool]\r\nVersion=1\r\n";
        auto add=[&](const std::string& name,DWORD value){text+=name+"="+std::to_string(value)+"\r\n";};
        add("VisibleItems",visibleItems_);add("AutoStart",autoStartEnabled_);add("TranslationEnabled",translationEnabled_);add("ScreenshotEnabled",screenshotEnabled_);add("MonitorInTaskbar",monitorInTaskbar_);
        add("StartupSchema",startupSchema_);
        for(size_t i=0;i<hotkeys_.size();++i)add("Hotkey"+std::to_string(i),MAKELONG(hotkeys_[i].key,hotkeys_[i].modifiers));
        app_storage::AtomicWrite(app_storage::Data()/L"settings.ini",text);return true;
    }catch(...){return false;}
}
bool AppSettings::IsItemVisible(MonitorItem item) const noexcept {
    return (visibleItems_ & ToMask(item)) != 0;
}

bool AppSettings::ToggleItem(MonitorItem item) noexcept {
    const std::uint32_t candidate = visibleItems_ ^ ToMask(item);
    if ((candidate & kAllMonitorItems) == 0) {
        return false;
    }
    visibleItems_ = candidate;
    return true;
}

bool AppSettings::ApplyAutoStart(std::wstring* error,const std::wstring& nameSpace) const {
    std::wstring failure;
    const bool ok=startup_registration::Apply(autoStartEnabled_,app_storage::Root()/L"PcTool.exe",failure,nameSpace);
    if(error)*error=std::move(failure);
    return ok;
}

bool AppSettings::InitializeStartup(std::wstring& error,const std::wstring& nameSpace){
    const auto oldSchema=startupSchema_;const bool oldEnabled=autoStartEnabled_;
    if(startupSchema_<2)autoStartEnabled_=true;
    startupSchema_=2;
    // Check durable settings before changing startup registration. A locked or
    // full data directory must not leave a newly enabled Run entry behind.
    if(!Save()){
        autoStartEnabled_=oldEnabled;startupSchema_=oldSchema;
        error=L"无法保存开机启动设置，请检查 Data 目录权限。";return false;
    }
    if(ApplyAutoStart(&error,nameSpace))return true;
    autoStartEnabled_=oldEnabled;startupSchema_=oldSchema;
    if(!Save())error+=L"\n原启动设置恢复失败，请检查 Data 目录权限。";
    autoStartEnabled_=false;return false;
}
