#pragma once
#include <windows.h>
#include <initializer_list>
namespace shared_platform {
struct MenuSource {HWND window{};DWORD process{};};
class MenuSourceTracker {
public:
    ~MenuSourceTracker(){if(hook_)UnhookWinEvent(hook_);if(active_==this)active_=nullptr;}
    void Start(){active_=this;Observe(GetForegroundWindow());hook_=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,Changed,0,0,WINEVENT_OUTOFCONTEXT);}
    MenuSource Freeze(){Observe(GetForegroundWindow());return current_;}
private:
    void Observe(HWND w){
        if(!IsWindow(w))return;wchar_t name[128]{};GetClassNameW(w,name,128);
        for(const auto* ignore:{L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd",L"NotifyIconOverflowWindow",L"TopLevelWindowForOverflowXamlIsland",L"#32768",L"PcTool.BackgroundController"})if(wcscmp(name,ignore)==0)return;
        DWORD pid{};GetWindowThreadProcessId(w,&pid);current_={w,pid};
    }
    static void CALLBACK Changed(HWINEVENTHOOK,DWORD,HWND w,LONG,LONG,DWORD,DWORD){if(active_)active_->Observe(w);}
    inline static MenuSourceTracker* active_{};HWINEVENTHOOK hook_{};MenuSource current_{};
};
}
