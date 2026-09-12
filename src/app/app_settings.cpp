#include "shared/platform/app_storage.h"
#include "app/app_settings.h"

#include <windows.h>

#include <array>
#include <string>

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"PcTool";

std::wstring GetExecutablePath() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    return std::wstring(path.data(), length);
}

std::wstring GetAutoStartCommand() {
    const std::wstring path = GetExecutablePath();
    if (path.empty()) {
        return {};
    }
    return L"\"" + path + L"\" --background";
}

}  // namespace

void AppSettings::Load() {
    const auto path=app_storage::Data()/L"settings.ini";
    if(!std::filesystem::exists(path))return;
    if(GetPrivateProfileIntW(L"PcTool",L"Version",0,path.c_str())!=1)throw std::runtime_error("Unsupported settings version");
    auto read=[&](const wchar_t* name,DWORD fallback){return GetPrivateProfileIntW(L"PcTool",name,fallback,path.c_str());};
    auto value=read(L"VisibleItems",visibleItems_);if(value&kAllMonitorItems)visibleItems_=value&kAllMonitorItems;
    autoStartEnabled_=read(L"AutoStart",autoStartEnabled_)!=0;
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

bool AppSettings::ApplyAutoStart() const {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
            nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status = ERROR_SUCCESS;
    if (autoStartEnabled_) {
        const std::wstring command = GetAutoStartCommand();
        if (command.empty()) {
            RegCloseKey(key);
            return false;
        }
        status = RegSetValueExW(
            key, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValueName);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
