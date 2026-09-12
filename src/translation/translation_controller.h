#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <vector>
#include "translation/translation_provider.h"
#include "shared/platform/hotkey_binding.h"
#include "shared/platform/menu_source.h"
namespace translation {
constexpr int ManualHotkey=510, CaptureHotkey=511, SelectionHotkey=512;
enum class Entry { Input,Capture,Selection };
class TranslationController {
public:
    TranslationController();
    ~TranslationController();
    void Configure(HWND owner,bool enabled,std::function<bool()> canCapture,bool showErrors=true);
    void Configure(HWND owner,bool enabled,std::function<bool()> canCapture,bool showErrors,const hotkeys::Bindings& bindings);
    bool HandleHotkey(WPARAM id);
    void Invoke(Entry entry,bool fromMenu=false,shared_platform::MenuSource source={});
    bool FocusCapture();
    void Shutdown();
    void ShowSourceSettings(HWND owner);
    // Provider injection is also used by native integration fixtures, without credentials.
    void SetProviders(std::vector<std::shared_ptr<TranslationProvider>> providers);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
