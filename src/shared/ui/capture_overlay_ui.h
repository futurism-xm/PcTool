#pragma once
#include "shared/ui/capture_ui.h"
#include "shared/ui/toolbar_icons.h"
#include "shared/annotation/annotation_style.h"
#include <stdexcept>
#include <objidl.h>
#include <gdiplus.h>

namespace capture {
// Capture-excluded, click-through surround shared by scrolling capture and editing.
class CaptureSurround {
public:
    ~CaptureSurround(){Close();}
    void Close(){for(HWND& w:windows_) {if(w&&IsWindow(w))DestroyWindow(w);w=nullptr;}}
    void Show(HWND owner,RECT region,RECT monitor,UINT dpi,const RECT* preview=nullptr) {
        if(!windows_[0]) {
            for(HWND& w:windows_) {
                w=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,
                    L"STATIC",L"PcTool.CaptureSurround",WS_POPUP|SS_BLACKRECT,0,0,1,1,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
                if(!w){const auto error=GetLastError();Close();throw std::runtime_error("Cannot create capture surround: "+std::to_string(error));}
                SetLayeredWindowAttributes(w,0,155,LWA_ALPHA);
                if(!SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE)){const auto error=GetLastError();Close();throw std::runtime_error("Cannot exclude capture surround: "+std::to_string(error));}
            }
        }
        SetWindowSubclass(windows_[0],BorderProc,1,RGB(0,0,0));
        const int pad=std::max(1,MulDiv(1,dpi,96));
        RECT outer=region;InflateRect(&outer,pad,pad);
        HRGN mask=CreateRectRgn(0,0,monitor.right-monitor.left,monitor.bottom-monitor.top);
        HRGN hole=CreateRectRgn(outer.left-monitor.left,outer.top-monitor.top,outer.right-monitor.left,outer.bottom-monitor.top);
        CombineRgn(mask,mask,hole,RGN_DIFF);DeleteObject(hole);
        if(preview){HRGN extra=CreateRectRgn(preview->left-monitor.left,preview->top-monitor.top,preview->right-monitor.left,preview->bottom-monitor.top);CombineRgn(mask,mask,extra,RGN_DIFF);DeleteObject(extra);}
        if(!SetWindowRgn(windows_[0],mask,FALSE))DeleteObject(mask);
        SetWindowPos(windows_[0],HWND_TOPMOST,monitor.left,monitor.top,monitor.right-monitor.left,monitor.bottom-monitor.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        RECT edges[]={{outer.left,outer.top,outer.right,region.top},{outer.left,region.bottom,outer.right,outer.bottom},
            {outer.left,region.top,region.left,region.bottom},{region.right,region.top,outer.right,region.bottom}};
        for(int i=0;i<4;++i){SetWindowSubclass(windows_[i+1],BorderProc,1,RGB(40,190,220));SetLayeredWindowAttributes(windows_[i+1],0,255,LWA_ALPHA);auto r=edges[i];SetWindowPos(windows_[i+1],HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);}
    }
private:
    static LRESULT CALLBACK BorderProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR color){
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);HBRUSH brush=CreateSolidBrush(COLORREF(color));FillRect(dc,&r,brush);DeleteObject(brush);EndPaint(w,&ps);return 0;}
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,BorderProc,1);
        return DefSubclassProc(w,m,wp,lp);
    }
    HWND windows_[5]{};
};
struct OverlayButton {int id;ToolbarIcon icon;std::wstring label,tip;int width{36};bool enabled{true},selected{};};
class OverlayToolbar final:public ToolWindow {
public:
    OverlayToolbar(HWND owner,const wchar_t* title,std::vector<OverlayButton> buttons,std::function<void(int)> command)
        :buttons_(std::move(buttons)),command_(std::move(command)) {
        if(!Create(title,400,36,WS_POPUP,WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE))throw std::runtime_error("Cannot create overlay toolbar");
        SetWindowLongPtrW(window_,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(owner));
        if(!SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE))throw std::runtime_error("Cannot exclude overlay toolbar");
        tooltip_=CreateToolbarTooltipWindow(window_,dpi_,true);
    }
    ~OverlayToolbar(){if(tooltip_)DestroyWindow(tooltip_);if(window_)DestroyWindow(window_);}
    void State(int id,bool enabled,bool selected=false){for(auto& b:buttons_)if(b.id==id){if(b.enabled!=enabled||b.selected!=selected){b.enabled=enabled;b.selected=selected;InvalidateRect(window_,nullptr,FALSE);}break;}}
    RECT Bounds()const{RECT r{};GetWindowRect(window_,&r);return r;}
    void PlaceNear(RECT region,RECT work,UINT dpi) {
        dpi_=dpi;rects_.clear();int x=0,y=0,width=0;
        for(auto& b:buttons_) {
            const int w=Dip(b.width);if(x+w>work.right-work.left&&x>0){x=0;y+=Dip(36);}
            rects_.push_back({x,y,x+w,y+Dip(36)});x+=w;width=std::max(width,x);
        }
        int left=std::clamp<int>(region.right-width,work.left,std::max(work.left,work.right-width));
        const int height=y+Dip(36);int top=region.bottom+Dip(4);
        if(top+height>work.bottom)top=region.top-height-Dip(4);
        top=std::clamp<int>(top,work.top,std::max(work.top,work.bottom-height));
        SetWindowPos(window_,HWND_TOPMOST,left,top,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        for(size_t i=0;i<buttons_.size();++i){TOOLINFOW t{sizeof(t)};t.hwnd=window_;t.uId=buttons_[i].id;SendMessageW(tooltip_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&t));t.uFlags=TTF_SUBCLASS;t.rect=rects_[i];t.lpszText=buttons_[i].tip.data();SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&t));}
        InvalidateRect(window_,nullptr,FALSE);
    }
