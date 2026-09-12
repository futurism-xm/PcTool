#pragma once
#include "shared/platform/capture_platform.h"
#include <commctrl.h>
#include <windowsx.h>
#include <functional>

namespace capture {
class ToolWindow {
public:
    virtual ~ToolWindow();
    HWND Window() const { return window_; }
    void Focus() { if(window_) { ShowWindow(window_,SW_RESTORE); SetForegroundWindow(window_); } }
protected:
    bool Create(const wchar_t* title,int width,int height,DWORD style=WS_OVERLAPPEDWINDOW,DWORD ex=0);
    HWND Child(const wchar_t* type,const wchar_t* text,DWORD style,int id);
    void Place(int id,int x,int y,int width,int height);
    void Text(int id,const std::wstring& text) { SetWindowTextW(GetDlgItem(window_,id),text.c_str()); }
    int Dip(int value) const { return MulDiv(value,int(dpi_),96); }
    virtual LRESULT Handle(UINT message,WPARAM w,LPARAM l);
    HWND window_{};
    UINT dpi_{96};
private:
    void UpdateIcons();
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    HFONT font_{};
};
std::wstring WindowText(HWND window);
}
