#include "clipboard/clipboard_history.h"
#include "shared/ui/popup_controls.h"
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <thread>

bool ClipboardHistoryModel::Add(std::wstring text) {
    if(text.empty()) return false;
    auto found=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e){return e->text==text;});
    if(found==entries_.begin() && found!=entries_.end()) return false;
    std::shared_ptr<const ClipboardHistoryEntry> entry;
    if(found!=entries_.end()) { entry=*found; entries_.erase(found); }
    else entry=std::make_shared<ClipboardHistoryEntry>(ClipboardHistoryEntry{nextId_++,std::move(text)});
    entries_.insert(entries_.begin(),std::move(entry));
    if(entries_.size()>100) entries_.pop_back();
    return true;
}
std::wstring ClipboardHistoryModel::Summary(const std::wstring& text) {
    std::wstring result; result.reserve(std::min<size_t>(text.size(),1024));
    size_t i=0;
    for(;i<text.size() && result.size()<1024;++i) {
        wchar_t c=text[i];
        if(c==L'\r' && i+1<text.size() && text[i+1]==L'\n') ++i;
        result+=c==L'\r' || c==L'\n' || c==L'\t'?L' ':c;
    }
    if(i<text.size()) { if(!result.empty() && result.back()>=0xd800 && result.back()<=0xdbff) result.pop_back(); result+=L"..."; }
    return result;
}
namespace {
constexpr UINT OutsideMessage=WM_APP+71;
constexpr int HistoryListTop=44;
constexpr COLORREF Paper=RGB(255,255,255),DetailPaper=RGB(102,102,102),Transparent=RGB(101,102,102);
void RoundFill(HDC dc,RECT r,float radius,COLORREF color) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path; float x=float(r.left),y=float(r.top),w=float(r.right-r.left),h=float(r.bottom-r.top),d=radius*2;
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90); path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
    Gdiplus::SolidBrush brush(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color))); g.FillPath(&brush,&path);
}
void Fill(HDC dc,RECT rect,COLORREF color) { HBRUSH b=CreateSolidBrush(color); FillRect(dc,&rect,b); DeleteObject(b); }
std::wstring ErrorText(DWORD error) {
    wchar_t* buffer{}; FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,reinterpret_cast<wchar_t*>(&buffer),0,nullptr);
    std::wstring result=buffer?buffer:L"未知错误"; if(buffer) LocalFree(buffer); return result;
}
}
struct ClipboardHistory::Impl {
    HWND listener{},window{},list{},detail{},detailFrame{},edit{},status{};
    HFONT font{},titleFont{},smallFont{}; HBRUSH paper=CreateSolidBrush(Paper),detailBrush=CreateSolidBrush(DetailPaper);
    shared_ui::PopupShadow shadow{L"PcTool.ClipboardShadow"};
    shared_ui::SlimScrollbar listScroll,detailScroll;
    int wheelRemainder{},frameDrag{};RECT dragStart{};POINT dragCursor{};
    bool detailOnRight=true; int detailTip=24; uint64_t rowHover{};
    POINT hoverPoint{};bool haveHoverPoint{};
    std::thread mouseThread; DWORD mouseThreadId{}; ULONG_PTR gdiplus{};
    ClipboardHistoryModel model;
    std::shared_ptr<const ClipboardHistoryEntry> shown;
    DWORD readSequence{}; bool haveSequence{}; int retries{}; UINT dpi=96;
    uint64_t hoverId{},suppressedHover{}; ULONGLONG leaveSince{},feedbackUntil{};
    static inline thread_local HWND hookWindow{};
    int Dip(int value) const { return MulDiv(value,int(dpi),96); }
    ~Impl() { Shutdown(); DeleteObject(paper); DeleteObject(detailBrush); }
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp) {
        Impl* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE) { self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        return self?self->Handle(w,m,wp,lp):DefWindowProcW(w,m,wp,lp);
    }
    bool Start() {
        if(listener) return true;
        Gdiplus::GdiplusStartupInput startup;
        if(!gdiplus && Gdiplus::GdiplusStartup(&gdiplus,&startup,nullptr)!=Gdiplus::Ok) return false;
        for(const auto* name:{L"PcTool.ClipboardListener",L"PcTool.ClipboardHistory",L"PcTool.ClipboardDetail"}) {
            WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=Proc; wc.lpszClassName=name; wc.style=0; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        listener=CreateWindowExW(0,L"PcTool.ClipboardListener",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),this);
        if(!listener || !AddClipboardFormatListener(listener)) { if(listener) DestroyWindow(listener); listener=nullptr; return false; }
        ReadClipboard(); return true;
    }
    void Shutdown() {
        Close();
        if(listener) { KillTimer(listener,1); RemoveClipboardFormatListener(listener); DestroyWindow(listener); listener=nullptr; }
        model=ClipboardHistoryModel{}; haveSequence=false;
        if(font) { DeleteObject(font); font=nullptr; }
        if(titleFont) { DeleteObject(titleFont); titleFont=nullptr; }
        if(smallFont) { DeleteObject(smallFont); smallFont=nullptr; }
        if(gdiplus) { Gdiplus::GdiplusShutdown(gdiplus); gdiplus=0; }
    }
    void ReadClipboard() {
        const DWORD sequence=GetClipboardSequenceNumber();
        if(haveSequence && sequence==readSequence) return;
        if(!OpenClipboard(listener)) {
            if(++retries<=20) SetTimer(listener,1,50,nullptr);
            return;
        }
        std::wstring text;
        if(IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HGLOBAL memory=GetClipboardData(CF_UNICODETEXT); const SIZE_T size=memory?GlobalSize(memory)/sizeof(wchar_t):0;
            if(size) { auto* chars=static_cast<const wchar_t*>(GlobalLock(memory));
                if(chars) { const auto end=std::find(chars,chars+size,L'\0'); text.assign(chars,end); GlobalUnlock(memory); }
            }
        }
        readSequence=GetClipboardSequenceNumber(); haveSequence=true; CloseClipboard(); KillTimer(listener,1); retries=0;
        if(model.Add(std::move(text))) Refresh();
    }
    void MakeFont() {
        HFONT previous=font;
        font=CreateFontW(-Dip(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        for(HWND child:{list,status,edit}) if(child) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        if(list) SendMessageW(list,LB_SETITEMHEIGHT,0,Dip(34));
        if(previous) DeleteObject(previous);
        if(titleFont) DeleteObject(titleFont); if(smallFont) DeleteObject(smallFont);
        titleFont=CreateFontW(-Dip(18),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        smallFont=CreateFontW(-Dip(11),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        if(status) SendMessageW(status,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
    }
    void Show() {
        POINT point{}; GetCursorPos(&point); MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&mi);
        if(window) {
            CloseDetail(); hoverId=suppressedHover=rowHover=0;
            Center(mi.rcWork);
            SetForegroundWindow(window); SetFocus(list); InvalidateRect(list,nullptr,FALSE);
            return;
        }
        window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_COMPOSITED,L"PcTool.ClipboardHistory",L"剪切板历史 · PcTool",WS_POPUP|WS_CLIPCHILDREN,
            mi.rcWork.left+20,mi.rcWork.top+20,540,440,nullptr,nullptr,GetModuleHandleW(nullptr),this);
        if(!window) return;
        dpi=GetDpiForWindow(window);
        list=CreateWindowExW(0,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,
            0,0,1,1,window,reinterpret_cast<HMENU>(101),GetModuleHandleW(nullptr),nullptr);
        status=CreateWindowExW(0,L"STATIC",L"Ctrl+C 复制   ·   悬停查看全文   ·   Esc 关闭",WS_CHILD|WS_VISIBLE,0,0,1,1,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetWindowSubclass(list,ChildProc,1,reinterpret_cast<DWORD_PTR>(this));
        listScroll.Create(window,104,[this](int position){ScrollListTo(position);},Paper);
        MakeFont();
        int width=std::min(Dip(480),int(mi.rcWork.right-mi.rcWork.left)),height=std::min(Dip(112+34*std::clamp<int>(int(model.Entries().size()),3,10)),int(mi.rcWork.bottom-mi.rcWork.top));
        SetWindowPos(window,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        Layout(); Refresh(); Center(mi.rcWork); SetForegroundWindow(window); SetFocus(list); SetTimer(window,2,50,nullptr);
        // Keep hook dispatch independent of painting, clipboard reads and dialogs.
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(ready) {
            const HWND target=window;
            mouseThread=std::thread([this,target,ready] {
                mouseThreadId=GetCurrentThreadId(); hookWindow=target;
                MSG message{}; PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
                HHOOK hook=SetWindowsHookExW(WH_MOUSE_LL,MouseProc,GetModuleHandleW(nullptr),0);
                SetEvent(ready);
                while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
                if(hook) UnhookWindowsHookEx(hook); hookWindow=nullptr;
            });
            WaitForSingleObject(ready,INFINITE); CloseHandle(ready);
        }
    }
    void CloseDetail() { shown.reset(); if(detailFrame)DestroyWindow(detailFrame);detailFrame=nullptr;if(detail) DestroyWindow(detail); detail=edit=nullptr; leaveSince=0; detailWheelRemainder=0; }
    void Close() {
        if(mouseThread.joinable()) { PostThreadMessageW(mouseThreadId,WM_QUIT,0,0); mouseThread.join(); } mouseThreadId=0;
        EndFrameDrag(false);CloseDetail();shadow.Close(); if(window) { KillTimer(window,2); DestroyWindow(window); } window=list=status=nullptr; hoverId=suppressedHover=rowHover=0;haveHoverPoint=false;
    }
    bool InWindows(HWND target) const { return target && (target==window || target==detailFrame || IsChild(window,target) || (detail && (target==detail || IsChild(detail,target)))); }
    static LRESULT CALLBACK MouseProc(int code,WPARAM wp,LPARAM lp) {
        if(code==HC_ACTION && hookWindow && (wp==WM_LBUTTONDOWN || wp==WM_RBUTTONDOWN || wp==WM_MBUTTONDOWN || wp==WM_XBUTTONDOWN)) {
            const auto* mouse=reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
            // Never perform cross-process hit testing inside the low-level hook.
            // A slow target window can otherwise exceed LowLevelHooksTimeout.
            PostMessageW(hookWindow,OutsideMessage,0,MAKELPARAM(mouse->pt.x,mouse->pt.y));
        }
        return CallNextHookEx(nullptr,code,wp,lp);
    }
    uint64_t ItemId(int index) const { return index>=0?uint64_t(SendMessageW(list,LB_GETITEMDATA,index,0)):0; }
    std::shared_ptr<const ClipboardHistoryEntry> Entry(uint64_t id) const {
        for(const auto& e:model.Entries()) if(e->id==id) return e; return {};
    }
    void Refresh() {
        if(!list) return;
        const auto selected=ItemId(int(SendMessageW(list,LB_GETCURSEL,0,0))),top=ItemId(int(SendMessageW(list,LB_GETTOPINDEX,0,0)));
        SendMessageW(list,WM_SETREDRAW,FALSE,0); SendMessageW(list,LB_RESETCONTENT,0,0);
        int i=0;
        for(const auto& e:model.Entries()) {
            const auto summary=ClipboardHistoryModel::Summary(e->text);
            SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(summary.c_str())); SendMessageW(list,LB_SETITEMDATA,i,LPARAM(e->id));
            if(e->id==selected) SendMessageW(list,LB_SETCURSEL,i,0);
            if(e->id==top) SendMessageW(list,LB_SETTOPINDEX,i,0);
            ++i;
        }
        SendMessageW(list,WM_SETREDRAW,TRUE,0); SyncListScroll(); InvalidateRect(list,nullptr,FALSE);
        InvalidateRect(window,nullptr,FALSE);
        if(GetTickCount64()>=feedbackUntil) SetWindowTextW(status,model.Entries().empty()?L"暂无文本记录，复制文本后会显示在这里":L"Ctrl+C 复制   ·   悬停查看全文   ·   Esc 关闭");
        Hover();
    }
    void Center(RECT work) {
        // Moving across monitors can synchronously change DPI and size.
        for(int pass=0;pass<2;++pass) {
            RECT area=work;InflateRect(&area,-Dip(14),-Dip(14));
            RECT bounds{};GetWindowRect(window,&bounds);
            const int width=std::min<int>(bounds.right-bounds.left,std::max(1L,area.right-area.left));
            const int height=std::min<int>(bounds.bottom-bounds.top,std::max(1L,area.bottom-area.top));
            SetWindowPos(window,HWND_TOP,area.left+(area.right-area.left-width)/2,area.top+(area.bottom-area.top-height)/2,width,height,SWP_SHOWWINDOW);
        }
        shadow.Sync(window,dpi);
    }
    void EndFrameDrag(bool cancel) {
        if(!frameDrag)return;frameDrag=0;
        if(cancel)SetWindowPos(window,nullptr,dragStart.left,dragStart.top,dragStart.right-dragStart.left,dragStart.bottom-dragStart.top,SWP_NOZORDER|SWP_NOACTIVATE);
        if(GetCapture()==window)ReleaseCapture();
    }
    void MoveFrame() {
        POINT cursor{};GetCursorPos(&cursor);RECT r=dragStart;
        const int dx=cursor.x-dragCursor.x,dy=cursor.y-dragCursor.y;
        if(frameDrag==HTCAPTION)OffsetRect(&r,dx,dy);
        else {
            if(frameDrag==HTLEFT||frameDrag==HTTOPLEFT||frameDrag==HTBOTTOMLEFT)r.left=std::min(r.left+dx,r.right-Dip(320));
            if(frameDrag==HTRIGHT||frameDrag==HTTOPRIGHT||frameDrag==HTBOTTOMRIGHT)r.right=std::max(r.right+dx,r.left+Dip(320));
            if(frameDrag==HTTOP||frameDrag==HTTOPLEFT||frameDrag==HTTOPRIGHT)r.top=std::min(r.top+dy,r.bottom-Dip(220));
            if(frameDrag==HTBOTTOM||frameDrag==HTBOTTOMLEFT||frameDrag==HTBOTTOMRIGHT)r.bottom=std::max(r.bottom+dy,r.top+Dip(220));
        }
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);RECT work=mi.rcWork;InflateRect(&work,-Dip(14),-Dip(14));
        int width=std::min(int(r.right-r.left),int(work.right-work.left)),height=std::min(int(r.bottom-r.top),int(work.bottom-work.top));
        r.left=std::clamp<int>(r.left,work.left,work.right-width);r.top=std::clamp<int>(r.top,work.top,work.bottom-height);
        SetWindowPos(window,nullptr,r.left,r.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void SyncListScroll() {
        if(!list||!listScroll.Window())return;RECT r{};GetClientRect(list,&r);
        const int row=std::max(1,int(SendMessageW(list,LB_GETITEMHEIGHT,0,0)));
        listScroll.Update(int(SendMessageW(list,LB_GETCOUNT,0,0)),std::max(1,int(r.bottom)/row),int(SendMessageW(list,LB_GETTOPINDEX,0,0)),dpi);
    }
    void ScrollListTo(int position) {
        CloseDetail();hoverId=rowHover=suppressedHover=0;
        SendMessageW(list,LB_SETTOPINDEX,std::clamp(position,0,listScroll.Maximum()),0);SyncListScroll();InvalidateRect(list,nullptr,FALSE);
    }
    void WheelList(WPARAM wp) {
        wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);int steps=wheelRemainder/WHEEL_DELTA;wheelRemainder%=WHEEL_DELTA;
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        if(steps)ScrollListTo(listScroll.Position()-steps*(lines==WHEEL_PAGESCROLL?listScroll.Page():int(lines)));
    }
    int DetailLineHeight() {
        HDC dc=GetDC(edit);auto old=SelectObject(dc,font);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(edit,dc);return std::max(1L,metrics.tmHeight);
    }
    void SyncDetailScroll() {
        if(!edit||!detailScroll.Window())return;RECT r{};GetClientRect(edit,&r);
        detailScroll.Update(int(SendMessageW(edit,EM_GETLINECOUNT,0,0)),std::max(1,int(r.bottom)/DetailLineHeight()),int(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)),dpi);
    }
    void ScrollDetailTo(int position) {
        if(!edit)return;position=std::clamp(position,0,detailScroll.Maximum());SendMessageW(edit,EM_LINESCROLL,0,position-int(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)));SyncDetailScroll();
    }
    int detailWheelRemainder{};
    void WheelDetail(WPARAM wp) {
        detailWheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);
        const int steps=detailWheelRemainder/WHEEL_DELTA;detailWheelRemainder%=WHEEL_DELTA;
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        if(steps&&lines)ScrollDetailTo(detailScroll.Position()-steps*(lines==WHEEL_PAGESCROLL?detailScroll.Page():int(std::min<UINT>(lines,UINT(std::max(1,detailScroll.Maximum()))))));
    }
    void Layout() {
        if(!list) return; RECT r{}; GetClientRect(window,&r);
        SetWindowRgn(window,CreateRoundRectRgn(0,0,r.right+1,r.bottom+1,Dip(16),Dip(16)),FALSE);
        const UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS;
        const int height=std::max(1L,r.bottom-Dip(HistoryListTop+40));
        HDWP layout=BeginDeferWindowPos(3);
        if(layout) layout=DeferWindowPos(layout,list,nullptr,Dip(12),Dip(HistoryListTop),std::max(1L,r.right-Dip(24)),height,flags);
        if(layout) layout=DeferWindowPos(layout,listScroll.Window(),nullptr,r.right-Dip(9),Dip(HistoryListTop),Dip(6),height,flags);
        if(layout) layout=DeferWindowPos(layout,status,nullptr,Dip(18),r.bottom-Dip(28),std::max(1L,r.right-Dip(36)),Dip(20),flags);
        if(layout) EndDeferWindowPos(layout);
        else {
            SetWindowPos(list,nullptr,Dip(12),Dip(HistoryListTop),std::max(1L,r.right-Dip(24)),height,flags);
            SetWindowPos(listScroll.Window(),nullptr,r.right-Dip(9),Dip(HistoryListTop),Dip(6),height,flags);
            SetWindowPos(status,nullptr,Dip(18),r.bottom-Dip(28),std::max(1L,r.right-Dip(36)),Dip(20),flags);
        }
        SyncListScroll();shadow.Sync(window,dpi);
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    void DrawDetailFrame() {
        if(!detailFrame)return;RECT r{};GetWindowRect(detail,&r);const int width=r.right-r.left,height=r.bottom-r.top;
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};void* pixels{};
        HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap){DeleteDC(dc);ReleaseDC(nullptr,screen);return;}auto old=SelectObject(dc,bitmap);
        {Gdiplus::Bitmap surface(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(pixels));Gdiplus::Graphics g(&surface);g.Clear(Gdiplus::Color(0,0,0,0));g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const float x=float(detailOnRight?Dip(8):0),right=float(width-(detailOnRight?0:Dip(8))),d=float(Dip(12));Gdiplus::GraphicsPath path;
        path.AddArc(x,0.0f,d,d,180.0f,90.0f);path.AddArc(right-d,0.0f,d,d,270.0f,90.0f);path.AddArc(right-d,height-d,d,d,0.0f,90.0f);path.AddArc(x,height-d,d,d,90.0f,90.0f);path.CloseFigure();
        Gdiplus::SolidBrush brush(Gdiplus::Color(255,102,102,102));g.FillPath(&brush,&path);
        float edge=detailOnRight?x:right;Gdiplus::PointF points[]={{edge,float(detailTip-Dip(7))},{float(detailOnRight?0:width),float(detailTip)},{edge,float(detailTip+Dip(7))}};g.FillPolygon(&brush,points,3);}
        POINT origin{r.left,r.top},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};UpdateLayeredWindow(detailFrame,screen,&origin,&size,dc,&source,0,&blend,ULW_ALPHA);
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
        SetWindowPos(detailFrame,detail,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    void DetailLayout() {
        if(!edit) return; RECT r{}; GetClientRect(detail,&r);
        // Keep native controls inside the body; the alpha frame draws the outline.
        const int left=detailOnRight?Dip(8):0,right=r.right-(detailOnRight?0:Dip(8));
        // Native text lives wholly inside the opaque portion. The separate alpha
        // frame supplies smooth exterior pixels without color-key/region stair steps.
        HRGN shape=CreateRoundRectRgn(left+2,2,right-1,r.bottom-1,Dip(12),Dip(12));
        if(!SetWindowRgn(detail,shape,TRUE)) DeleteObject(shape);
        const int textLeft=left+Dip(16),barLeft=right-Dip(9);
        MoveWindow(edit,textLeft,Dip(12),std::max(1,barLeft-Dip(4)-textLeft),std::max(1L,r.bottom-Dip(24)),TRUE);
        MoveWindow(detailScroll.Window(),barLeft,Dip(12),Dip(6),std::max(1L,r.bottom-Dip(24)),TRUE);SyncDetailScroll();
        DrawDetailFrame();
    }
    RECT RowTextRect(RECT row) const { row.left+=Dip(6); row.right-=Dip(12); return row; }
    bool NeedsDetail(const ClipboardHistoryEntry& entry,int row) {
        RECT bounds{};
        if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&bounds))==LB_ERR) return false;
        bounds=RowTextRect(bounds);
        const auto summary=ClipboardHistoryModel::Summary(entry.text);
        // Summary also limits very large entries before the drawing ellipsis.
        if(entry.text.size()>1024 && summary.size()>=1024) return true;
        HDC dc=GetDC(list); if(!dc) return false;
        auto old=SelectObject(dc,font); RECT measured{};
        DrawTextW(dc,summary.c_str(),int(summary.size()),&measured,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,old); ReleaseDC(list,dc);
        return measured.right-measured.left>bounds.right-bounds.left;
    }
    void ShowDetail(std::shared_ptr<const ClipboardHistoryEntry> entry) {
        if(shown && shown->id==entry->id) return;
        CloseDetail(); shown=std::move(entry);
        RECT root{}; GetWindowRect(window,&root); MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);
        HDC dc=GetDC(window); auto old=SelectObject(dc,font);
        auto sample=shown->text.substr(0,8192); RECT measured{0,0,Dip(348),0};
        DrawTextW(dc,sample.c_str(),int(sample.size()),&measured,DT_CALCRECT|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX);
        const int width=std::min(std::clamp<int>(measured.right+Dip(48),Dip(120),Dip(400)),int(mi.rcWork.right-mi.rcWork.left));
        RECT lines{0,0,width-Dip(37),0}; DrawTextW(dc,sample.c_str(),int(sample.size()),&lines,DT_CALCRECT|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX);
        SelectObject(dc,old); ReleaseDC(window,dc);
        const int height=std::min(std::clamp<int>(lines.bottom+Dip(28),Dip(46),Dip(320)),int(mi.rcWork.bottom-mi.rcWork.top));
        detailOnRight=root.right+Dip(6)+width<=mi.rcWork.right;
        int x=detailOnRight?root.right+Dip(6):root.left-width-Dip(6);
        x=std::clamp<int>(x,mi.rcWork.left,mi.rcWork.right-width);
        POINT anchor{}; GetCursorPos(&anchor);
        for(int i=0;i<int(model.Entries().size());++i) if(ItemId(i)==shown->id) {
            RECT row{}; SendMessageW(list,LB_GETITEMRECT,i,reinterpret_cast<LPARAM>(&row)); anchor={0,(row.top+row.bottom)/2}; ClientToScreen(list,&anchor); break;
        }
        int y=std::clamp<int>(anchor.y-Dip(23),mi.rcWork.top,mi.rcWork.bottom-height);
        detailTip=std::clamp<int>(anchor.y-y,Dip(14),height-Dip(14));
        detail=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,L"PcTool.ClipboardDetail",L"剪切板文本详情",WS_POPUP|WS_CLIPCHILDREN,x,y,width,height,window,nullptr,GetModuleHandleW(nullptr),this);
        detailFrame=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,L"STATIC",L"PcTool.ClipboardDetailFrame",WS_POPUP,x,y,width,height,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetLayeredWindowAttributes(detail,Transparent,255,LWA_COLORKEY);
        edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,0,0,1,1,detail,reinterpret_cast<HMENU>(102),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(edit,EM_SETLIMITTEXT,0x7ffffffe,0); SetWindowTextW(edit,shown->text.c_str()); SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        detailScroll.Create(detail,105,[this](int position){ScrollDetailTo(position);},DetailPaper);
        SetWindowSubclass(edit,ChildProc,1,reinterpret_cast<DWORD_PTR>(this)); DetailLayout(); ShowWindow(detail,SW_SHOWNOACTIVATE);
    }
    void Hover() {
        SyncListScroll();SyncDetailScroll();
        if(detail&&GetCapture()==edit)return;
        POINT screen{}; GetCursorPos(&screen); HWND hit=WindowFromPoint(screen);
        uint64_t id=0;int row=-1;
        if(hit==list) { POINT p=screen; ScreenToClient(list,&p); const DWORD index=DWORD(SendMessageW(list,LB_ITEMFROMPOINT,0,MAKELPARAM(p.x,p.y))); if(!HIWORD(index)){row=LOWORD(index);id=ItemId(row);} }
        const bool moved=!haveHoverPoint||screen.x!=hoverPoint.x||screen.y!=hoverPoint.y;
        if(moved||id!=rowHover){haveHoverPoint=true;hoverPoint=screen;if(id!=rowHover){rowHover=id;SendMessageW(list,LB_SETCURSEL,row,0);InvalidateRect(list,nullptr,FALSE);}if(id&&GetFocus()!=list)SetFocus(list);}
        if(detail && (GetFocus()==edit || GetCapture()==edit)) return;
        if(detail && (hit==detail || hit==detailFrame || IsChild(detail,hit))) { leaveSince=0; return; }
        if(id==suppressedHover && id) return;
        suppressedHover=0;
        const auto now=GetTickCount64();
        if(id) {
            auto entry=Entry(id);
            if(!entry || !NeedsDetail(*entry,row)) { CloseDetail(); hoverId=id; return; }
        }
        if(id && shown && id==shown->id) { leaveSince=0; return; }
        hoverId=id;
        if(id) { if(auto entry=Entry(id)) ShowDetail(entry); leaveSince=0; }
        else if(detail) { if(!leaveSince) leaveSince=now; if(now-leaveSince>=400) CloseDetail(); }
    }
    void CopySelected() {
        auto entry=Entry(ItemId(int(SendMessageW(list,LB_GETCURSEL,0,0)))); if(!entry) return;
        CopyValue(entry->text);
    }
    void CopyValue(const std::wstring& text) {
        feedbackUntil=GetTickCount64()+1800;
        const size_t bytes=(text.size()+1)*sizeof(wchar_t); HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
        if(!memory) { SetWindowTextW(status,L"复制失败：内存不足"); return; }
        auto* data=GlobalLock(memory); if(!data) { GlobalFree(memory); return; } memcpy(data,text.c_str(),bytes); GlobalUnlock(memory);
        bool copied=false;
        if(OpenClipboard(window)) { if(EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory)) copied=true; CloseClipboard(); }
        if(!copied) GlobalFree(memory);
        SetWindowTextW(status,copied?L"已复制文本":L"剪贴板被占用，按 Ctrl+C 重试");
    }
    static LRESULT CALLBACK ChildProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<Impl*>(data);
        if(m==WM_NCDESTROY) { RemoveWindowSubclass(w,ChildProc,1); return DefSubclassProc(w,m,wp,lp); }
        if(w==self->list&&(m==WM_MOUSEMOVE||m==WM_MOUSELEAVE)){
            self->Hover();if(m==WM_MOUSEMOVE){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);}
        }
        if(m==WM_CHAR && (wp==1 || wp==3)) return 0;
        if(m==WM_KEYDOWN) {
            if(wp==VK_ESCAPE) { if(self->frameDrag){self->EndFrameDrag(true);return 0;} if(self->detail) { self->suppressedHover=self->shown?self->shown->id:0; self->CloseDetail(); SetFocus(self->list); } else self->Close(); return 0; }
            if(GetKeyState(VK_CONTROL)<0) {
                if(wp=='A' && w==self->edit) { SendMessageW(w,EM_SETSEL,0,-1); return 0; }
                if(wp=='C') {
                    if(w==self->list) self->CopySelected();
                    else if(w==self->edit) {
                        DWORD a{},b{}; SendMessageW(w,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
                        int length=GetWindowTextLengthW(w); std::wstring text(size_t(length)+1,L'\0'); GetWindowTextW(w,text.data(),length+1); text.resize(size_t(length));
                        if(a<b && b<=text.size()) self->CopyValue(text.substr(a,b-a));
                    }
                    return 0;
                }
            }
        }
        if(w==self->list&&m==WM_MOUSEWHEEL){self->WheelList(wp);return 0;}
        if(w==self->edit&&m==WM_MOUSEWHEEL){self->WheelDetail(wp);return 0;}
        const auto result=DefSubclassProc(w,m,wp,lp);
        if(w==self->list&&(m==WM_KEYDOWN||m==LB_SETTOPINDEX||m==LB_SETCURSEL||m==WM_TIMER||m==WM_VSCROLL))self->SyncListScroll();
        if(w==self->edit&&(m==WM_KEYDOWN||m==WM_MOUSEWHEEL||m==EM_LINESCROLL||m==WM_VSCROLL||m==WM_TIMER||m==WM_MOUSEMOVE))self->SyncDetailScroll();
        return result;
    }
    LRESULT Handle(HWND w,UINT m,WPARAM wp,LPARAM lp) {
        if(w==listener) {
            if(m==WM_CLIPBOARDUPDATE) { retries=0; ReadClipboard(); return 0; }
            if(m==WM_TIMER) { KillTimer(listener,1); ReadClipboard(); return 0; }
        }
        if(m==OutsideMessage) { if(!InWindows(WindowFromPoint({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}))) Close(); return 0; }
        if(m==WM_NCCALCSIZE && w==window && wp) return WVR_REDRAW;
        if(m==WM_NCHITTEST && w==window) {
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(w,&p); RECT r{}; GetClientRect(w,&r);
            bool l=p.x<Dip(5),right=p.x>=r.right-Dip(5),top=p.y<Dip(5),bottom=p.y>=r.bottom-Dip(5);
            if(top) return l?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
            if(bottom) return l?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
            if(l) return HTLEFT; if(right) return HTRIGHT;
            return p.y<Dip(HistoryListTop)?HTCAPTION:HTCLIENT;
        }
        if(w==window) {
            if(m==WM_NCLBUTTONDOWN&&(wp==HTCAPTION||(wp>=HTLEFT&&wp<=HTBOTTOMRIGHT))){CloseDetail();frameDrag=int(wp);GetWindowRect(window,&dragStart);GetCursorPos(&dragCursor);SetCapture(window);return 0;}
            if(m==WM_MOUSEMOVE&&frameDrag){MoveFrame();return 0;}
            if(m==WM_LBUTTONUP&&frameDrag){EndFrameDrag(false);return 0;}
            if(m==WM_KEYDOWN&&wp==VK_ESCAPE){if(frameDrag)EndFrameDrag(true);else Close();return 0;}
            if(m==WM_CANCELMODE){EndFrameDrag(true);return 0;}
            if(m==WM_CAPTURECHANGED){frameDrag=0;}
            if(m==WM_WINDOWPOSCHANGED||m==WM_SHOWWINDOW)shadow.Sync(window,dpi);
            if(m==WM_MOUSEWHEEL){WheelList(wp);return 0;}
        }
        if(w==detail&&m==WM_MOUSEWHEEL){WheelDetail(wp);return 0;}
        if(m==WM_ENTERSIZEMOVE && w==window) CloseDetail();
        if(m==WM_CLOSE && w==window) { Close(); return 0; }
        if(m==WM_TIMER && w==window) { Hover(); return 0; }
        if(m==WM_SIZE) { if(w==window) Layout(); else if(w==detail) DetailLayout(); return 0; }
        if(m==WM_GETMINMAXINFO && w==window) { auto* mm=reinterpret_cast<MINMAXINFO*>(lp); mm->ptMinTrackSize={Dip(320),Dip(220)}; return 0; }
        if(m==WM_DPICHANGED) {
            if(w==window) { CloseDetail(); dpi=HIWORD(wp); MakeFont(); }
            auto* r=reinterpret_cast<RECT*>(lp); SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); if(w==window) Layout(); InvalidateRect(w,nullptr,FALSE); return 0;
        }
        if(m==WM_CTLCOLORSTATIC || m==WM_CTLCOLOREDIT || m==WM_CTLCOLORLISTBOX) {
            bool dark=reinterpret_cast<HWND>(lp)==edit; SetBkColor(reinterpret_cast<HDC>(wp),dark?DetailPaper:Paper);
            SetTextColor(reinterpret_cast<HDC>(wp),dark?RGB(255,255,255):reinterpret_cast<HWND>(lp)==status?RGB(130,136,146):RGB(39,44,54)); return reinterpret_cast<LRESULT>(dark?detailBrush:paper);
        }
        if(m==WM_ERASEBKGND) return 1;
        if(m==WM_PAINT) {
            PAINTSTRUCT ps{}; HDC dc=BeginPaint(w,&ps); RECT r{}; GetClientRect(w,&r);
            HDC buffer=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom)); auto old=SelectObject(buffer,bitmap);
            Fill(buffer,r,w==detail?Transparent:Paper);
            if(w==detail) {
                RECT body=r; if(detailOnRight) body.left+=Dip(8); else body.right-=Dip(8);
                RoundFill(buffer,body,float(Dip(6)),DetailPaper);
                Gdiplus::Graphics g(buffer); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                float edge=float(detailOnRight?body.left:body.right),tip=float(detailOnRight?0:r.right);
                Gdiplus::PointF points[]={{edge,float(detailTip-Dip(7))},{tip,float(detailTip)},{edge,float(detailTip+Dip(7))}};
                Gdiplus::SolidBrush brush(Gdiplus::Color(255,102,102,102)); g.FillPolygon(&brush,points,3);
            } else if(w==window) {
                SetBkMode(buffer,TRANSPARENT); SetTextColor(buffer,RGB(31,38,50)); auto saved=SelectObject(buffer,titleFont);
                RECT title{Dip(18),Dip(8),r.right-Dip(18),Dip(35)};
                RECT titleMeasure{}; DrawTextW(buffer,L"剪切板历史",-1,&titleMeasure,DT_SINGLELINE|DT_CALCRECT);
                DrawTextW(buffer,L"剪切板历史",-1,&title,DT_SINGLELINE|DT_VCENTER);
                SelectObject(buffer,smallFont); SetTextColor(buffer,RGB(139,145,156));
                auto count=std::to_wstring(model.Entries().size())+L" 条文本 · 仅本次运行";
                RECT sub{title.left+titleMeasure.right+Dip(12),title.top,r.right-Dip(18),title.bottom};
                if(sub.right>sub.left) DrawTextW(buffer,count.c_str(),-1,&sub,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
                SelectObject(buffer,saved);
            }
            GetClientRect(w,&r); BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY); SelectObject(buffer,old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(w,&ps); return 0;
        }
        if(m==WM_DRAWITEM) {
            auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if(item->itemID==UINT(-1)) return TRUE;
            auto entry=Entry(uint64_t(item->itemData)); if(!entry) return TRUE;
            Fill(item->hDC,item->rcItem,Paper); RECT card=item->rcItem; InflateRect(&card,-Dip(2),-Dip(2));
            const bool selected=(item->itemState&ODS_SELECTED)!=0;
            RoundFill(item->hDC,card,float(Dip(8)),selected?RGB(233,241,253):rowHover==entry->id?RGB(229,229,229):RGB(237,237,237));
            if(selected) { RECT mark{card.left,card.top+Dip(7),card.left+Dip(3),card.bottom-Dip(7)}; RoundFill(item->hDC,mark,float(Dip(1)),RGB(72,126,224)); }
            RECT r=RowTextRect(item->rcItem); auto old=SelectObject(item->hDC,font); SetBkMode(item->hDC,TRANSPARENT); SetTextColor(item->hDC,RGB(96,98,102));
            auto text=ClipboardHistoryModel::Summary(entry->text); DrawTextW(item->hDC,text.c_str(),int(text.size()),&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(item->hDC,old); return TRUE;
        }
        return DefWindowProcW(w,m,wp,lp);
    }
};
ClipboardHistory::ClipboardHistory():impl_(std::make_unique<Impl>()) {}
ClipboardHistory::~ClipboardHistory()=default;
bool ClipboardHistory::Start() { return impl_->Start(); }
void ClipboardHistory::Show() { impl_->Show(); }
void ClipboardHistory::Shutdown() { impl_->Shutdown(); }