private:
    int Hit(POINT p)const{for(size_t i=0;i<rects_.size();++i)if(PtInRect(&rects_[i],p))return int(i);return -1;}
    void Paint(HDC target){RECT r{};GetClientRect(window_,&r);HDC dc=CreateCompatibleDC(target);HBITMAP bmp=CreateCompatibleBitmap(target,r.right,r.bottom);auto old=SelectObject(dc,bmp);HBRUSH bg=CreateSolidBrush(ToolbarBackground);FillRect(dc,&r,bg);DeleteObject(bg);HBRUSH border=CreateSolidBrush(ToolbarBorder);FrameRect(dc,&r,border);DeleteObject(border);HFONT font=CreateToolbarFont(dpi_);auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
        for(size_t i=0;i<rects_.size();++i){auto& b=buttons_[i];auto cell=rects_[i];DrawToolbarButtonState(dc,cell,dpi_,b.enabled,b.selected,int(i)==hover_,int(i)==pressed_);auto color=b.enabled?ToolbarInk:ToolbarDisabled;
            if(b.label.empty())DrawToolbarIcon(dc,b.icon,cell,dpi_,color);
            else {if(b.icon!=ToolbarIcon::None){RECT icon=cell;icon.right=icon.left+Dip(30);DrawToolbarIcon(dc,b.icon,icon,dpi_,color);cell.left+=Dip(26);}SetTextColor(dc,color);DrawTextW(dc,b.label.c_str(),-1,&cell,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);}}
        SelectObject(dc,oldFont);DeleteObject(font);BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bmp);DeleteDC(dc);
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l)override{
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(window_,&ps);Paint(dc);EndPaint(window_,&ps);return 0;}
        if(m==WM_PRINTCLIENT){Paint(reinterpret_cast<HDC>(w));return 0;}
        if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(m==WM_MOUSEMOVE){int hit=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)});if(hit!=hover_){hover_=hit;InvalidateRect(window_,nullptr,FALSE);}TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window_,0};TrackMouseEvent(&t);return 0;}
        if(m==WM_MOUSELEAVE){hover_=-1;InvalidateRect(window_,nullptr,FALSE);return 0;}
        if(m==WM_LBUTTONDOWN||m==WM_LBUTTONDBLCLK){pressed_=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)});SetCapture(window_);InvalidateRect(window_,nullptr,FALSE);return 0;}
        if(m==WM_LBUTTONUP){int i=pressed_;pressed_=-1;ReleaseCapture();InvalidateRect(window_,nullptr,FALSE);if(i>=0&&i==Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)})&&buttons_[i].enabled){auto command=command_;const int id=buttons_[i].id;command(id);}return 0;}
        if(m==WM_CAPTURECHANGED){pressed_=-1;return 0;}
        if(m==WM_KEYDOWN&&w==VK_ESCAPE){SendMessageW(GetWindow(window_,GW_OWNER),WM_CLOSE,0,0);return 0;}
        return ToolWindow::Handle(m,w,l);
    }
    struct GraphicsLifetime {
        ULONG_PTR token{};
        GraphicsLifetime(){Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("Cannot start toolbar graphics");}
        ~GraphicsLifetime(){if(token)Gdiplus::GdiplusShutdown(token);}
    } graphics_;
    std::vector<OverlayButton> buttons_;std::vector<RECT> rects_;std::function<void(int)> command_;int hover_{-1},pressed_{-1};HWND tooltip_{};
};
class CaptureLabel final:public ToolWindow {
public:
    CaptureLabel(HWND owner,const wchar_t* name){if(!Create(name,150,24,WS_POPUP,WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_LAYERED))throw std::runtime_error("Cannot create capture label");SetWindowLongPtrW(window_,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(owner));SetLayeredWindowAttributes(window_,0,235,LWA_ALPHA);if(!SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE))throw std::runtime_error("Cannot exclude capture label");}
    void Show(std::wstring text,RECT anchor,RECT work,UINT dpi,bool center){
        if(text.empty()){ShowWindow(window_,SW_HIDE);return;}anchor_=anchor;work_=work;center_=center;dpi_=dpi;text_=std::move(text);
        HDC dc=GetDC(window_);HFONT font=CreateToolbarFont(dpi_);auto old=SelectObject(dc,font);RECT measured{};DrawTextW(dc,text_.c_str(),-1,&measured,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);DeleteObject(font);ReleaseDC(window_,dc);
        int width=std::min<int>(measured.right+Dip(16),work.right-work.left),height=Dip(26);
        int x=center?(anchor.left+anchor.right-width)/2:anchor.left,y=center?(anchor.top+anchor.bottom-height)/2:anchor.top-height-Dip(2);
        x=std::clamp<int>(x,work.left,std::max(work.left,work.right-width));y=std::clamp<int>(y,work.top,std::max(work.top,work.bottom-height));
        HRGN shape=CreateRoundRectRgn(0,0,width+1,height+1,Dip(6),Dip(6));if(!SetWindowRgn(window_,shape,FALSE))DeleteObject(shape);
        SetWindowPos(window_,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);InvalidateRect(window_,nullptr,FALSE);
    }
    void Scale(UINT dpi){if(IsWindowVisible(window_))Show(text_,anchor_,work_,dpi,center_);}
private:
    LRESULT Handle(UINT m,WPARAM w,LPARAM l)override{
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);RECT r{};GetClientRect(window_,&r);HBRUSH brush=CreateSolidBrush(RGB(60,60,60));FillRect(dc,&r,brush);DeleteObject(brush);auto font=CreateToolbarFont(dpi_);auto old=SelectObject(dc,font);SetTextColor(dc,RGB(255,255,255));SetBkMode(dc,TRANSPARENT);DrawTextW(dc,text_.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,old);DeleteObject(font);EndPaint(window_,&ps);return 0;}return ToolWindow::Handle(m,w,l);
    }
    std::wstring text_;RECT anchor_{},work_{};bool center_{};
};

}
