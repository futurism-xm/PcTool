#include "shared/ui/confirmation.h"
#include "shared/ui/popup_controls.h"
#include "shared/ui/toolbar_icons.h"
#include "shared/ui/resource.h"
#include "shared/annotation/text_layout.h"
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <algorithm>

namespace shared_ui {
namespace {
constexpr wchar_t ClassName[] = L"PcTool.TranslationConfirmation";
struct Confirmation {
    HWND window{}, owner{};
    UINT dpi{96};
    HFONT font{};
    bool accepted{};
    std::wstring title, message, accept, cancel;
    shared_ui::PopupShadow shadow{L"PcTool.TranslationConfirmationShadow"};
    ~Confirmation(){shadow.Close();if(IsWindow(window))DestroyWindow(window);DeleteObject(font);}
    int D(int value)const{return MulDiv(value,dpi,96);}
    void Font(){
        DeleteObject(font);
        font=CreateFontW(-D(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        for(int id:{IDYES,IDNO,IDCANCEL})SendDlgItemMessageW(window,id,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    }
    void Layout(){
        RECT r{};GetClientRect(window,&r);
        MoveWindow(GetDlgItem(window,IDNO),r.right-D(244),r.bottom-D(54),D(104),D(36),FALSE);
        MoveWindow(GetDlgItem(window,IDYES),r.right-D(128),r.bottom-D(54),D(104),D(36),FALSE);
        MoveWindow(GetDlgItem(window,IDCANCEL),r.right-D(48),D(16),D(30),D(30),FALSE);
        SetWindowRgn(window,CreateRoundRectRgn(0,0,r.right+1,r.bottom+1,D(20),D(20)),FALSE);
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
        shadow.Sync(window,dpi);
    }
    static void Round(Gdiplus::Graphics& g,const RECT& r,float radius,Gdiplus::Color color,bool outline=false){
        Gdiplus::GraphicsPath path;capture::text_layout::AddRoundedRectangle(path,
            {float(r.left),float(r.top),float(r.right-r.left),float(r.bottom-r.top)},radius);
        if(outline){Gdiplus::Pen pen(color,1.0F);g.DrawPath(&pen,&path);}
        else{Gdiplus::SolidBrush brush(color);g.FillPath(&brush,&path);}
    }
    void Paint(HDC target, DRAWITEMSTRUCT* item=nullptr){
        RECT r{};if(item)r=item->rcItem;else GetClientRect(window,&r);
        HDC dc=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,r.right),std::max(1L,r.bottom));auto old=SelectObject(dc,bitmap);
        {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const bool close=item&&item->CtlID==IDCANCEL;
            g.Clear(item&&!close?Gdiplus::Color(255,255,255,255):Gdiplus::Color(255,247,248,250));
            SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
            if(item){
                const bool hot=GetPropW(item->hwndItem,L"ConfirmationHot")!=nullptr;
                const bool down=(item->itemState&ODS_SELECTED)!=0,green=item->CtlID==IDYES;
                RECT button=r;InflateRect(&button,-D(1),-D(1));
                Round(g,button,float(D(6)),green?Gdiplus::Color(255,hot||down?86:103,hot||down?174:194,hot||down?45:58):
                    hot||down?Gdiplus::Color(255,237,239,242):close?Gdiplus::Color(255,247,248,250):Gdiplus::Color(255,255,255,255));
                if(!green&&!close)Round(g,button,float(D(6)),Gdiplus::Color(255,219,223,229),true);
                if(item->itemState&ODS_FOCUS)Round(g,button,float(D(6)),Gdiplus::Color(255,151,164,183),true);
                if(close)capture::DrawToolbarIcon(dc,capture::ToolbarIcon::Cancel,button,dpi,RGB(125,131,140),11);
                else{SetTextColor(dc,green?RGB(255,255,255):RGB(96,98,102));DrawTextW(dc,(green?accept:cancel).c_str(),-1,&button,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
            }else{
                Gdiplus::SolidBrush white(Gdiplus::Color(255,255,255,255));g.FillRectangle(&white,0,r.bottom-D(72),r.right,D(72));
                Gdiplus::Pen line(Gdiplus::Color(255,232,234,237));g.DrawLine(&line,0,r.bottom-D(72),r.right,r.bottom-D(72));
                Gdiplus::FontFamily family(L"Microsoft YaHei UI");Gdiplus::Font heading(&family,float(D(18)),Gdiplus::FontStyleBold,Gdiplus::UnitPixel);
                Gdiplus::SolidBrush ink(Gdiplus::Color(255,48,51,56));g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                g.DrawString(title.c_str(),-1,&heading,Gdiplus::RectF(float(D(24)),float(D(22)),float(r.right-D(80)),float(D(34))),nullptr,&ink);
                RECT body{D(24),D(76),r.right-D(24),r.bottom-D(92)};SetTextColor(dc,RGB(96,98,102));DrawTextW(dc,message.c_str(),-1,&body,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
            }
        }
        BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    static LRESULT CALLBACK Button(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
        if(m==WM_MOUSEMOVE&&!GetPropW(w,L"ConfirmationHot")){SetPropW(w,L"ConfirmationHot",HANDLE(1));TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);InvalidateRect(w,nullptr,FALSE);}
        if(m==WM_MOUSELEAVE){RemovePropW(w,L"ConfirmationHot");InvalidateRect(w,nullptr,FALSE);}
        if(m==WM_NCDESTROY){RemovePropW(w,L"ConfirmationHot");RemoveWindowSubclass(w,Button,1);}
        return DefSubclassProc(w,m,wp,lp);
    }
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Confirmation*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<Confirmation*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(dc);EndPaint(w,&ps);return 0;}
        case WM_DRAWITEM:{auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);self->Paint(item->hDC,item);return TRUE;}
        case WM_COMMAND:if(LOWORD(wp)==IDYES||LOWORD(wp)==IDNO||LOWORD(wp)==IDCANCEL){self->accepted=LOWORD(wp)==IDYES;DestroyWindow(w);return 0;}break;
        case WM_CLOSE:self->accepted=false;DestroyWindow(w);return 0;
        case WM_NCHITTEST:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&p);RECT r{};GetClientRect(w,&r);return p.y<self->D(58)&&p.x<r.right-self->D(52)?HTCAPTION:HTCLIENT;}
        case WM_DPICHANGED:{self->dpi=HIWORD(wp);self->Font();RECT r=*reinterpret_cast<RECT*>(lp);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&mi);int width=std::min(int(r.right-r.left),int(mi.rcWork.right-mi.rcWork.left)-self->D(28)),height=std::min(int(r.bottom-r.top),int(mi.rcWork.bottom-mi.rcWork.top)-self->D(28));r.left=std::clamp(int(r.left),int(mi.rcWork.left)+self->D(14),int(mi.rcWork.right)-self->D(14)-width);r.top=std::clamp(int(r.top),int(mi.rcWork.top)+self->D(14),int(mi.rcWork.bottom)-self->D(14)-height);SetWindowPos(w,nullptr,r.left,r.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();return 0;}
        case WM_WINDOWPOSCHANGED:self->shadow.Sync(w,self->dpi);break;
        case WM_DESTROY:self->shadow.Close();return 0;
        }
        return DefWindowProcW(w,m,wp,lp);
    }
};
}
bool HandleSourceConfirmationMessage(MSG& message){
    HWND root=GetAncestor(message.hwnd,GA_ROOT);wchar_t name[96]{};GetClassNameW(root,name,96);
    if(wcscmp(name,ClassName)!=0)return false;
    if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){SendMessageW(root,WM_CLOSE,0,0);return true;}
    if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN){
        HWND focus=GetFocus();const int id=IsChild(root,focus)?GetDlgCtrlID(focus):IDNO;
        SendMessageW(root,WM_COMMAND,id==IDYES?IDYES:id==IDCANCEL?IDCANCEL:IDNO,0);return true;
    }
    return IsDialogMessageW(root,&message)!=FALSE;
}
bool ConfirmSourceAction(HWND owner,const std::wstring& title,const std::wstring& message,const std::wstring& accept,const std::wstring& cancel){
    Confirmation view;view.owner=owner;view.title=title;view.message=message;view.accept=accept;view.cancel=cancel;view.dpi=GetDpiForWindow(owner);
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=Confirmation::Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=ClassName;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
    RECT parent{};GetWindowRect(owner,&parent);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&mi);
    const int width=std::min(view.D(460),int(mi.rcWork.right-mi.rcWork.left)-view.D(28)),height=std::min(view.D(240),int(mi.rcWork.bottom-mi.rcWork.top)-view.D(28));
    int x=std::clamp(int(parent.left+(parent.right-parent.left-width)/2),int(mi.rcWork.left)+view.D(14),int(mi.rcWork.right)-view.D(14)-width);
    int y=std::clamp(int(parent.top+(parent.bottom-parent.top-height)/2),int(mi.rcWork.top)+view.D(14),int(mi.rcWork.bottom)-view.D(14)-height);
    if(!CreateWindowExW(0,ClassName,title.c_str(),WS_POPUP|WS_CLIPCHILDREN,x,y,width,height,owner,nullptr,wc.hInstance,&view))return false;
    for(int id:{IDNO,IDYES,IDCANCEL}){auto button=CreateWindowExW(0,L"BUTTON",id==IDYES?accept.c_str():id==IDNO?cancel.c_str():L"关闭",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,view.window,HMENU(INT_PTR(id)),wc.hInstance,nullptr);SetWindowSubclass(button,Confirmation::Button,1,0);}
    view.Font();view.Layout();HWND focus=GetFocus();const bool enabled=IsWindowEnabled(owner)!=FALSE;EnableWindow(owner,FALSE);ShowWindow(view.window,SW_SHOW);view.shadow.Sync(view.window,view.dpi);SetForegroundWindow(view.window);SetFocus(GetDlgItem(view.window,IDNO));
    MSG msg{};int result=1;
    while(IsWindow(view.window)&&(result=GetMessageW(&msg,nullptr,0,0))>0){if(!HandleSourceConfirmationMessage(msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(IsWindow(owner)){EnableWindow(owner,enabled);SetForegroundWindow(owner);if(IsWindow(focus))SetFocus(focus);}
    if(result==0)PostQuitMessage(int(msg.wParam));
    return result>0&&view.accepted&&IsWindow(owner);
}
}
