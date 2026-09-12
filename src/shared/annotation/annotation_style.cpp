#include "shared/annotation/annotation_style.h"
#include "shared/ui/toolbar_icons.h"
#include <objidl.h>
#include <gdiplus.h>
#include <winrt/base.h>

namespace capture {
namespace {
void Fill(HDC dc,RECT rect,COLORREF color) { HBRUSH brush=CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush); }
void Frame(HDC dc,RECT rect,COLORREF color) { HBRUSH brush=CreateSolidBrush(color); FrameRect(dc,&rect,brush); DeleteObject(brush); }
constexpr UINT CloseDropdownMessage=WM_APP+41;
}
void DrawToolbarButtonState(HDC dc,RECT bounds,UINT dpi,bool enabled,bool selected,bool hovered,bool pressed,COLORREF accent) {
    COLORREF fill=CLR_INVALID,border=CLR_INVALID;
    if(enabled && pressed) { fill=RGB(204,222,238); border=RGB(66,128,180); }
    else if(selected) { fill=ToolbarSelected; border=RGB(91,148,194); }
    else if(enabled && hovered) { fill=RGB(232,238,244); border=RGB(145,154,165); }
    if(accent!=CLR_INVALID) {
        const int shade=enabled && pressed?85:enabled && hovered?93:100;
        fill=RGB(GetRValue(accent)*shade/100,GetGValue(accent)*shade/100,GetBValue(accent)*shade/100); border=fill;
    }
    if(fill==CLR_INVALID) return;
    const auto d=[dpi](int v){return float(MulDiv(v,int(dpi),96));};
    Gdiplus::RectF r(bounds.left+d(2),bounds.top+d(3),bounds.right-bounds.left-d(4),bounds.bottom-bounds.top-d(6));
    const float diameter=d(6);
    Gdiplus::GraphicsPath path;
    path.AddArc(r.X,r.Y,diameter,diameter,180,90); path.AddArc(r.GetRight()-diameter,r.Y,diameter,diameter,270,90);
    path.AddArc(r.GetRight()-diameter,r.GetBottom()-diameter,diameter,diameter,0,90); path.AddArc(r.X,r.GetBottom()-diameter,diameter,diameter,90,90); path.CloseFigure();
    Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255,GetRValue(fill),GetGValue(fill),GetBValue(fill)));
    Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(border),GetGValue(border),GetBValue(border)),std::max(1.0F,float(dpi)/96));
    pen.SetLineJoin(Gdiplus::LineJoinRound); graphics.FillPath(&brush,&path); graphics.DrawPath(&pen,&path);
}
HWND CreateToolbarTooltipWindow(HWND owner,UINT dpi,bool excludeFromCapture) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    HWND tip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX|TTS_NOANIMATE|TTS_NOFADE,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!tip) return nullptr;
    // Native tooltip fading uses a layered popup, which Windows 10 cannot
    // exclude reliably. Keep the native theme and use an opaque, unfaded tip
    // for both screenshot and recording toolbars.
    SetWindowLongPtrW(tip,GWL_EXSTYLE,GetWindowLongPtrW(tip,GWL_EXSTYLE)&~WS_EX_LAYERED);
    if(excludeFromCapture && !SetWindowDisplayAffinity(tip,WDA_EXCLUDEFROMCAPTURE)) { DestroyWindow(tip); return nullptr; }
    SendMessageW(tip,TTM_SETDELAYTIME,TTDT_INITIAL,450); SendMessageW(tip,TTM_SETDELAYTIME,TTDT_RESHOW,100);
    SendMessageW(tip,TTM_SETMAXTIPWIDTH,0,MulDiv(240,int(dpi),96)); return tip;
}
HFONT CreateToolbarFont(UINT dpi) {
    return CreateFontW(-MulDiv(ToolbarFontPoints,int(dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
}
void DrawToolbarFontCombo(HDC dc,RECT bounds,UINT dpi,int points) {
    const auto d=[dpi](int v){return MulDiv(v,int(dpi),96);};
    Fill(dc,bounds,RGB(255,255,255)); Frame(dc,bounds,ToolbarBorder);
    HFONT font=CreateToolbarFont(dpi); auto oldFont=SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,ToolbarInk);
    RECT label{bounds.left+d(7),bounds.top,bounds.right-d(20),bounds.bottom};
    const auto text=std::to_wstring(points); DrawTextW(dc,text.c_str(),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(dc,oldFont); DeleteObject(font);
    const LONG cy=(bounds.top+bounds.bottom)/2;
    const POINT triangle[]={{bounds.right-d(16),cy-d(2)},{bounds.right-d(8),cy-d(2)},{bounds.right-d(12),cy+d(3)}};
    HBRUSH brush=CreateSolidBrush(ToolbarInk); auto oldBrush=SelectObject(dc,brush); auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
    Polygon(dc,triangle,3); SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(brush);
}
StylePanelLayout MakeStylePanelLayout(POINT origin,UINT dpi,bool text,bool preview) {
    const auto d=[dpi](int v){return MulDiv(v,int(dpi),96);};
    const auto r=[&](int x,int y,int w,int h){return RECT{origin.x+d(x),origin.y+d(y),origin.x+d(x+w),origin.y+d(y+h)};};
    StylePanelLayout layout; const int palette=(text?104:112)+(preview?40:0);
    layout.bounds=r(0,0,palette+168,48); layout.fontCombo=r(38,9,52,30);
    if(preview) layout.preview=r(palette-40,6,32,36);
    for(int i=0;i<3;++i) layout.widths[i]=r(8+i*32,5,32,38);
    for(int i=0;i<16;++i) layout.colors[i]=r(palette+i%8*20,4+i/8*20,18,18);
    return layout;
}
int StylePanelLayout::Hit(POINT p,bool text) const {
    if(text && PtInRect(&fontCombo,p)) return 200;
    if(!text) for(int i=0;i<3;++i) if(PtInRect(&widths[i],p)) return i+1;
    for(int i=0;i<16;++i) if(PtInRect(&colors[i],p)) return 100+i;
    return 0;
}
class AnnotationStylePanel::FontListWindow final : public ToolWindow {
public:
    explicit FontListWindow(AnnotationStylePanel& panel):panel_(panel) {
        if(!Create(L"PcTool · 录屏字号列表",52,1,WS_POPUP,WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE)) winrt::throw_last_error();
        SetWindowLongPtrW(window_,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(panel.Window()));
        if(!SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE)) winrt::throw_last_error();
    }
    void PlaceList(RECT rect,UINT dpi) {
        dpi_=dpi; RECT old{}; GetWindowRect(window_,&old);
        if(!EqualRect(&old,&rect)) SetWindowPos(window_,HWND_TOPMOST,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOACTIVATE|SWP_NOREDRAW);
        // Paint the new list before showing it; the palette's window never resizes.
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);
        ShowWindow(window_,SW_SHOWNOACTIVATE);
    }
private:
    int Hit(POINT p) const {
        RECT r{}; GetClientRect(window_,&r); InflateRect(&r,-1,-1);
        if(!PtInRect(&r,p)) return 0;
        const int index=(p.y-1)/std::max(1,Dip(22));
        return index<panel_.fontRows_?300+panel_.fontFirst_+index:0;
    }
    void Paint(HDC target) {
        RECT r{}; GetClientRect(window_,&r); if(r.right<=0 || r.bottom<=0) return;
        HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,r.right,r.bottom); auto old=SelectObject(dc,bitmap);
        Fill(dc,r,RGB(255,255,255)); Frame(dc,r,ToolbarBorder);
        HFONT font=CreateToolbarFont(dpi_); auto oldFont=SelectObject(dc,font);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,ToolbarInk);
        for(int index=0;index<panel_.fontRows_;++index) {
            const int size=panel_.fontFirst_+index;
            RECT row{1,1+Dip(22)*index,r.right-1,1+Dip(22)*(index+1)};
            if((hover_?hover_-300:panel_.style_.fontPoints)==size) Fill(dc,row,ToolbarSelected);
            const auto label=std::to_wstring(size); DrawTextW(dc,label.c_str(),-1,&row,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        }
        SelectObject(dc,oldFont); DeleteObject(font); BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l) override {
        if(m==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if(m==WM_ERASEBKGND) return 1;
        if(m==WM_PAINT) { PAINTSTRUCT ps{}; auto dc=BeginPaint(window_,&ps); Paint(dc); EndPaint(window_,&ps); return 0; }
        if(m==WM_PRINTCLIENT) { Paint(reinterpret_cast<HDC>(w)); return 0; }
        if(m==WM_MOUSEMOVE) { hover_=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); InvalidateRect(window_,nullptr,FALSE); TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window_,0}; TrackMouseEvent(&track); return 0; }
        if(m==WM_MOUSELEAVE) { hover_=0; InvalidateRect(window_,nullptr,FALSE); return 0; }
        if(m==WM_MOUSEWHEEL) { panel_.fontFirst_=std::clamp(panel_.fontFirst_-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA,MinimumFontPoints,MaximumFontPoints-panel_.fontRows_+1); hover_=0; InvalidateRect(window_,nullptr,FALSE); return 0; }
        if(m==WM_LBUTTONDOWN) { pressed_=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); SetCapture(window_); return 0; }
        if(m==WM_LBUTTONUP) {
            const int hit=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}),pressed=pressed_; pressed_=0; ReleaseCapture();
            if(hit && hit==pressed) { panel_.style_.fontPoints=hit-300; panel_.CloseFontDropdown(); panel_.changed_(panel_.style_); InvalidateRect(panel_.Window(),nullptr,FALSE); }
            return 0;
        }
        if(m==WM_KEYDOWN && w==VK_ESCAPE) { panel_.CloseFontDropdown(); return 0; }
        if(m==WM_SHOWWINDOW && !w) { hover_=pressed_=0; if(GetCapture()==window_) ReleaseCapture(); }
        return ToolWindow::Handle(m,w,l);
    }
    AnnotationStylePanel& panel_; int hover_{},pressed_{};
};
AnnotationStylePanel::AnnotationStylePanel(HWND owner,std::function<void(AnnotationStyle)> changed,std::function<void()> escape)
    :changed_(std::move(changed)),escape_(std::move(escape)) {
    if(!Create(L"PcTool · 录屏样式",320,48,WS_POPUP,WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE)) winrt::throw_last_error();
    SetWindowLongPtrW(window_,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(owner));
    if(!SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE)) winrt::throw_last_error();
}
void AnnotationStylePanel::Update(AnnotationStyle style,bool text,UINT dpi,RECT toolbar,RECT monitor) {
    if(text_!=text) fontOpen_=false;
    style_=style; text_=text; dpi_=dpi; toolbar_=toolbar; monitor_=monitor;
    layout_=MakeStylePanelLayout({},dpi_,text_,true); Position();
}
AnnotationStylePanel::~AnnotationStylePanel() { Hide(); }
void AnnotationStylePanel::Hide() {
    fontOpen_=false; hover_=pressed_=0; WatchOutsideClicks(); if(fontWindow_) ShowWindow(fontWindow_->Window(),SW_HIDE); ShowWindow(window_,SW_HIDE);
}
bool AnnotationStylePanel::CloseFontDropdown() {
    if(!fontOpen_) return false;
    fontOpen_=false; hover_=pressed_=0; WatchOutsideClicks(); if(fontWindow_) ShowWindow(fontWindow_->Window(),SW_HIDE); return true;
}
void AnnotationStylePanel::WatchOutsideClicks() {
    if(fontOpen_ && !mouseHook_) {
        if(openPanel_ && openPanel_!=this) openPanel_->CloseFontDropdown();
        mouseHook_=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
        if(mouseHook_) openPanel_=this;
    } else if(!fontOpen_ && mouseHook_) {
        UnhookWindowsHookEx(mouseHook_); mouseHook_=nullptr;
        if(openPanel_==this) openPanel_=nullptr;
    }
}
LRESULT CALLBACK AnnotationStylePanel::MouseHook(int code,WPARAM message,LPARAM data) {
    auto* panel=openPanel_;
    if(code==HC_ACTION && panel && (message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_XBUTTONDOWN)) {
        const POINT screen=reinterpret_cast<MSLLHOOKSTRUCT*>(data)->pt;
        POINT point=screen; ScreenToClient(panel->window_,&point);
        const HWND target=WindowFromPoint(screen);
        const bool overList=panel->fontWindow_ && target==panel->fontWindow_->Window();
        if(!overList && (target!=panel->window_ || !PtInRect(&panel->layout_.fontCombo,point)))
            PostMessageW(panel->window_,CloseDropdownMessage,0,0);
    }
    // Never capture or swallow the click intended for the drawing surface or another app.
    return CallNextHookEx(nullptr,code,message,data);
}
void AnnotationStylePanel::Position() {
    WatchOutsideClicks();
    layout_=MakeStylePanelLayout({},dpi_,text_,true);
    const int width=layout_.bounds.right,height=layout_.bounds.bottom;
    int x=std::clamp<int>(toolbar_.left,monitor_.left,std::max(monitor_.left,monitor_.right-width));
    int y=toolbar_.bottom+Dip(4);
    if(y+height>monitor_.bottom) y=toolbar_.top-height-Dip(4);
    y=std::clamp<int>(y,monitor_.top,std::max(monitor_.top,monitor_.bottom-height));
    RECT destination{x,y,x+width,y+height},old{}; GetWindowRect(window_,&old);
    if(!EqualRect(&destination,&old)) SetWindowPos(window_,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE);
    if(!IsWindowVisible(window_)) ShowWindow(window_,SW_SHOWNOACTIVATE);
    InvalidateRect(window_,nullptr,FALSE);
    PositionFontList();
}
void AnnotationStylePanel::PositionFontList() {
    if(!fontOpen_) { if(fontWindow_) ShowWindow(fontWindow_->Window(),SW_HIDE); return; }
    RECT combo=layout_.fontCombo; MapWindowPoints(window_,nullptr,reinterpret_cast<POINT*>(&combo),2);
    const int row=std::max(1,Dip(22)),gap=Dip(2),count=MaximumFontPoints-MinimumFontPoints+1;
    const int below=monitor_.bottom-combo.bottom-gap,above=combo.top-gap-monitor_.top;
    const bool upward=below<count*row+2 && above>below;
    fontRows_=std::min(count,std::max(0,((upward?above:below)-2)/row));
    if(!fontRows_) { CloseFontDropdown(); return; }
    fontFirst_=std::clamp(fontFirst_,MinimumFontPoints,MaximumFontPoints-fontRows_+1);
    const int height=fontRows_*row+2,top=upward?combo.top-gap-height:combo.bottom+gap;
    fontList_={combo.left,top,combo.right,top+height};
    if(!fontWindow_) fontWindow_=std::make_unique<FontListWindow>(*this);
    fontWindow_->PlaceList(fontList_,dpi_);
}
int AnnotationStylePanel::Hit(POINT p) const { return layout_.Hit(p,text_); }
void AnnotationStylePanel::Paint(HDC target) {
    RECT client{}; GetClientRect(window_,&client);
    HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,client.right,client.bottom); auto old=SelectObject(dc,bitmap);
    Fill(dc,client,ToolbarBackground); Frame(dc,layout_.bounds,ToolbarBorder);
    HFONT font=CreateToolbarFont(dpi_); auto oldFont=SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,ToolbarInk);
    {
        Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if(!text_) for(int i=0;i<3;++i) {
            RECT r=layout_.widths[i]; InflateRect(&r,-Dip(3),-Dip(4));
            if(style_.lineIndex==i || hover_==i+1) { Fill(dc,r,ToolbarSelected); Frame(dc,r,ToolbarBorder); }
            const float diameter=float(Dip(std::array<int,3>{4,7,11}[i]));
            const float x=(r.left+r.right-diameter)/2,y=(r.top+r.bottom-diameter)/2;
            Gdiplus::SolidBrush dot(Gdiplus::Color(255,GetRValue(style_.color),GetGValue(style_.color),GetBValue(style_.color)));
            g.FillEllipse(&dot,x,y,diameter,diameter);
        }
    }
    if(text_) {
        RECT icon{Dip(4),layout_.bounds.top,Dip(34),layout_.bounds.bottom}; DrawToolbarIcon(dc,ToolbarIcon::Text,icon,dpi_,ToolbarInk,22);
        DrawToolbarFontCombo(dc,layout_.fontCombo,dpi_,style_.fontPoints);
    }
    RECT separator{Dip(text_?96:104),layout_.bounds.top+Dip(7),Dip(text_?96:104)+1,layout_.bounds.top+Dip(41)}; Fill(dc,separator,ToolbarBorder);
    Fill(dc,layout_.preview,style_.color); Frame(dc,layout_.preview,ToolbarBorder);
    for(int i=0;i<16;++i) {
        RECT r=layout_.colors[i]; Fill(dc,r,AnnotationColors[i]); Frame(dc,r,ToolbarBorder);
        if(style_.color==AnnotationColors[i] || hover_==100+i) { InflateRect(&r,Dip(1),Dip(1)); Frame(dc,r,ToolbarInk); }
    }
    SelectObject(dc,oldFont); DeleteObject(font); BitBlt(target,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
}
LRESULT AnnotationStylePanel::Handle(UINT m,WPARAM w,LPARAM l) {
    if(m==CloseDropdownMessage) { CloseFontDropdown(); return 0; }
    if(m==WM_SHOWWINDOW && !w) { CloseFontDropdown(); }
    if(m==WM_DESTROY) { CloseFontDropdown(); }
    if(m==WM_PAINT) { PAINTSTRUCT ps{}; auto dc=BeginPaint(window_,&ps); Paint(dc); EndPaint(window_,&ps); return 0; }
    if(m==WM_PRINTCLIENT) { Paint(reinterpret_cast<HDC>(w)); return 0; }
    if(m==WM_ERASEBKGND) return 1;
    if(m==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(m==WM_MOUSEWHEEL && fontOpen_ && fontWindow_) return SendMessageW(fontWindow_->Window(),m,w,l);
    if(m==WM_KEYDOWN && w==VK_ESCAPE) { if(!CloseFontDropdown()) escape_(); return 0; }
    if(m==WM_MOUSEMOVE) { hover_=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); InvalidateRect(window_,nullptr,FALSE); TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window_,0}; TrackMouseEvent(&t); return 0; }
    if(m==WM_MOUSELEAVE) { hover_=0; InvalidateRect(window_,nullptr,FALSE); return 0; }
    if(m==WM_LBUTTONDOWN) {
        POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(fontOpen_ && !PtInRect(&layout_.fontCombo,point)) {
            ClientToScreen(window_,&point); CloseFontDropdown(); ScreenToClient(window_,&point);
        }
        pressed_=Hit(point); SetCapture(window_); return 0;
    }
    if(m==WM_LBUTTONUP) {
        const int hit=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}),pressed=pressed_; pressed_=0; ReleaseCapture();
        if(hit && hit==pressed) {
            if(hit==200) { if(!CloseFontDropdown()) { fontOpen_=true; fontFirst_=MinimumFontPoints; WatchOutsideClicks(); PositionFontList(); } }
            else { if(hit<=3) style_.lineIndex=hit-1; else style_.color=AnnotationColors[hit-100]; changed_(style_); }
            if(hit!=200) InvalidateRect(window_,nullptr,FALSE);
        } return 0;
    }
    return ToolWindow::Handle(m,w,l);
}
}

