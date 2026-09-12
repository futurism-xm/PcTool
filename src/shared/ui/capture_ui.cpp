#include "shared/ui/capture_ui.h"
#include "shared/ui/resource.h"

namespace capture {
std::wstring WindowText(HWND w) {
    std::wstring text(size_t(GetWindowTextLengthW(w))+1,L'\0');
    const int n=GetWindowTextW(w,text.data(),int(text.size())); text.resize(n); return text;
}
ToolWindow::~ToolWindow() { if(window_) DestroyWindow(window_); if(font_) DeleteObject(font_); }
bool ToolWindow::Create(const wchar_t* title,int width,int height,DWORD style,DWORD ex) {
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=Proc;
    wc.lpszClassName=L"PcTool.CaptureToolWindow"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE); wc.style=CS_DBLCLKS;
    wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm=wc.hIcon;
    RegisterClassExW(&wc);
    dpi_=GetDpiForSystem();
    window_=CreateWindowExW(ex,wc.lpszClassName,title,style,CW_USEDEFAULT,CW_USEDEFAULT,Dip(width),Dip(height),nullptr,nullptr,wc.hInstance,this);
    if(!window_) return false;
    dpi_=GetDpiForWindow(window_);
    UpdateIcons();
    font_=CreateFontW(-Dip(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    return true;
}
void ToolWindow::UpdateIcons() {
    // Resource-backed shared handles remain valid for the window/class lifetime.
    for(bool small:{false,true}) {
        auto icon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_APP_ICON),IMAGE_ICON,
            GetSystemMetricsForDpi(small?SM_CXSMICON:SM_CXICON,dpi_),
            GetSystemMetricsForDpi(small?SM_CYSMICON:SM_CYICON,dpi_),LR_SHARED));
        if(icon) SendMessageW(window_,WM_SETICON,small?ICON_SMALL:ICON_BIG,reinterpret_cast<LPARAM>(icon));
    }
}
HWND ToolWindow::Child(const wchar_t* type,const wchar_t* text,DWORD style,int id) {
    HWND c=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window_,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE); return c;
}
void ToolWindow::Place(int id,int x,int y,int w,int h) { MoveWindow(GetDlgItem(window_,id),Dip(x),Dip(y),Dip(w),Dip(h),TRUE); }
LRESULT ToolWindow::Handle(UINT m,WPARAM w,LPARAM l) {
    if(m==WM_CLOSE) { DestroyWindow(window_); return 0; }
    if(m==WM_DPICHANGED) {
        dpi_=HIWORD(w); const auto* r=reinterpret_cast<RECT*>(l);
        UpdateIcons();
        HFONT previous=font_;
        font_=CreateFontW(-Dip(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        for(HWND child=GetWindow(window_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        if(previous) DeleteObject(previous);
        SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        return 0;
    }
    return DefWindowProcW(window_,m,w,l);
}
LRESULT CALLBACK ToolWindow::Proc(HWND window,UINT m,WPARAM w,LPARAM l) {
    auto* self=reinterpret_cast<ToolWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(m==WM_NCCREATE) { self=static_cast<ToolWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); self->window_=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
    if(!self) return DefWindowProcW(window,m,w,l);
    LRESULT result=0;
    try { result=self->Handle(m,w,l); }
    catch(...) { MessageBoxW(window,CurrentError(L"操作失败").c_str(),L"PcTool",MB_OK|MB_ICONERROR); }
    if(m==WM_NCDESTROY) { SetWindowLongPtrW(window,GWLP_USERDATA,0); self->window_=nullptr; }
    return result;
}
}
