#include "capture/screenshot_overlay.h"
#include "shared/platform/capture_platform.h"
#include "shared/annotation/annotation_style.h"
#include <dwmapi.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
void CheckCrossProcessClick(POINT point,HWND overlay);

namespace {
void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void Pump(int ms=20) {
    auto until=GetTickCount64()+ms;
    do { MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&m);DispatchMessageW(&m);} Sleep(5); } while(GetTickCount64()<until);
}
POINT Center(RECT r) { return {(r.left+r.right)/2,(r.top+r.bottom)/2}; }
void Mouse(HWND w,UINT message,POINT p) { SendMessageW(w,message,message==WM_LBUTTONUP?0:MK_LBUTTON,MAKELPARAM(p.x,p.y)); }
void Click(HWND w,POINT p) { Mouse(w,WM_LBUTTONDOWN,p);Mouse(w,WM_LBUTTONUP,p); }
void Drag(HWND w,POINT a,POINT b,int steps=4) {
    Mouse(w,WM_LBUTTONDOWN,a);
    for(int i=1;i<=steps;++i) Mouse(w,WM_MOUSEMOVE,{a.x+(b.x-a.x)*i/steps,a.y+(b.y-a.y)*i/steps});
    Mouse(w,WM_LBUTTONUP,b);
}
COLORREF backgroundColor=RGB(20,80,130);
LRESULT CALLBACK Background(HWND w,UINT m,WPARAM wp,LPARAM lp) {
    if(m==WM_PAINT) { PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);
        HBRUSH brush=CreateSolidBrush(backgroundColor);FillRect(dc,&r,brush);DeleteObject(brush);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        for(int y=0;y<r.bottom;y+=80) for(int x=0;x<r.right;x+=100) { auto s=std::to_wstring(x)+L","+std::to_wstring(y);TextOutW(dc,x,y,s.c_str(),int(s.size())); }
        EndPaint(w,&ps);return 0; }
    return DefWindowProcW(w,m,wp,lp);
}
capture::Image Desktop(RECT r) {
    capture::Image result(r.right-r.left,r.bottom-r.top); HBITMAP b=capture::ToBitmap(result);
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);auto old=SelectObject(dc,b);
    BitBlt(dc,0,0,result.width,result.height,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);
    SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);result=capture::FromBitmap(b);DeleteObject(b);return result;
}
}

