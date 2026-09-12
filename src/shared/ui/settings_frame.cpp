#include "shared/ui/settings_frame.h"
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>
namespace shared_ui {
SettingsFrame::SettingsFrame(){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&graphics_,&input,nullptr);}
SettingsFrame::~SettingsFrame(){if(IsWindow(window_))RemoveWindowSubclass(window_,Proc,771);Cleanup();if(graphics_)Gdiplus::GdiplusShutdown(graphics_);}
void SettingsFrame::Cleanup(){shadow_.Close();if(IsWindow(edge_))DestroyWindow(edge_);edge_=nullptr;window_=nullptr;close_=nullptr;width_=height_=0;}
void SettingsFrame::Attach(HWND window){
    window_=window;dpi_=GetDpiForWindow(window);SetWindowSubclass(window,Proc,771,reinterpret_cast<DWORD_PTR>(this));
    SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~(WS_CAPTION|WS_MINIMIZEBOX|WS_MAXIMIZEBOX))|WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN);
    // Present the parent and its native controls together during interactive sizing.
    SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)|WS_EX_COMPOSITED);
    SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    close_=CreateWindowExW(0,L"BUTTON",L"关闭",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,window,HMENU(INT_PTR(7901)),GetModuleHandleW(nullptr),nullptr);
    SetWindowSubclass(close_,CloseProc,1,reinterpret_cast<DWORD_PTR>(this));Sync();
}
void SettingsFrame::Edge(){
    RECT rect{};GetWindowRect(window_,&rect);const int w=rect.right-rect.left,h=rect.bottom-rect.top;if(w<=0||h<=0)return;
    if(!edge_){WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PcTool.SettingsEdge";wc.lpfnWndProc=[](HWND w,UINT m,WPARAM wp,LPARAM lp)->LRESULT{if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(w,m,wp,lp);};RegisterClassW(&wc);edge_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,0,0,0,0,window_,nullptr,wc.hInstance,nullptr);}
    if(w!=width_||h!=height_){
        const double radius=MulDiv(8,dpi_,96);SetWindowRgn(window_,CreateRoundRectRgn(1,1,w-1,h-1,int(radius*2),int(radius*2)),TRUE);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels{};HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(bitmap){auto old=SelectObject(dc,bitmap);auto* data=static_cast<uint32_t*>(pixels);
            for(int y=0;y<h;++y)for(int x=0;x<w;++x){const double qx=std::abs(x+0.5-w/2.0)-(w/2.0-radius),qy=std::abs(y+0.5-h/2.0)-(h/2.0-radius);const double distance=std::hypot(std::max(qx,0.0),std::max(qy,0.0))+std::min(std::max(qx,qy),0.0)-radius;
                if(distance < -3){data[y*w+x]=0;continue;}const unsigned alpha=unsigned(std::clamp(0.5-distance,0.0,1.0)*255);const unsigned color=distance> -1.1?214:247;const unsigned channel=color*alpha/255;data[y*w+x]=(alpha<<24)|(channel<<16)|(channel<<8)|channel;
            }
            POINT pos{rect.left,rect.top},source{};SIZE size{w,h};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};UpdateLayeredWindow(edge_,nullptr,&pos,&size,dc,&source,0,&blend,ULW_ALPHA);SelectObject(dc,old);DeleteObject(bitmap);
        }DeleteDC(dc);width_=w;height_=h;
    }
    SetWindowPos(edge_,HWND_TOP,rect.left,rect.top,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
void SettingsFrame::Sync(){if(!IsWindow(window_)||syncing_)return;syncing_=true;RECT r{};GetClientRect(window_,&r);const int size=MulDiv(32,dpi_,96),inset=MulDiv(12,dpi_,96);if(close_)SetWindowPos(close_,HWND_TOP,r.right-inset-size,inset,size,size,SWP_NOACTIVATE);
    if(!IsWindowVisible(window_)||IsIconic(window_)){shadow_.Hide();if(edge_)ShowWindow(edge_,SW_HIDE);}else{shadow_.Sync(window_,dpi_);Edge();}syncing_=false;}
LRESULT CALLBACK SettingsFrame::CloseProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR ref){
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);HBRUSH bg=CreateSolidBrush(RGB(247,247,247));FillRect(dc,&r,bg);DeleteObject(bg);Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);const float d=float(reinterpret_cast<SettingsFrame*>(ref)->dpi_)/96;Gdiplus::Pen pen(Gdiplus::Color(255,96,98,102),1.6f*d);float mid=r.right/2.0f,half=5*d;g.DrawLine(&pen,mid-half,mid-half,mid+half,mid+half);g.DrawLine(&pen,mid+half,mid-half,mid-half,mid+half);EndPaint(w,&ps);return 0;}
    return DefSubclassProc(w,m,wp,lp);
}
LRESULT CALLBACK SettingsFrame::Proc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR ref){auto* self=reinterpret_cast<SettingsFrame*>(ref);if(m==WM_DPICHANGED)self->dpi_=HIWORD(wp);const int d=MulDiv(6,self->dpi_,96);
    // Returning zero preserves old client pixels during native edge resizing.
    // Layout has already moved the children, so those pixels are no longer valid.
    if(m==WM_NCCALCSIZE)return wp?WVR_REDRAW:0;if(m==WM_NCACTIVATE)return TRUE;
    if(m==WM_COMMAND&&LOWORD(wp)==7901){SendMessageW(w,WM_CLOSE,0,0);return 0;}
    if(m==WM_SYSCOMMAND&&((wp&0xfff0)==SC_MAXIMIZE||(wp&0xfff0)==SC_MINIMIZE))return 0;
    if(m==WM_NCLBUTTONDBLCLK)return 0;
    if(m==WM_NCHITTEST){RECT r{};GetWindowRect(w,&r);POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};const bool l=p.x<r.left+d,t=p.y<r.top+d,rr=p.x>=r.right-d,b=p.y>=r.bottom-d;if(t)return l?HTTOPLEFT:rr?HTTOPRIGHT:HTTOP;if(b)return l?HTBOTTOMLEFT:rr?HTBOTTOMRIGHT:HTBOTTOM;if(l)return HTLEFT;if(rr)return HTRIGHT;if(p.y<r.top+MulDiv(42,self->dpi_,96)&&p.x<r.right-MulDiv(58,self->dpi_,96))return HTCAPTION;return HTCLIENT;}
    if(m==WM_WINDOWPOSCHANGING){
        auto* pos=reinterpret_cast<WINDOWPOS*>(lp);RECT current{};GetWindowRect(w,&current);
        if(!(pos->flags&SWP_NOSIZE))pos->flags|=SWP_NOCOPYBITS;
        int x=(pos->flags&SWP_NOMOVE)?current.left:pos->x,y=(pos->flags&SWP_NOMOVE)?current.top:pos->y;
        int width=(pos->flags&SWP_NOSIZE)?current.right-current.left:pos->cx,height=(pos->flags&SWP_NOSIZE)?current.bottom-current.top:pos->cy;
        RECT r{x,y,x+width,y+height};MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&mi);const int margin=MulDiv(14,self->dpi_,96);
        if(!(pos->flags&SWP_NOSIZE)){pos->cx=width=std::min(width,int(mi.rcWork.right-mi.rcWork.left)-2*margin);pos->cy=height=std::min(height,int(mi.rcWork.bottom-mi.rcWork.top)-2*margin);}
        const int boundedX=std::clamp(x,int(mi.rcWork.left)+margin,std::max(int(mi.rcWork.left)+margin,int(mi.rcWork.right)-margin-width));
        const int boundedY=std::clamp(y,int(mi.rcWork.top)+margin,std::max(int(mi.rcWork.top)+margin,int(mi.rcWork.bottom)-margin-height));
        if(x!=boundedX||y!=boundedY){pos->flags&=~SWP_NOMOVE;pos->x=boundedX;pos->y=boundedY;}
    }
    if(m==WM_GETMINMAXINFO){auto result=DefSubclassProc(w,m,wp,lp);auto* mm=reinterpret_cast<MINMAXINFO*>(lp);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi);int margin=MulDiv(28,self->dpi_,96);mm->ptMaxTrackSize={mi.rcWork.right-mi.rcWork.left-margin,mi.rcWork.bottom-mi.rcWork.top-margin};mm->ptMinTrackSize.x=std::min(mm->ptMinTrackSize.x,mm->ptMaxTrackSize.x);mm->ptMinTrackSize.y=std::min(mm->ptMinTrackSize.y,mm->ptMaxTrackSize.y);return result;}
    if(m==WM_NCDESTROY){RemoveWindowSubclass(w,Proc,771);self->Cleanup();return DefSubclassProc(w,m,wp,lp);}
    auto result=DefSubclassProc(w,m,wp,lp);if(m==WM_DPICHANGED)self->width_=self->height_=0;if(m==WM_WINDOWPOSCHANGED||m==WM_SHOWWINDOW||m==WM_DPICHANGED)self->Sync();
    if(m==WM_EXITSIZEMOVE)RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    return result;
}
}
