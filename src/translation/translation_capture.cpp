#include "translation/translation_capture.h"
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
namespace translation {
RECT SelectionRect(POINT a,POINT b,RECT bounds) {
    return {std::clamp(std::min(a.x,b.x),bounds.left,bounds.right),std::clamp(std::min(a.y,b.y),bounds.top,bounds.bottom),
        std::clamp(std::max(a.x,b.x),bounds.left,bounds.right),std::clamp(std::max(a.y,b.y),bounds.top,bounds.bottom)};
}
struct TranslationCapture::Impl {
    HWND window{};capture::FrozenDesktop frozen;HBITMAP bitmap{};
    POINT first{},pointer{};bool dragging{};UINT dpi{96};ULONG_PTR gdiplus{};
    std::function<void(capture::Image)> complete;std::function<void()> canceled;
    Impl(){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&gdiplus,&input,nullptr);}
    ~Impl(){Close();if(gdiplus)Gdiplus::GdiplusShutdown(gdiplus);}
    int Dip(int n) const{return MulDiv(n,dpi,96);}
    void Close(){dragging=false;if(window)DestroyWindow(window);window=nullptr;if(bitmap)DeleteObject(bitmap);bitmap=nullptr;frozen={};complete={};canceled={};}
    void Cancel(){auto callback=canceled;Close();if(callback)callback();}
    void Paint(HDC output){
        const int width=frozen.image.width,height=frozen.image.height;
        HDC dc=CreateCompatibleDC(output),source=CreateCompatibleDC(output);
        HBITMAP buffer=CreateCompatibleBitmap(output,width,height);
        auto old=SelectObject(dc,buffer),oldSource=SelectObject(source,bitmap);
        BitBlt(dc,0,0,width,height,source,0,0,SRCCOPY);
        POINT screen=pointer;screen.x+=frozen.bounds.left;screen.y+=frozen.bounds.top;
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(screen,MONITOR_DEFAULTTONEAREST),&monitor);
        RECT work=monitor.rcMonitor;OffsetRect(&work,-frozen.bounds.left,-frozen.bounds.top);
        {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            if(dragging){
                RECT r=SelectionRect(first,pointer,{0,0,width,height});
                Gdiplus::SolidBrush fill(Gdiplus::Color(60,38,148,255));Gdiplus::Pen border(Gdiplus::Color(255,52,139,255),float(Dip(1)));
                const Gdiplus::RectF shape(float(r.left),float(r.top),float(r.right-r.left),float(r.bottom-r.top));
                g.FillRectangle(&fill,shape);g.DrawRectangle(&border,shape);
                wchar_t size[64]{};swprintf_s(size,L"%ld × %ld",r.right-r.left,r.bottom-r.top);
                int x=std::clamp<int>(r.left,work.left,std::max<int>(work.left,work.right-Dip(112)));
                int y=r.top-Dip(28);if(y<work.top)y=r.top+Dip(2);
                Gdiplus::SolidBrush dark(Gdiplus::Color(230,65,65,65)),white(Gdiplus::Color(255,255,255,255));
                g.FillRectangle(&dark,x,y,Dip(112),Dip(26));
                Gdiplus::Font font(L"Microsoft YaHei UI",float(Dip(12)),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
                g.DrawString(size,-1,&font,Gdiplus::PointF(float(x+Dip(8)),float(y+Dip(3))),&white);
            }
        }
        const int side=Dip(112),gap=Dip(20),sample=24;
        int x=pointer.x+gap,y=pointer.y+gap;
        if(x+side>work.right)x=pointer.x-gap-side;if(y+side>work.bottom)y=pointer.y-gap-side;
        x=std::clamp(x,int(work.left),std::max(int(work.left),int(work.right)-side));y=std::clamp(y,int(work.top),std::max(int(work.top),int(work.bottom)-side));
        const int sx=std::clamp(int(pointer.x)-sample/2,0,std::max(0,width-sample)),sy=std::clamp(int(pointer.y)-sample/2,0,std::max(0,height-sample));
        SetStretchBltMode(dc,COLORONCOLOR);StretchBlt(dc,x,y,side,side,source,sx,sy,sample,sample,SRCCOPY);
        {
            Gdiplus::Graphics g(dc);Gdiplus::Pen pen(Gdiplus::Color(255,105,105,105),float(Dip(1)));
            g.DrawRectangle(&pen,x,y,side,side);
            const int cx=x+MulDiv(int(pointer.x)-sx,side,sample),cy=y+MulDiv(int(pointer.y)-sy,side,sample);
            g.DrawLine(&pen,cx,y,cx,y+side);g.DrawLine(&pen,x,cy,x+side,cy);
        }
        BitBlt(output,0,0,width,height,dc,0,0,SRCCOPY);
        SelectObject(source,oldSource);SelectObject(dc,old);DeleteObject(buffer);DeleteDC(source);DeleteDC(dc);
    }
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);self->Paint(dc);EndPaint(w,&ps);return 0;}
        if(m==WM_MOUSEMOVE||m==WM_LBUTTONDOWN||m==WM_LBUTTONUP){
            self->pointer={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if(m==WM_LBUTTONDOWN){self->dragging=true;self->first=self->pointer;SetCapture(w);}
            if(m==WM_LBUTTONUP&&self->dragging){
                self->dragging=false;ReleaseCapture();
                auto r=SelectionRect(self->first,self->pointer,{0,0,self->frozen.image.width,self->frozen.image.height});
                if(r.right-r.left>=4&&r.bottom-r.top>=4){
                    auto image=self->frozen.image.Crop(r.left,r.top,r.right-r.left,r.bottom-r.top);
                    auto callback=self->complete;self->Close();if(callback)callback(std::move(image));return 0;
                }
            }
            InvalidateRect(w,nullptr,FALSE);return 0;
        }
        if(m==WM_CAPTURECHANGED){self->dragging=false;InvalidateRect(w,nullptr,FALSE);return 0;}
        if(m==WM_RBUTTONUP||m==WM_CLOSE||(m==WM_KEYDOWN&&wp==VK_ESCAPE)){self->Cancel();return 0;}
        if(m==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE){PostMessageW(w,WM_CLOSE,0,0);return 0;}
        if(m==WM_NCDESTROY){self->window=nullptr;SetWindowLongPtrW(w,GWLP_USERDATA,0);}
        return DefWindowProcW(w,m,wp,lp);
    }
};
TranslationCapture::TranslationCapture():impl_(std::make_unique<Impl>()){}
TranslationCapture::~TranslationCapture()=default;
bool TranslationCapture::Start(std::function<void(capture::Image)> complete,std::function<void()> canceled){
    if(Focus())return true;
    auto& s=*impl_;s.frozen=capture::FreezeDesktop();s.bitmap=capture::ToBitmap(s.frozen.image);
    if(!s.bitmap){s.Close();return false;}
    WNDCLASSW wc{};wc.lpfnWndProc=Impl::Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_CROSS);wc.lpszClassName=L"PcTool.TranslationCapture";RegisterClassW(&wc);
    s.complete=std::move(complete);s.canceled=std::move(canceled);
    GetCursorPos(&s.pointer);s.pointer.x-=s.frozen.bounds.left;s.pointer.y-=s.frozen.bounds.top;
    auto w=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"截图取词",WS_POPUP,s.frozen.bounds.left,s.frozen.bounds.top,s.frozen.image.width,s.frozen.image.height,nullptr,nullptr,wc.hInstance,&s);
    if(!w){s.Close();return false;}s.dpi=GetDpiForWindow(w);
    ShowWindow(w,SW_SHOW);SetForegroundWindow(w);SetFocus(w);return true;
}
void TranslationCapture::Cancel(){impl_->Cancel();}
bool TranslationCapture::Focus(){if(!impl_->window)return false;SetForegroundWindow(impl_->window);return true;}
}
