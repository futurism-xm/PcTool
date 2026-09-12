#include "shared/ui/popup_controls.h"
#include <cmath>
#include <cstdint>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
namespace shared_ui {
HWND SlimScrollbar::Create(HWND parent,int id,std::function<void(int)> changed,COLORREF background){
    changed_=std::move(changed);background_=background;
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"PcTool.SlimScrollbar";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    window_=CreateWindowExW(0,wc.lpszClassName,L"",WS_CHILD,0,0,1,1,parent,reinterpret_cast<HMENU>(INT_PTR(id)),wc.hInstance,this);return window_;
}
ScrollGeometry SlimScrollbar::Geometry()const{RECT r{};GetClientRect(window_,&r);return {int(r.bottom),total_,page_,position_,MulDiv(18,dpi_,96)};}
void SlimScrollbar::Update(int total,int page,int position,UINT dpi){total_=std::max(0,total);page_=std::max(1,page);position_=std::clamp(position,0,Maximum());dpi_=dpi;ShowWindow(window_,Maximum()?SW_SHOWNA:SW_HIDE);InvalidateRect(window_,nullptr,FALSE);}
void SlimScrollbar::Move(int position){position=std::clamp(position,0,Maximum());if(position==position_)return;position_=position;InvalidateRect(window_,nullptr,FALSE);if(changed_)changed_(position);}
LRESULT CALLBACK SlimScrollbar::Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    auto* self=reinterpret_cast<SlimScrollbar*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE){self=static_cast<SlimScrollbar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window_=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(w,m,wp,lp);
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom));auto old=SelectObject(buffer,bitmap);HBRUSH brush=CreateSolidBrush(self->background_);FillRect(buffer,&r,brush);DeleteObject(brush);
        auto shape=self->Geometry();{Gdiplus::Graphics g(buffer);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::SolidBrush ink(Gdiplus::Color(255,195,195,195));
        const float width=float(r.right),y=float(shape.top),height=float(shape.length);if(width>0&&height>=width){g.FillRectangle(&ink,0.0f,y+width/2,width,height-width);g.FillEllipse(&ink,0.0f,y,width,width);g.FillEllipse(&ink,0.0f,y+height-width,width,width);}}BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);EndPaint(w,&ps);return 0;}
    if(m==WM_LBUTTONDOWN){auto shape=self->Geometry();int y=GET_Y_LPARAM(lp);if(y>=shape.top&&y<shape.top+shape.length){self->dragging_=true;self->grab_=y-shape.top;SetCapture(w);}else self->Move(self->position_+(y<shape.top?-self->page_:self->page_));return 0;}
    if(m==WM_MOUSEMOVE&&self->dragging_){self->Move(self->Geometry().Position(GET_Y_LPARAM(lp)-self->grab_));return 0;}
    if(m==WM_LBUTTONUP||m==WM_CANCELMODE||m==WM_CAPTURECHANGED){self->dragging_=false;if(GetCapture()==w)ReleaseCapture();return 0;}
    if(m==WM_MOUSEWHEEL)return SendMessageW(GetParent(w),m,wp,lp);
    // Standard range queries keep diagnostics and accessibility clients useful.
    if(m==SBM_GETPOS)return self->position_;
    if(m==SBM_GETSCROLLINFO){auto* i=reinterpret_cast<SCROLLINFO*>(lp);if(!i)return FALSE;i->nMin=0;i->nMax=std::max(0,self->total_-1);i->nPage=self->page_;i->nPos=i->nTrackPos=self->position_;return TRUE;}
    if(m==WM_NCDESTROY)self->window_=nullptr;
    return DefWindowProcW(w,m,wp,lp);
}
PopupShadow::~PopupShadow(){Close();}
void PopupShadow::Close(){if(window_)DestroyWindow(window_);window_=nullptr;width_=height_=margin_=0;}
void PopupShadow::Hide(){if(window_)ShowWindow(window_,SW_HIDE);}
void PopupShadow::Sync(HWND owner,UINT dpi){
    if(!IsWindowVisible(owner)||IsIconic(owner)){Hide();return;}
    if(!window_){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=className_;
        wc.lpfnWndProc=[](HWND w,UINT m,WPARAM wp,LPARAM lp)->LRESULT{if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(w,m,wp,lp);};
        RegisterClassW(&wc);
        window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,0,0,0,0,owner,nullptr,wc.hInstance,nullptr);
    }
    RECT r{};GetWindowRect(owner,&r);const int margin=MulDiv(14,dpi,96),width=r.right-r.left+2*margin,height=r.bottom-r.top+2*margin;
    if(width!=width_||height!=height_||margin!=margin_){
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels{};HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);if(!bitmap){DeleteDC(dc);return;}
        auto old=SelectObject(dc,bitmap);auto* data=static_cast<uint32_t*>(pixels);
        const double radius=double(MulDiv(8,dpi,96)),sigma=double(margin)/2.4;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            const double qx=std::abs(x+0.5-width/2.0)-(width/2.0-margin-radius);
            const double qy=std::abs(y+0.5-height/2.0)-(height/2.0-margin-radius);
            const double distance=std::hypot(std::max(qx,0.0),std::max(qy,0.0))+std::min(std::max(qx,qy),0.0)-radius;
            // Fully transparent interior also protects the content if Windows reorders an owned window.
            const auto alpha=distance>=0?uint32_t(40.0*std::exp(-distance*distance/(2*sigma*sigma))):0u;
            data[y*width+x]=alpha<<24;
        }
        POINT target{r.left-margin,r.top-margin},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        UpdateLayeredWindow(window_,nullptr,&target,&size,dc,&source,0,&blend,ULW_ALPHA);
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);width_=width;height_=height;margin_=margin;
    }
    SetWindowPos(window_,owner,r.left-margin,r.top-margin,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
}
