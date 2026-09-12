#pragma once
#include "shared/platform/hotkey_binding.h"
#include <functional>
namespace hotkeys {
// Suspend global shortcuts only while a binding field is receiving keys.
void ShowSettings(HWND owner,const Bindings& current,
    const std::function<bool(const Bindings&,std::wstring&)>& save,
    const std::function<void(bool)>& captureChanged = {});
}
