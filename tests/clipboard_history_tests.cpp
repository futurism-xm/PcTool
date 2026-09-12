#include "clipboard/clipboard_history.h"
#include "system_tools/paint.h"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <objidl.h>
#include <gdiplus.h>
static void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static void Pump(int ms) { const auto end=GetTickCount64()+ms; do { MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(5); } while(GetTickCount64()<end); }
static void Put(const std::wstring& text) {
    Require(OpenClipboard(nullptr)!=FALSE,"open clipboard"); EmptyClipboard();
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t)); void* p=GlobalLock(h); memcpy(p,text.c_str(),(text.size()+1)*sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT,h); CloseClipboard();
}
static std::wstring Get() { Require(OpenClipboard(nullptr)!=FALSE,"read clipboard"); HGLOBAL h=GetClipboardData(CF_UNICODETEXT); const auto* p=h?static_cast<wchar_t*>(GlobalLock(h)):nullptr; std::wstring result=p?p:L""; if(p) GlobalUnlock(h); CloseClipboard(); return result; }
static void Click(HWND w,int y=15) {
    RECT r{}; GetWindowRect(w,&r); SetCursorPos(r.left+30,r.top+y);
    INPUT inputs[2]{}; inputs[0].type=inputs[1].type=INPUT_MOUSE; inputs[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; inputs[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    SendInput(2,inputs,sizeof(INPUT)); Pump(60);
}
static void Chord(WORD key) {
    INPUT inputs[4]{}; for(auto& i:inputs) i.type=INPUT_KEYBOARD;
    inputs[0].ki.wVk=inputs[3].ki.wVk=VK_CONTROL; inputs[1].ki.wVk=inputs[2].ki.wVk=key;
    inputs[2].ki.dwFlags=inputs[3].ki.dwFlags=KEYEVENTF_KEYUP;
    Require(SendInput(4,inputs,sizeof(INPUT))==4,"keyboard input"); Pump(100);
}
static void Core() {
    ClipboardHistoryModel m; Require(!m.Add(L""),"ignore empty");
    for(int i=0;i<105;++i) m.Add(std::to_wstring(i));
    Require(m.Entries().size()==100 && m.Entries().back()->text==L"5","capacity and oldest eviction");
    const auto id=m.Entries()[50]->id; auto value=m.Entries()[50]->text;
    Require(m.Add(value) && m.Entries().front()->id==id && m.Entries().size()==100,"deduplicate stable identity");
    Require(!m.Add(value),"repeat at front");
    m.Add(L" 中文\r\nMixed TEXT \t"); Require(m.Entries().front()->text==L" 中文\r\nMixed TEXT \t","preserve original");
    Require(ClipboardHistoryModel::Summary(L"a\r\nb\nc\t")==L"a b c ","one-line summary");
    m.Add(L"a"); m.Add(L"A"); m.Add(L"a "); Require(m.Entries()[2]->text==L"a","exact comparison");
    std::cout<<"CLIPBOARD CORE PASS\n";
}
struct ClipboardRestore {
    std::vector<std::pair<UINT,std::vector<BYTE>>> values;
    ClipboardRestore() { if(!OpenClipboard(nullptr)) throw std::runtime_error("clipboard backup");
        for(UINT f=EnumClipboardFormats(0);f;f=EnumClipboardFormats(f)) {
            if(f==CF_BITMAP || f==CF_PALETTE || f==CF_ENHMETAFILE || f==CF_METAFILEPICT) continue;
            HANDLE h=GetClipboardData(f); SIZE_T size=h?GlobalSize(h):0; if(!size) continue;
            auto* p=static_cast<BYTE*>(GlobalLock(h)); if(p) { values.push_back({f,{p,p+size}}); GlobalUnlock(h); }
        } CloseClipboard();
    }
    ~ClipboardRestore() { if(!OpenClipboard(nullptr)) return; EmptyClipboard(); for(auto& v:values) { HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,v.second.size()); void* p=GlobalLock(h); if(!p) { GlobalFree(h); continue; } memcpy(p,v.second.data(),v.second.size()); GlobalUnlock(h); if(!SetClipboardData(v.first,h)) GlobalFree(h); } CloseClipboard(); }
};
static std::vector<DWORD> Snapshot(HWND w,const std::wstring& path) {
    RECT r{}; GetWindowRect(w,&r); HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen); HBITMAP bitmap=CreateCompatibleBitmap(screen,r.right-r.left,r.bottom-r.top); auto old=SelectObject(dc,bitmap);
    BitBlt(dc,0,0,r.right-r.left,r.bottom-r.top,screen,r.left,r.top,SRCCOPY|CAPTUREBLT); SelectObject(dc,old); DeleteDC(dc); ReleaseDC(nullptr,screen);
    Gdiplus::Bitmap image(bitmap,nullptr); UINT count{},bytes{}; Gdiplus::GetImageEncodersSize(&count,&bytes); std::vector<BYTE> data(bytes); auto* encoders=reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data()); Gdiplus::GetImageEncoders(count,bytes,encoders);
    for(UINT i=0;i<count;++i) if(wcscmp(encoders[i].MimeType,L"image/png")==0) image.Save(path.c_str(),&encoders[i].Clsid,nullptr);
    BITMAPINFO info{}; info.bmiHeader={sizeof(BITMAPINFOHEADER),r.right-r.left,-(r.bottom-r.top),1,32,BI_RGB};
    std::vector<DWORD> pixels(size_t(r.right-r.left)*(r.bottom-r.top)); HDC read=GetDC(nullptr);
    GetDIBits(read,bitmap,0,r.bottom-r.top,pixels.data(),&info,DIB_RGB_COLORS); ReleaseDC(nullptr,read); DeleteObject(bitmap);
    return pixels;
}
static void Ui() {
    ClipboardRestore restore; Put(L"initial");
    ClipboardHistory history; Require(history.Start(),"listener start"); history.Show(); Pump(100);
    HWND window=FindWindowW(L"PcTool.ClipboardHistory",nullptr),list=GetDlgItem(window,101);
    SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    Require(list && SendMessageW(list,LB_GETCOUNT,0,0)==1,"initial clipboard snapshot");
    Require(!GetDlgItem(window,103),"close button still exists");
    Require(!(GetWindowLongPtrW(window,GWL_STYLE)&WS_THICKFRAME),"native popup frame remains");
    auto centered=[&]{POINT cursor{};GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);RECT bounds{};GetWindowRect(window,&bounds);
        Require(abs(bounds.left+bounds.right-mi.rcWork.left-mi.rcWork.right)<=1&&abs(bounds.top+bounds.bottom-mi.rcWork.top-mi.rcWork.bottom)<=1,"popup not centered in cursor monitor work area");};
    centered();
    SetWindowPos(window,nullptr,30,30,0,0,SWP_NOSIZE|SWP_NOZORDER);history.Show();Pump(80);centered();
    HWND shadow=FindWindowW(L"PcTool.ClipboardShadow",nullptr);Require(shadow&&IsWindowVisible(shadow),"four-sided shadow missing");
    Require((GetWindowLongPtrW(shadow,GWL_EXSTYLE)&(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE))==(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE),"shadow blocks input or takes focus");
    Require(GetWindow(shadow,GW_OWNER)==window,"shadow owner mismatch");
    Require(!IsWindowVisible(GetDlgItem(window,104)),"short list scrollbar should be hidden");
    Require(OpenClipboard(nullptr)!=FALSE,"non-text open"); EmptyClipboard(); CloseClipboard(); Pump(100);
    Require(SendMessageW(list,LB_GETCOUNT,0,0)==1,"ignore non-text clipboard");
    Put(L"second"); Pump(100); Require(SendMessageW(list,LB_GETCOUNT,0,0)==2,"clipboard listener");
    Require(OpenClipboard(nullptr)!=FALSE,"lock"); EmptyClipboard(); Pump(100); CloseClipboard(); Put(L"after lock"); Require(OpenClipboard(nullptr)!=FALSE,"hold pending text"); Pump(150); CloseClipboard(); Pump(150);
    const auto retryDeadline=GetTickCount64()+1500; while(SendMessageW(list,LB_GETCOUNT,0,0)!=3 && GetTickCount64()<retryDeadline) Pump(30);
    Require(SendMessageW(list,LB_GETCOUNT,0,0)==3,"retry clipboard");
    Put(L"initial"); Pump(100); Require(SendMessageW(list,LB_GETCOUNT,0,0)==3,"dedupe listener");
    RECT hoverBounds{};GetWindowRect(list,&hoverBounds);SetCursorPos(hoverBounds.left+30,hoverBounds.top-15);SendMessageW(list,WM_MOUSELEAVE,0,0);SetCursorPos(hoverBounds.left+30,hoverBounds.top+51);
    SendMessageW(list,WM_MOUSEMOVE,0,MAKELPARAM(30,51));
    Require(SendMessageW(list,LB_GETCURSEL,0,0)==1,"hover did not select row immediately");
    Require(!FindWindowW(L"PcTool.ClipboardDetail",nullptr),"fully visible short row opened detail");
    Require(GetFocus()==list,"hover copy target is not list");auto selected=SendMessageW(list,LB_GETITEMDATA,1,0); Chord('C');
    std::cout<<"foreground="<<(GetForegroundWindow()==window)<<" focus="<<(GetFocus()==list)<<" status="<<GetWindowTextLengthW(GetDlgItem(window,0))<<" copied="<<(Get()==L"after lock")<<" index="<<SendMessageW(list,LB_GETCURSEL,0,0)<<" oldId="<<selected<<" rowId="<<SendMessageW(list,LB_GETITEMDATA,1,0)<<"\n";
    Require(Get()==L"after lock"&&SendMessageW(list,LB_GETCURSEL,0,0)==1&&SendMessageW(list,LB_GETITEMDATA,1,0)!=selected,"copy reorder did not keep selection under stationary pointer");
    POINT fixedCursor{};GetCursorPos(&fixedCursor);
    for(int repeat=0;repeat<6;++repeat){
        auto expected=repeat%2?L"after lock":L"initial";
        Require(!FindWindowW(L"PcTool.ClipboardDetail",nullptr),"reordered short row opened detail");
        Chord('C');POINT cursor{};GetCursorPos(&cursor);
        Require(Get()==expected&&SendMessageW(list,LB_GETCURSEL,0,0)==1&&cursor.x==fixedCursor.x&&cursor.y==fixedCursor.y,"repeated copy no longer follows stationary second row");
    }
    GetWindowRect(window,&hoverBounds);SetCursorPos(hoverBounds.left+30,hoverBounds.bottom-20);SendMessageW(list,WM_MOUSELEAVE,0,0);
    Require(SendMessageW(list,LB_GETCURSEL,0,0)==LB_ERR,"leaving row did not clear selection");
    std::wstring text=L"全文选中测试\r\n"; for(int i=0;i<250;++i) text+=L"中文与 English 0123456789，长文本滚动验证。\r\n";
    Put(text); Pump(100);
    for(UINT dpi:{96u,144u,192u}) {
        RECT r{}; GetWindowRect(window,&r); SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));
        Require(!(GetWindowLongPtrW(list,GWL_STYLE)&WS_VSCROLL),"native list scrollbar remains");
        RECT scrollRect{};GetWindowRect(GetDlgItem(window,104),&scrollRect);Require(scrollRect.right-scrollRect.left==MulDiv(6,dpi,96),"list scrollbar width differs from translation");
        HRGN region=CreateRectRgn(0,0,0,0);Require(GetWindowRgn(window,region)!=ERROR&&!PtInRegion(region,0,0),"popup corners not rounded");DeleteObject(region);
        // Resize in both directions without explicitly invalidating the window.
        // Its result must already match a clean full repaint.
        SetCursorPos(1,1);
        RECT original{}; GetWindowRect(window,&original);
        SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
        for(int step=0;step<12;++step) {
            const int extra=(step%6)*7;
            SetWindowPos(window,nullptr,0,0,original.right-original.left+extra,original.bottom-original.top+extra,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE); Pump(20);
        }
        SetWindowPos(window,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SendMessageW(window,WM_EXITSIZEMOVE,0,0); Pump(250);
        auto resized=Snapshot(window,L"clipboard-resized-"+std::to_wstring(dpi)+L".png");
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW); Pump(80);
        auto repainted=Snapshot(window,L"clipboard-repainted-"+std::to_wstring(dpi)+L".png");
        Require(resized==repainted,"Resize left stale pixels before forced repaint");
        SendMessageW(list,LB_SETTOPINDEX,0,0); GetWindowRect(list,&r); SetCursorPos(r.left+50,r.top+MulDiv(17,dpi,96)); Pump(500);
        HWND detail=FindWindowW(L"PcTool.ClipboardDetail",nullptr),edit=GetDlgItem(detail,102);
        if(!detail || !edit) { POINT p{}; GetCursorPos(&p); std::cout<<"hover hit="<<WindowFromPoint(p)<<" list="<<list<<" root="<<IsWindow(window)<<" count="<<SendMessageW(list,LB_GETCOUNT,0,0)<<"\n"; }
        Require(detail && edit,"hover detail");
        HWND frame=FindWindowW(L"STATIC",L"PcTool.ClipboardDetailFrame");Require(frame&&IsWindowVisible(frame),"alpha detail frame missing");
        Require((GetWindowLongPtrW(frame,GWL_EXSTYLE)&(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT))==(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT),"detail frame steals input");
        Require(!(GetWindowLongPtrW(edit,GWL_STYLE)&WS_VSCROLL),"native detail scrollbar remains");
        HWND detailBar=GetDlgItem(detail,105);Require(detailBar&&IsWindowVisible(detailBar),"long detail scrollbar missing");
        RECT bubble{},track{},parent{};GetWindowRect(detail,&bubble);GetWindowRect(detailBar,&track);GetWindowRect(window,&parent);
        const bool rightSide=bubble.left>=parent.right;
        Require(bubble.right-track.right==MulDiv(rightSide?3:11,dpi,96),"detail scrollbar is not 3 DIP from body edge");
        RECT detailTrack{};GetClientRect(detailBar,&detailTrack);
        SendMessageW(detailBar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));SendMessageW(detailBar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(2,detailTrack.bottom-2));SendMessageW(detailBar,WM_LBUTTONUP,0,0);
        Require(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)>0,"detail thumb does not scroll");
        SendMessageW(edit,WM_VSCROLL,SB_TOP,0);
        GetWindowRect(edit,&r); SetCursorPos(r.left+30,r.top+30); Pump(500); Require(IsWindow(detail),"hover transition keeps detail");
        const auto focused=GetFocus();const auto listTop=SendMessageW(list,LB_GETTOPINDEX,0,0);
        SendMessageW(edit,EM_SETSEL,1,4);
        SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(r.left+30,r.top+30));
        const auto afterDown=SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0);Require(afterDown>0,"detail wheel down did not scroll text");
        Require(SendMessageW(detailBar,SBM_GETPOS,0,0)==afterDown,"detail wheel thumb out of sync");
        SendMessageW(detailBar,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(track.left+2,track.top+20));
        Require(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)<afterDown,"detail scrollbar wheel up did not scroll text");
        SendMessageW(detail,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),0);
        Require(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)>0,"detail padding wheel did not scroll text");
        DWORD selA{},selB{};SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&selA),reinterpret_cast<LPARAM>(&selB));
        Require(selA==1&&selB==4&&GetFocus()==focused&&SendMessageW(list,LB_GETTOPINDEX,0,0)==listTop,"detail wheel changed selection/focus or scrolled list");
        const auto beforeNative=SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0);INPUT wheel{};wheel.type=INPUT_MOUSE;wheel.mi.dwFlags=MOUSEEVENTF_WHEEL;wheel.mi.mouseData=DWORD(-WHEEL_DELTA);SendInput(1,&wheel,sizeof(wheel));Pump(80);
        Require(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)>beforeNative,"native hover wheel did not scroll detail");
        Snapshot(window,L"clipboard-main-"+std::to_wstring(dpi)+L".png");
        Snapshot(shadow,L"clipboard-shadow-"+std::to_wstring(dpi)+L".png");
        Snapshot(detail,L"clipboard-detail-"+std::to_wstring(dpi)+L".png");
        Click(edit); Chord('A'); DWORD a{},b{}; SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
        Require(a==0 && b==text.size(),"detail Ctrl+A"); Chord('C'); Require(Get()==text,"detail full copy");
        SendMessageW(edit,EM_SETSEL,0,4); Chord('C'); Require(Get()==text.substr(0,4),"detail selection copy");
        Require(GetWindowTextLengthW(edit)==int(text.size()),"detail remains pinned after clipboard update");
        SendMessageW(edit,WM_VSCROLL,SB_BOTTOM,0); Require(SendMessageW(edit,EM_GETFIRSTVISIBLELINE,0,0)>0,"detail scroll");
        SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0); Require(!IsWindow(detail) && IsWindow(window),"detail escape");
        Require(!IsWindow(frame),"alpha frame survives closing detail");
        Put(text); Pump(100);
    }
    // Overflow uses the shared scrollbar, including thumb, wheel and keyboard sync.
    for(int i=0;i<35;++i){Put(L"滚动条测试 "+std::to_wstring(i));Pump(10);}
    SendMessageW(list,LB_SETTOPINDEX,0,0);Pump(80);
    HWND listBar=GetDlgItem(window,104);Require(IsWindowVisible(listBar),"overflow list scrollbar missing");
    RECT track{};GetClientRect(listBar,&track);
    SendMessageW(listBar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));SendMessageW(listBar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(2,track.bottom-2));SendMessageW(listBar,WM_LBUTTONUP,0,0);
    const auto bottom=SendMessageW(list,LB_GETTOPINDEX,0,0);Require(bottom>20,"list thumb did not reach bottom");
    SendMessageW(listBar,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);Require(SendMessageW(list,LB_GETTOPINDEX,0,0)<bottom,"wheel over scrollbar does not scroll list");
    SetFocus(list);SendMessageW(list,WM_KEYDOWN,VK_HOME,0);Require(SendMessageW(list,LB_GETTOPINDEX,0,0)==0,"Home did not scroll to beginning");
    SendMessageW(list,LB_SETTOPINDEX,1,0);SendMessageW(list,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);Require(SendMessageW(list,LB_GETTOPINDEX,0,0)==0,"wheel near top did not clamp to beginning");
    SendMessageW(list,WM_KEYDOWN,VK_END,0);Require(SendMessageW(listBar,SBM_GETPOS,0,0)==SendMessageW(list,LB_GETTOPINDEX,0,0),"keyboard scroll and thumb out of sync");
    Pump(80);Snapshot(window,L"clipboard-overflow.png");
    SendMessageW(list,LB_SETTOPINDEX,0,0);
    // Direct mouse dragging retains custom window move/resize behavior.
    RECT start{};GetWindowRect(window,&start);POINT cursor{};GetCursorPos(&cursor);
    SendMessageW(window,WM_NCLBUTTONDOWN,HTCAPTION,0);SetCursorPos(cursor.x+30,cursor.y+20);SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,0);SendMessageW(window,WM_LBUTTONUP,0,0);
    RECT moved{};GetWindowRect(window,&moved);Require(moved.left!=start.left||moved.top!=start.top,"frameless popup cannot move");
    GetCursorPos(&cursor);SendMessageW(window,WM_NCLBUTTONDOWN,HTBOTTOMRIGHT,0);SetCursorPos(cursor.x+40,cursor.y+30);SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,0);SendMessageW(window,WM_LBUTTONUP,0,0);
    RECT resized{};GetWindowRect(window,&resized);Require(resized.right-resized.left>moved.right-moved.left,"frameless popup cannot resize");
    history.Show();Pump(80);centered();
    // Only ellipsized rows need a detail window; selection/copy remain immediate.
    Put(L"简短内容"); Pump(100); SendMessageW(list,LB_SETTOPINDEX,0,0);
    RECT shortRow{}; GetWindowRect(list,&shortRow); SetCursorPos(shortRow.left+40,shortRow.top+34); Pump(500);
    Require(!FindWindowW(L"PcTool.ClipboardDetail",nullptr),"short row should not open detail");
    Require(SendMessageW(list,LB_GETCURSEL,0,0)==0,"short row no longer selected on hover");
    std::wstring clipped;
    HDC measure=GetDC(list);auto oldFont=SelectObject(measure,reinterpret_cast<HFONT>(SendMessageW(list,WM_GETFONT,0,0)));
    RECT rowBounds{};SendMessageW(list,LB_GETITEMRECT,0,reinterpret_cast<LPARAM>(&rowBounds));
    SIZE extent{};do {clipped+=L"W";GetTextExtentPoint32W(measure,clipped.c_str(),int(clipped.size()),&extent);}while(extent.cx<=rowBounds.right-rowBounds.left-MulDiv(18,192,96));
    SelectObject(measure,oldFont);ReleaseDC(list,measure);
    Put(clipped);Pump(100);SendMessageW(list,LB_SETTOPINDEX,0,0);SendMessageW(list,WM_MOUSEMOVE,0,MAKELPARAM(40,34));
    HWND shortDetail=FindWindowW(L"PcTool.ClipboardDetail",nullptr); Require(shortDetail!=nullptr,"clipped one-line row needs immediate detail");
    RECT shortBounds{}; GetWindowRect(shortDetail,&shortBounds);
    Require(shortBounds.bottom-shortBounds.top<=320,"short tooltip should be compact at 200% DPI");
    Require(!(GetWindowLongPtrW(GetDlgItem(shortDetail,102),GWL_STYLE)&WS_VSCROLL),"short tooltip has unnecessary scrollbar");
    Require(!IsWindowVisible(GetDlgItem(shortDetail,105)),"short tooltip custom scrollbar should be hidden");
    Require(!(GetClassLongPtrW(shortDetail,GCL_STYLE)&CS_DROPSHADOW),"tooltip has rectangular system shadow");
    HRGN silhouette=CreateRectRgn(0,0,0,0); Require(GetWindowRgn(shortDetail,silhouette)!=ERROR,"tooltip silhouette missing");
    const int width=shortBounds.right-shortBounds.left;
    Require(!PtInRegion(silhouette,1,1) && !PtInRegion(silhouette,width-1,1),"transparent corners are rectangular");
    Require(PtInRegion(silhouette,width/2,20),"tooltip body clipped"); DeleteObject(silhouette);
    Snapshot(shortDetail,L"clipboard-short.png");
    RECT screenBounds{}; UnionRect(&screenBounds,&shortBounds,&shortRow); RECT mainBounds{}; GetWindowRect(window,&mainBounds); UnionRect(&screenBounds,&screenBounds,&mainBounds);
    // Move to another row and confirm both the content and vertical anchor follow.
    SetCursorPos(shortRow.left+40,shortRow.top+102); Pump(550);
    Require(!FindWindowW(L"PcTool.ClipboardDetail",nullptr),"moving to fully visible row must close previous detail");
    SetCursorPos(shortRow.left+40,shortRow.top+34);SendMessageW(list,WM_MOUSEMOVE,0,MAKELPARAM(40,34));
    Require(FindWindowW(L"PcTool.ClipboardDetail",nullptr)!=nullptr,"returning to clipped row did not reopen detail");
    RECT originalWidth{};GetWindowRect(window,&originalWidth);
    SetWindowPos(window,nullptr,0,0,originalWidth.right-originalWidth.left+200,originalWidth.bottom-originalWidth.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);Pump(100);
    Require(!FindWindowW(L"PcTool.ClipboardDetail",nullptr),"widened fully visible row retained detail");
    SetWindowPos(window,nullptr,0,0,originalWidth.right-originalWidth.left,originalWidth.bottom-originalWidth.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);Pump(100);
    Require(FindWindowW(L"PcTool.ClipboardDetail",nullptr)!=nullptr,"narrowed clipped row did not reopen detail");
        RECT outsideBounds{}; GetWindowRect(window,&outsideBounds);
    SetCursorPos(outsideBounds.left>100?10:GetSystemMetrics(SM_CXSCREEN)-10,10); INPUT mouse[2]{}; for(auto& i:mouse) i.type=INPUT_MOUSE; mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    SendInput(2,mouse,sizeof(INPUT)); Pump(300); if(IsWindow(window)) { POINT p{};GetCursorPos(&p);std::cout<<"outside="<<p.x<<","<<p.y<<" hit="<<WindowFromPoint(p)<<" root="<<window<<" bounds="<<outsideBounds.left<<","<<outsideBounds.top<<","<<outsideBounds.right<<","<<outsideBounds.bottom<<"\n"; } Require(!IsWindow(window),"outside click closes");
    history.Show(); Pump(50); window=FindWindowW(L"PcTool.ClipboardHistory",nullptr); list=GetDlgItem(window,101);
    centered();Require(IsWindowVisible(FindWindowW(L"PcTool.ClipboardShadow",nullptr)),"shadow missing after close and reopen");
    SendMessageW(list,WM_KEYDOWN,VK_ESCAPE,0); Require(!IsWindow(window),"list escape");
    history.Shutdown(); Put(L"restart only"); Require(history.Start(),"restart"); history.Show(); Pump(50); window=FindWindowW(L"PcTool.ClipboardHistory",nullptr);
    Require(SendMessageW(GetDlgItem(window,101),LB_GETCOUNT,0,0)==1,"shutdown clears history"); history.Shutdown();
    std::cout<<"CLIPBOARD UI PASS: listener, retry, dedupe, copy, detail selection/scroll, DPI, outside click, shutdown\n";
}
int main(int argc,char** argv) { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); InitCommonControls(); try { if(argc>1 && std::string(argv[1])=="paint-test") { LaunchSystemPaint(nullptr); return 0; } if(argc>1) Ui(); else Core(); return 0; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
