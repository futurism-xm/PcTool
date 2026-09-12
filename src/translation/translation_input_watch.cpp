#include "translation/translation_input_watch.h"
#include <windowsx.h>
namespace translation {
bool InputWatch::Start(HWND window){
    if(thread_.joinable())return true;
    window_=window;std::promise<bool> ready;auto future=ready.get_future();
    thread_=std::thread([this,ready=std::move(ready)]()mutable{
        current_=this;threadId_=GetCurrentThreadId();MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
        auto mouse=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
        auto keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,KeyHook,GetModuleHandleW(nullptr),0);
        const bool ok=mouse&&keyboard;ready.set_value(ok);
        if(ok)while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        if(mouse)UnhookWindowsHookEx(mouse);if(keyboard)UnhookWindowsHookEx(keyboard);current_=nullptr;
    });
    if(future.get())return true;thread_.join();threadId_=0;return false;
}
void InputWatch::Stop(){if(thread_.joinable()){PostThreadMessageW(threadId_,WM_QUIT,0,0);thread_.join();}threadId_=0;window_=nullptr;}
LRESULT CALLBACK InputWatch::MouseHook(int code,WPARAM wp,LPARAM lp){
    auto* self=current_;
    if(code==HC_ACTION&&self&&(wp==WM_LBUTTONDOWN||wp==WM_RBUTTONDOWN||wp==WM_MBUTTONDOWN||wp==WM_XBUTTONDOWN)){
        const auto* event=reinterpret_cast<MSLLHOOKSTRUCT*>(lp);++self->serial_;
        if(auto request=self->copyRequest_.load())PostMessageW(self->window_,CancelCopy,WPARAM(request),0);
        PostMessageW(self->window_,Mouse,WPARAM(self->invocation_.load()),MAKELPARAM(event->pt.x,event->pt.y));
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
LRESULT CALLBACK InputWatch::KeyHook(int code,WPARAM wp,LPARAM lp){
    auto* self=current_;
    if(code==HC_ACTION&&self&&(wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN)){
        const auto* key=reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        const bool trigger=self->waitingKeys_&&(key->vkCode==self->triggerKey_.load()||key->vkCode==VK_MENU||key->vkCode==VK_LMENU||key->vkCode==VK_RMENU||key->vkCode==VK_CONTROL||key->vkCode==VK_LCONTROL||key->vkCode==VK_RCONTROL||key->vkCode==VK_SHIFT||key->vkCode==VK_LSHIFT||key->vkCode==VK_RSHIFT);
        if(key->dwExtraInfo!=InjectionTag&&!trigger){++self->serial_;const bool switching=key->vkCode==VK_LWIN||key->vkCode==VK_RWIN||(key->vkCode==VK_TAB&&(GetAsyncKeyState(VK_MENU)&0x8000));
            if(auto request=self->copyRequest_.load())PostMessageW(self->window_,CancelCopy,WPARAM(request),1);
            PostMessageW(self->window_,Key,WPARAM(self->invocation_.load()),MAKELPARAM(key->vkCode,switching?1:0));
            // Escape is handled by the generation-tagged UI message. Do not
            // also deliver it to a control after hiding/refocusing the popup.
            if(key->vkCode==VK_ESCAPE)return 1;
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
}