#include <shellapi.h>
// Friend access observes model/geometry while input follows native window messages.
struct ScreenshotInteractionTests {
    using Command=ScreenshotOverlay::ToolbarCommand;
    using Tool=capture::Tool;
    ScreenshotOverlay overlay{GetModuleHandleW(nullptr)};
    std::vector<HWND> pins;
    HWND background{}; RECT work{}; std::wstring prefix; std::ofstream report;
    ~ScreenshotInteractionTests() { overlay.Cancel(); for(auto w:pins) if(IsWindow(w)) DestroyWindow(w);if(background) DestroyWindow(background); }
    void Pass(const std::string& id) { report<<id<<" PASS\n";report.flush();std::cout<<id<<" PASS\n"; }
    void WindowSnap() {
        HWND target=CreateWindowExW(WS_EX_APPWINDOW,L"STATIC",L"Synthetic settings",WS_POPUP|WS_VISIBLE,120,120,480,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        HWND helper=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,L"STATIC",L"Synthetic shadow",WS_POPUP|WS_VISIBLE,110,110,500,380,target,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetWindowPos(target,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        SetWindowPos(helper,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        Require(overlay.Start(),"Snap capture start failed");Pump();
        RECT expected{},actual{};GetWindowRect(target,&expected);
        const bool matched=overlay.FindWindowAtScreenPoint({240,240},actual)&&EqualRect(&expected,&actual);
        overlay.selection_={expected.left-overlay.virtualLeft_+10,expected.top-overlay.virtualTop_+10,expected.right-overlay.virtualLeft_-10,expected.bottom-overlay.virtualTop_-10};
        const RECT selection=overlay.selection_;
        Require(!overlay.IsToolbarCommandEnabled(Command::Clear),"Empty clear should be disabled");
        overlay.annotations_.resize(2);
        overlay.movingAnnotationIndex_=1;
        Require(overlay.IsToolbarCommandEnabled(Command::Clear),"Clear unavailable with annotations");
        const auto buttons=overlay.ToolbarButtons();
        auto undo=std::find_if(buttons.begin(),buttons.end(),[](const auto& b){return b.command==Command::Undo;});
        Require(undo!=buttons.end()&&std::next(undo)!=buttons.end()&&std::next(undo)->command==Command::Clear,"Clear position");
        overlay.ExecuteToolbarCommand(Command::Clear);
        Require(overlay.annotations_.empty()&&overlay.movingAnnotationIndex_==-1&&EqualRect(&selection,&overlay.selection_),"Clear changed selection or retained annotations");
        Require(!overlay.IsToolbarCommandEnabled(Command::Clear),"Clear remained enabled");
        Require(overlay.CopySelectionToClipboard(),"Screenshot file copy failed");
        Require(OpenClipboard(target)!=FALSE,"Clipboard inaccessible");
        auto drop=static_cast<HDROP>(GetClipboardData(CF_HDROP));wchar_t copied[32768]{};
        const bool fileCopied=drop&&DragQueryFileW(drop,0,copied,32768)>0&&GetClipboardData(CF_BITMAP)!=nullptr;
        CloseClipboard();Require(fileCopied&&std::filesystem::exists(copied),"Screenshot did not expose file and image formats");
        overlay.Cancel();DestroyWindow(helper);DestroyWindow(target);
        Require(matched,"Own app window skipped or capture/shadow selected");
        std::cout<<"PASS own application window snap, excluding overlay and shadow\n";
    }
    void Dpi(UINT dpi) { overlay.dpi_=dpi;if(overlay.toolbarFont_)DeleteObject(overlay.toolbarFont_);overlay.toolbarFont_=capture::CreateToolbarFont(dpi);overlay.UpdateToolbarPosition(true); }
    void CommandClick(Command c) {
        for(const auto& b:overlay.ToolbarButtons()) if(b.command==c) { Click(overlay.window_,Center(b.rect));return; }
        throw std::runtime_error("Toolbar command missing");
    }
    void NoTool() { if(overlay.textEditor_) overlay.CommitTextEditing(true);overlay.activeTool_=Tool::None;overlay.UpdateSettingsPanelPosition();overlay.UpdatePinnedEditingRegion(); }
    void Region() {
        if(!overlay.editingPinned_ || overlay.interaction_==ScreenshotOverlay::Interaction::MovingPinned) return;
        HRGN actual=CreateRectRgn(0,0,0,0),expected=CreateRectRgnIndirect(&overlay.selection_);
        auto add=[&](RECT r){HRGN part=CreateRectRgnIndirect(&r);CombineRgn(expected,expected,part,RGN_OR);DeleteObject(part);};
        if(overlay.ToolbarVisible()) add(overlay.toolbar_);
        if(overlay.SettingsPanelVisible()) { add(overlay.settingsPanel_);if(overlay.fontSizeMenuOpen_) add(overlay.FontSizePopupRect()); }
        const int type=GetWindowRgn(overlay.window_,actual);const bool equal=EqualRgn(actual,expected)!=FALSE;
        DeleteObject(actual);DeleteObject(expected);Require(type!=ERROR && equal,"Pin editor region includes stale background or misses controls");
    }
    capture::Image Output() { auto b=overlay.CreateOutputBitmap();Require(b!=nullptr,"Output bitmap missing");auto image=capture::FromBitmap(b);DeleteObject(b);return image; }
    void Snapshot(const std::wstring& suffix) { UpdateWindow(overlay.window_);DwmFlush();capture::SavePng(Desktop(work),prefix+suffix+L".png"); }
    void Begin(UINT dpi) {
        Require(overlay.Start(),"Screenshot did not start");
        // Simulate layout DPI explicitly; this is not a physical mixed-DPI test.
        overlay.dpi_=dpi; if(overlay.toolbarFont_) DeleteObject(overlay.toolbarFont_);overlay.toolbarFont_=capture::CreateToolbarFont(dpi);
        POINT a{work.left-overlay.virtualLeft_+100,work.top-overlay.virtualTop_+100};
        Drag(overlay.window_,a,{a.x+640,a.y+400});Require(overlay.selectionCommitted_,"Selection missing");
    }
    HWND Pin() { CommandClick(Command::Pin);Require(!overlay.IsActive(),"Pin command retained screenshot");
        HWND pin=overlay.pinnedAnnotationRecords_.back().window;pins.push_back(pin);Require(overlay.StartPinnedEditing(pin),"Pin editing failed");return pin; }
    void DrawTools(const std::string& id) {
        const Command commands[]={Command::Rectangle,Command::Ellipse,Command::Arrow,Command::Pen,Command::Mosaic,Command::Text,Command::Number};
        const Tool tools[]={Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Mosaic,Tool::Text,Tool::Number};
        for(int n=0;n<7;++n) {
            NoTool();CommandClick(commands[n]);const size_t count=overlay.annotations_.size();
            if(n<4) {
                auto layout=capture::MakeStylePanelLayout({overlay.settingsPanel_.left,overlay.settingsPanel_.top},overlay.dpi_,false,false);
                Click(overlay.window_,Center(layout.colors[n]));
                Require(overlay.activeColor_==capture::AnnotationColors[n],"Palette selection lost color");
                Click(overlay.window_,{overlay.settingsPanel_.left+overlay.Scale(24+32*(n%3)),overlay.settingsPanel_.top+overlay.Scale(24)});
                Require(overlay.activeSizeIndex_==n%3,"Stroke size selection failed");
            }
            POINT a{overlay.selection_.left+30+n*72,overlay.selection_.top+60+(n%2)*120};POINT b{a.x+44,a.y+44};
            Drag(overlay.window_,a,b);
            if(overlay.textEditor_) {
                SendMessageW(overlay.textEditor_,WM_IME_STARTCOMPOSITION,0,0);
                SetWindowTextW(overlay.textEditor_,n==5?L"中文测试\r\n第二行":L"序号");
                SendMessageW(overlay.textEditor_,WM_IME_ENDCOMPOSITION,0,0);
                overlay.CommitTextEditing(true);
            }
            Require(overlay.annotations_.size()==count+1 && overlay.annotations_.back().tool==tools[n],"Annotation creation lost content");
            auto before=overlay.annotations_.back();NoTool();
            if(tools[n]!=Tool::Mosaic) {
                RECT bounds=overlay.AnnotationBounds(before); POINT hit{};bool found=false;
                for(int y=bounds.top;y<=bounds.bottom && !found;y+=2) for(int x=bounds.left;x<=bounds.right;x+=2) {
                    POINT p{x+overlay.selection_.left,y+overlay.selection_.top};
                    if(overlay.HitTestAnnotation(p)==int(count)) {hit=p;found=true;break;}
                }
                Require(found,"Annotation cannot be hit");
                Drag(overlay.window_,hit,{hit.x+13,hit.y+9},5);
                Require(overlay.annotations_.size()==count+1,"Annotation drag duplicated content");
                Require(overlay.annotations_.back().start.x!=before.start.x || overlay.annotations_.back().start.y!=before.start.y,"Annotation drag did not move");
                // Hold capture through an out-of-selection excursion, then return.
                POINT movedHit{hit.x+13,hit.y+9};
                Mouse(overlay.window_,WM_LBUTTONDOWN,movedHit);
                Require(overlay.interaction_==ScreenshotOverlay::Interaction::MovingAnnotation,"Moved annotation lost hit target");
                Mouse(overlay.window_,WM_MOUSEMOVE,{overlay.selection_.right+300,overlay.selection_.bottom+300});
                Mouse(overlay.window_,WM_MOUSEMOVE,movedHit);Mouse(overlay.window_,WM_LBUTTONUP,movedHit);
                Require(overlay.annotations_.size()==count+1,"Outside drag duplicated annotation");
            }
            Region(); Pass(id+"-tool-"+std::to_string(n));
        }
        NoTool();auto full=Output();if(full.width!=640 || full.height!=400) throw std::runtime_error("Export size changed: "+std::to_string(full.width)+"x"+std::to_string(full.height));
        capture::SavePng(full,prefix+L"-annotations.png");
        size_t count=overlay.annotations_.size();CommandClick(Command::Undo);Require(overlay.annotations_.size()==count-1,"Undo did not remove last item");
        NoTool();CommandClick(Command::Rectangle);POINT p{overlay.selection_.left+30,overlay.selection_.top+310};
        count=overlay.annotations_.size();Click(overlay.window_,p);Require(overlay.annotations_.size()==count,"Invalid rectangle was committed");
        Mouse(overlay.window_,WM_LBUTTONDOWN,p);Mouse(overlay.window_,WM_MOUSEMOVE,{p.x+80,p.y+40});
        SendMessageW(overlay.window_,WM_KEYDOWN,VK_ESCAPE,0);Require(overlay.annotations_.size()==count && GetCapture()!=overlay.window_,"Interrupted drawing leaked content/capture");
        NoTool();Pass(id+"-invalid-undo-cancel-export");
    }
    void ToolbarMatrix(UINT dpi,int cycles=30) {
        Dpi(dpi);
        auto original=Output();
        for(int i=0;i<cycles;++i) {
            NoTool(); if(i%3==1) CommandClick(Command::Rectangle);
            if(i%3==2) { CommandClick(Command::Text);Click(overlay.window_,Center(overlay.FontSizeComboRect()));Require(overlay.fontSizeMenuOpen_,"Font menu did not open"); }
            POINT a{overlay.toolbar_.left+overlay.Scale(2),overlay.toolbar_.top+overlay.Scale(2)};
            // Width is clamped by the product; destinations alternate inside/outside/edges.
            POINT b{i%2?overlay.selection_.left:overlay.virtualWidth_-20, i%4==0?20:i%4==1?overlay.selection_.top+220:overlay.virtualHeight_-120};
            Mouse(overlay.window_,WM_LBUTTONDOWN,a);Require(overlay.interaction_==ScreenshotOverlay::Interaction::MovingToolbar,"Toolbar gesture not started");Mouse(overlay.window_,WM_MOUSEMOVE,{a.x+1,a.y+1});Region();
            if(i==0) Snapshot(L"-drag-start");
            for(int step=1;step<=5;++step) { Mouse(overlay.window_,WM_MOUSEMOVE,{a.x+(b.x-a.x)*step/5,a.y+(b.y-a.y)*step/5});Region(); }
            if(!overlay.toolbarDragStarted_) throw std::runtime_error("Toolbar never exceeded drag threshold at cycle "+std::to_string(i));
            if(i==0) {
                Snapshot(L"-drag-mid");
                // Change the uncovered background after the editor's frozen capture.
                backgroundColor=RGB(145,35,75);InvalidateRect(background,nullptr,FALSE);UpdateWindow(background);DwmFlush();
                POINT exposed{work.left-overlay.virtualLeft_+40,work.top-overlay.virtualTop_+40};
                Require(!PtInRect(&overlay.selection_,exposed) && !PtInRect(&overlay.toolbar_,exposed),"Background probe intersects content");
                HDC screen=GetDC(nullptr);COLORREF actual=GetPixel(screen,exposed.x+overlay.virtualLeft_,exposed.y+overlay.virtualTop_);ReleaseDC(nullptr,screen);
                Require(actual==backgroundColor,"Toolbar drag exposes stale desktop instead of live background");
            }
            if(i%5==1) SendMessageW(overlay.window_,WM_KEYDOWN,VK_ESCAPE,0);
            else if(i%5==2) { SetCapture(background);ReleaseCapture(); }
            else if(i%5==3) SendMessageW(overlay.window_,WM_CANCELMODE,0,0);
            else Mouse(overlay.window_,WM_LBUTTONUP,b);
            Region();Require(GetCapture()!=overlay.window_,"Toolbar retained capture");
            Require(Output().pixels==original.pixels,"Toolbar movement modified exported image");
            if(i==0) Snapshot(L"-drag-end");
        }
        NoTool();Pass("PIN-toolbar-"+std::to_string(cycles)+"cycles-dpi"+std::to_string(dpi));
    }
    void TextCases() {
        NoTool();overlay.UpdateToolbarPosition(true);
        const auto oldCount=overlay.annotations_.size();CommandClick(Command::Number);
        POINT a{overlay.selection_.left+60,overlay.selection_.top+285};Click(overlay.window_,a);
        Require(overlay.textEditor_!=nullptr,"Number text editor missing");
        SetWindowTextW(overlay.textEditor_,L"输入中拖动\r\n第二行");
        POINT hit=overlay.editingAnnotation_.start;hit.x+=overlay.selection_.left;hit.y+=overlay.selection_.top;
        Mouse(overlay.window_,WM_LBUTTONDOWN,hit);Mouse(overlay.window_,WM_MOUSEMOVE,{hit.x+24,hit.y+14});
        Require(overlay.interaction_==ScreenshotOverlay::Interaction::MovingEditingAnnotation,"Number editor drag not entered");
        SendMessageW(overlay.window_,WM_CANCELMODE,0,0);
        Require(overlay.textEditor_ && IsWindowVisible(overlay.textEditor_),"Interrupted number drag left text editor hidden");
        Require(capture::WindowText(overlay.textEditor_).find(L"输入中拖动")!=std::wstring::npos,"Number drag lost uncommitted text");
        overlay.CommitTextEditing(true);Require(overlay.annotations_.size()==oldCount+1,"Number editing duplicated item");
        NoTool();CommandClick(Command::Text);Click(overlay.window_,Center(overlay.FontSizeComboRect()));Require(overlay.fontSizeMenuOpen_,"Font popup not open");
        SendMessageW(overlay.window_,WM_KEYDOWN,VK_ESCAPE,0);Require(!overlay.fontSizeMenuOpen_,"Escape did not close popup");
        NoTool();Region();Pass("PIN-text-drag-interrupt-font-popup");
    }
    void MultiplePins(HWND first) {
        Dpi(GetDpiForWindow(overlay.window_));
        auto firstImage=Output();overlay.CompletePinnedEditing(true);
        Begin(96);HWND second=Pin();auto secondImage=Output();
        for(int i=0;i<8;++i) {
            HWND target=i%2?second:first;Require(overlay.SwitchPinnedEditingForPointerDown(target),"Multi-pin switch failed");Region();
            Require(Output().pixels==(i%2?secondImage:firstImage).pixels,"Switch loaded another pin's pixels/annotations");
        }
        overlay.CompletePinnedEditing(true);DestroyWindow(second);Require(overlay.StartPinnedEditing(first),"First pin lost after closing sibling");
        Pass("PIN-multiple-switch-close");
    }
    void CopyCheck() {
        std::vector<std::pair<UINT,std::vector<BYTE>>> saved;
        Require(OpenClipboard(overlay.window_)!=FALSE,"Clipboard backup unavailable");
        for(UINT f=EnumClipboardFormats(0);f;f=EnumClipboardFormats(f)) {
            if(f==CF_BITMAP || f==CF_PALETTE || f==CF_ENHMETAFILE || f==CF_METAFILEPICT) continue;
            HANDLE h=GetClipboardData(f);SIZE_T n=h?GlobalSize(h):0;
            if(n) {auto p=static_cast<BYTE*>(GlobalLock(h));if(p){saved.push_back({f,{p,p+n}});GlobalUnlock(h);}}
        }
        CloseClipboard();
        struct Restore { HWND w;decltype(saved)& data;~Restore(){if(!OpenClipboard(w))return;EmptyClipboard();for(auto& item:data){HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,item.second.size());if(!h)continue;void* p=GlobalLock(h);if(!p){GlobalFree(h);continue;}memcpy(p,item.second.data(),item.second.size());GlobalUnlock(h);if(!SetClipboardData(item.first,h))GlobalFree(h);}CloseClipboard();} } restore{overlay.window_,saved};
        const auto expected=Output();Require(overlay.CopySelectionToClipboard(),"Copy image failed");
        Require(OpenClipboard(overlay.window_)!=FALSE,"Copied bitmap unavailable");
        auto bitmap=static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));auto actual=capture::FromBitmap(bitmap);CloseClipboard();
        Require(expected.width==actual.width && expected.height==actual.height && expected.pixels==actual.pixels,"Clipboard image differs from complete output");
        Pass("PIN-copy-full-image");
    }
    void MovePin(HWND pin,UINT dpi) {
        NoTool();overlay.UpdateToolbarPosition(true);auto original=Output();
        for(int i=0;i<6;++i) {
            Dpi(dpi);
            POINT a{};bool blank=false;
            for(int y=350;y>=30 && !blank;y-=20) for(int x=30;x<610;x+=20) {
                POINT p{overlay.selection_.left+x,overlay.selection_.top+y};
                if(overlay.HitTestAnnotation(p,true)<0 && overlay.HitTestToolbar(p)==Command::None && overlay.HitTestResizeHandle(p)==ScreenshotOverlay::ResizeHandle::None) {a=p;blank=true;break;}
            }
            Require(blank,"No blank pin area for window drag");RECT before=overlay.selection_;
            if(i%2) {
                POINT screen{a.x+overlay.virtualLeft_,a.y+overlay.virtualTop_};
                overlay.HidePinnedToolbar(pin);Require(!overlay.IsActive(),"Hidden toolbar retained editor");
                SetCursorPos(screen.x,screen.y);Pump();Require(WindowFromPoint(screen)==pin,"Hidden pin mouse target obscured");
                INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;Require(SendInput(1,&input,sizeof(input))==1,"Pin mouse down failed");Pump();
                SetCursorPos(screen.x+20,screen.y+12);Pump();input.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&input,sizeof(input));Pump();
                Require(overlay.IsEditingPinnedWindow(pin),"Native pin drag did not restore editor");
                Dpi(dpi);
            } else {
            Mouse(overlay.window_,WM_LBUTTONDOWN,a);Mouse(overlay.window_,WM_MOUSEMOVE,{a.x+20,a.y+12});
            Require(overlay.interaction_==ScreenshotOverlay::Interaction::MovingPinned,"Blank pin gesture hit a control");
            Require(!overlay.ToolbarVisible(),"Toolbar not hidden while pin moves");
            if(i==2) SendMessageW(overlay.window_,WM_KEYDOWN,VK_ESCAPE,0);
            else if(i==4) { SetCapture(background);ReleaseCapture(); }
            else Mouse(overlay.window_,WM_LBUTTONUP,{a.x+20,a.y+12});
            }
            if(overlay.selection_.left!=before.left+20 || overlay.selection_.top!=before.top+12) throw std::runtime_error("Pin geometry failed to move at "+std::to_string(i)+" delta="+std::to_string(overlay.selection_.left-before.left)+","+std::to_string(overlay.selection_.top-before.top));
            Require(overlay.ToolbarVisible() && GetCapture()!=overlay.window_,"Pin did not restore controls");
            Region();Require(Output().pixels==original.pixels,"Pin movement altered image/annotations");
        }
        Pass("PIN-visible-hidden-move-dpi"+std::to_string(dpi));
    }
    void Run(const std::wstring& output) {
        prefix=output;report.open(std::filesystem::path(output+L".txt"));
        POINT cursor{};GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);work=mi.rcWork;
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Background;wc.lpszClassName=L"PcTool.PinTestBackground";RegisterClassW(&wc);
        background=CreateWindowExW(0,wc.lpszClassName,L"Pin test grid",WS_POPUP|WS_VISIBLE,work.left,work.top,work.right-work.left,work.bottom-work.top,nullptr,nullptr,wc.hInstance,nullptr);
        SetWindowPos(background,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);Pump();
        for(UINT dpi:{96u,144u,192u}) {
            prefix=output+L"-"+std::to_wstring(dpi);Begin(dpi);DrawTools("SHOT-dpi"+std::to_string(dpi));
            HWND pin=Pin();overlay.dpi_=dpi;if(overlay.toolbarFont_)DeleteObject(overlay.toolbarFont_);overlay.toolbarFont_=capture::CreateToolbarFont(dpi);overlay.UpdateToolbarPosition(true);
            // Clear via actual Undo commands, then cover the empty and single-content states.
            while(!overlay.annotations_.empty()) CommandClick(Command::Undo);
            ToolbarMatrix(dpi,6);MovePin(pin,dpi);
            NoTool();CommandClick(Command::Rectangle);POINT single{overlay.selection_.left+50,overlay.selection_.top+50};Drag(overlay.window_,single,{single.x+60,single.y+40});NoTool();ToolbarMatrix(dpi,6);CommandClick(Command::Undo);
            DrawTools("PIN-dpi"+std::to_string(dpi));TextCases();ToolbarMatrix(dpi);MovePin(pin,dpi);
            MultiplePins(pin);
            CopyCheck();
            POINT outside{work.right-40,work.top+40};CheckCrossProcessClick(outside,overlay.window_);
            Require(!overlay.IsActive(),"Outside activation failed to hide pin controls");Require(overlay.StartPinnedEditing(pin),"Pin failed to reopen after external click");
            Pass("PIN-outside-cross-process-click");
            auto expected=Output();overlay.CompletePinnedEditing(true);Require(overlay.StartPinnedEditing(pin),"Re-edit missing");Require(Output().pixels==expected.pixels,"Re-edit changed committed contents");
            overlay.CompletePinnedEditing(true);DestroyWindow(pin);Pass("PIN-complete-reedit-dpi"+std::to_string(dpi));
        }
        report<<"ENV monitors="<<GetSystemMetrics(SM_CMONITORS)<<"; DPI simulated; physical mixed-DPI/IME candidate UI not exercised\n";
        Pass("SCREENSHOT-PIN-MATRIX");
    }
};
void RunScreenshotInteractionTests(const std::wstring& prefix) { ScreenshotInteractionTests tests;tests.Run(prefix); }
void RunWindowSnapTests() { ScreenshotInteractionTests tests;tests.WindowSnap(); }
