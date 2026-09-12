#include "shared/platform/app_storage.h"
#include "translation/translation_controller.h"
#include "translation/translation_capture.h"
#include "translation/translation_view.h"
#include "translation/translation_input_watch.h"
#include "translation/source_settings.h"
#include "translation/source_provider.h"
#include "translation/source_icons.h"
#include "ocr/ocr_service.h"
#include "shared/ui/capture_ui.h"
#include "shared/ui/toolbar_icons.h"
#include "shared/annotation/text_layout.h"
#include "shared/ui/resource.h"
#include <dwmapi.h>
#include <future>
#include <chrono>
#include <vector>
#include <array>
#include <cwctype>
#include <richedit.h>
#include <tom.h>
#include <fstream>
#include <filesystem>
namespace translation {
namespace {
constexpr int Input=1101,Pin=1102,ResultBase=1120;
constexpr UINT_PTR StatusTip=1200;
constexpr UINT_PTR Poll=41,NoticeTimer=42;
constexpr UINT CheckInactive=WM_APP+79;
constexpr ULONG_PTR CopyInputTag=0x50435443;
struct OcrCompletion {uint64_t revision{};std::wstring text,error;};
RECT MouseWorkArea(POINT point){MONITORINFO m{sizeof(m)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&m);return m.rcWork;}
bool DesktopWindow(HWND w){if(!w)return true;wchar_t name[64]{};GetClassNameW(w,name,64);return wcscmp(name,L"Progman")==0||wcscmp(name,L"WorkerW")==0||wcscmp(name,L"Shell_TrayWnd")==0;}
bool SameCopyForeground(HWND target){auto foreground=GetForegroundWindow();return foreground==target||(DesktopWindow(foreground)&&DesktopWindow(target));}
bool HasText(const std::wstring& text){return std::any_of(text.begin(),text.end(),[](wchar_t c){return !(std::iswspace(c)||c==0x85||c==0xa0||c==0x1680||(c>=0x2000&&c<=0x200b)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000||c==0xfeff);});}

}
struct TranslationController::Impl {
    HWND owner{},window{},editor{},tooltip{},viewport{},scrollbar{},notice{};HFONT font{};
    SlimScrollbar outerScroll,inputScroll;
    HMODULE richEdit{};
    uint64_t visibilityRevision{};bool passive{},activated{},userSwitch{};
    void Trace(const wchar_t* event){
        wchar_t path[32768]{};if(!GetEnvironmentVariableW(L"PCTOOL_TRANSLATION_TRACE",path,32768))return;
        std::filesystem::path log;try{log=app_storage::Data()/L"Logs";std::filesystem::create_directories(log);}catch(...){return;}
        std::wofstream file(log/L"translation.log",std::ios::app);
        DWORD clipboardPid{};GetWindowThreadProcessId(GetClipboardOwner(),&clipboardPid);
        GUITHREADINFO gui{sizeof(gui)};GetGUIThreadInfo(GetWindowThreadProcessId(GetForegroundWindow(),nullptr),&gui);
        file<<GetTickCount64()<<L" "<<event<<L" invocation="<<visibilityRevision<<L" request="<<revision<<L" foreground="<<GetForegroundWindow()<<L" visible="<<(window&&IsWindowVisible(window))
            <<L" target="<<target<<L" targetPid="<<targetPid<<L" clipboardPid="<<clipboardPid<<L" sequence="<<GetClipboardSequenceNumber()<<L" serial="<<inputWatch.Serial()<<L" expectedSerial="<<copyInputSerial<<L" phase="<<int(pending)<<L" focus="<<gui.hwndFocus<<L" guiFlags="<<gui.flags<<L"\n";
    }
    ULONG_PTR gdiplus{};UINT dpi{96};
    bool enabled{},pinned{},changing{},shuttingDown{};
    bool keys[3]{};int hover{},headerHover{-1};UINT fontDpi{};std::wstring status;
    hotkeys::Bindings bindings{hotkeys::Defaults};
    bool scrollbarShown{true},composing{},suppressReturn{};
    Submission submission;PopupShadow shadow{L"PcTool.TranslationShadow"};
    std::vector<HWND> results;
    SourceStore sourceStore;std::vector<SourceConfig> sources;bool sourcesLoaded{};
    std::unique_ptr<SourceSettings> sourceSettings;
    std::vector<std::shared_ptr<TranslationProvider>> retiredProviders;
    void ApplyProviders(std::vector<std::shared_ptr<TranslationProvider>> providers){
        CancelPending();submission.Cancel();for(auto& p:submission.providers)if(p)retiredProviders.push_back(p);
        submission.providers=std::move(providers);submission.Reset();headerHover=-1;
        if(editor){RebuildResults();Layout();}
    }
    void ReloadProviders(){std::vector<std::shared_ptr<TranslationProvider>> providers;for(const auto& source:sources)if(source.enabled&&source.verified)providers.push_back(MakeSourceProvider(source));ApplyProviders(std::move(providers));}
    void LoadSources(){if(sourcesLoaded)return;sources=sourceStore.Load();sourcesLoaded=true;ReloadProviders();}
    void OpenSettings(HWND parent){LoadSources();Hide();if(!sourceSettings)sourceSettings=std::make_unique<SourceSettings>(sourceStore,sources,[this]{ReloadProviders();});sourceSettings->Show(parent);}
    void RebuildResults(){
        const bool previousLayout=layingOut;layingOut=true;
        for(HWND result:results)DestroyWindow(result);results.clear();
        for(size_t i=0;i<submission.cards.size();++i){
            HWND result=CreateWindowExW(0,MSFTEDIT_CLASS,L"",WS_CHILD|WS_TABSTOP|ES_MULTILINE|ES_READONLY,0,0,1,1,viewport,reinterpret_cast<HMENU>(INT_PTR(ResultBase+i)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(result,EM_SETTEXTMODE,TM_RICHTEXT,0);SendMessageW(result,EM_SETBKGNDCOLOR,0,PopupStyle::ResultBackground);SetWindowSubclass(result,ControlProc,1,reinterpret_cast<DWORD_PTR>(this));results.push_back(result);if(font)StyleText(result);
        }
        layingOut=previousLayout;
    }
    PopupLayout geometry;int scrollOffset{},viewportHeight{},lineHeight{20},wheelRemainder{};bool wheelOverInput{};
    bool layingOut{},manualHeight{},sizing{},centerOnLayout{};int sizeStartHeight{};
    RECT invocationWork{},dragStart{};POINT dragCursor{};int frameDrag{};bool dragManualHeight{};
    void EndFrameDrag(bool cancel){
        if(!frameDrag)return;frameDrag=0;sizing=false;
        if(cancel){manualHeight=dragManualHeight;SetWindowPos(window,nullptr,dragStart.left,dragStart.top,dragStart.right-dragStart.left,dragStart.bottom-dragStart.top,SWP_NOZORDER|SWP_NOACTIVATE);}
        else{RECT r{};GetWindowRect(window,&r);if(r.bottom-r.top!=dragStart.bottom-dragStart.top)manualHeight=true;}
        if(GetCapture()==window)ReleaseCapture();Layout();
    }
    void MoveFrame(){
        if(!frameDrag)return;POINT cursor{};GetCursorPos(&cursor);RECT r=dragStart;const int dx=cursor.x-dragCursor.x,dy=cursor.y-dragCursor.y;
        if(frameDrag==HTCAPTION)OffsetRect(&r,dx,dy);
        else{
            if(frameDrag==HTLEFT||frameDrag==HTTOPLEFT||frameDrag==HTBOTTOMLEFT)r.left=std::min(r.left+dx,r.right-Dip(360));
            if(frameDrag==HTRIGHT||frameDrag==HTTOPRIGHT||frameDrag==HTBOTTOMRIGHT)r.right=std::max(r.right+dx,r.left+Dip(360));
            if(frameDrag==HTTOP||frameDrag==HTTOPLEFT||frameDrag==HTTOPRIGHT)r.top=std::min(r.top+dy,r.bottom-Dip(170));
            if(frameDrag==HTBOTTOM||frameDrag==HTBOTTOMLEFT||frameDrag==HTBOTTOMRIGHT)r.bottom=std::max(r.bottom+dy,r.top+Dip(170));
        }
        auto work=MouseWorkArea(cursor);InflateRect(&work,-Dip(14),-Dip(14));int width=std::min<int>(r.right-r.left,work.right-work.left),height=std::min<int>(r.bottom-r.top,work.bottom-work.top);
        r.left=std::clamp<int>(r.left,work.left,work.right-width);r.top=std::clamp<int>(r.top,work.top,work.bottom-height);
        SetWindowPos(window,nullptr,r.left,r.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    PopupLayout Geometry()const{return geometry;}
    HWND Button(int id)const{return GetDlgItem(window,id);}
    void PlaceContent(){
        const auto& layout=geometry;
        auto place=[&](HWND child,const RECT& r){SetWindowPos(child,nullptr,r.left,r.top-scrollOffset,std::max(1L,r.right-r.left),std::max(1L,r.bottom-r.top),SWP_NOZORDER|SWP_NOACTIVATE);};
        place(editor,layout.input);place(inputScroll.Window(),layout.inputScrollbar);SetWindowPos(inputScroll.Window(),HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);SyncInputScroll();
        for(size_t i=0;i<results.size();++i){place(results[i],layout.bodies[i]);ShowWindow(results[i],submission.cards[i].expanded?SW_SHOWNA:SW_HIDE);}
        InvalidateRect(viewport,nullptr,FALSE);
    }
    void ScrollTo(int position){
        const int next=std::clamp(position,0,std::max(0,geometry.contentHeight-viewportHeight));
        if(next==scrollOffset)return;scrollOffset=next;outerScroll.Update(geometry.contentHeight,viewportHeight,next,dpi);PlaceContent();
    }
    void Wheel(WPARAM wp,LPARAM lp,bool inputTarget=false){
        // RichEdit's native wheel handling depends on its native scrollbar.
        // Scroll explicitly because our scrollbar occupies the card padding.
        POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(viewport,&point);
        RECT clip{};GetClientRect(viewport,&clip);
        RECT card{Dip(12),0,geometry.inputScrollbar.right+Dip(3),geometry.inputBottom};
        const bool inViewport=PtInRect(&clip,point)!=FALSE;point.y+=scrollOffset;
        const bool overInput=(inViewport&&PtInRect(&card,point))||(inputTarget&&lp==0);
        if(overInput!=wheelOverInput){wheelRemainder=0;wheelOverInput=overInput;}
        wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);
        const int steps=wheelRemainder/WHEEL_DELTA;wheelRemainder%=WHEEL_DELTA;
        if(!steps)return;
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        if(!lines)return;
        if(overInput&&scrollbarShown){
            POINT before{};SendMessageW(editor,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&before));
            const int count=int(SendMessageW(editor,EM_GETLINECOUNT,0,0));
            const int page=10*lineHeight,maximum=std::max(0,(count-10)*lineHeight);
            const int distance=lines==WHEEL_PAGESCROLL?page:int(std::min<UINT>(lines,UINT(count)))*lineHeight;
            POINT next{before.x,std::clamp(int(before.y)-steps*distance,0,maximum)};
            SendMessageW(editor,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&next));SyncInputScroll();
        }
        // The input card owns its wheel, including its boundaries and short text.
        // Only a pointer outside the card may scroll the outer viewport.
        if(overInput)return;
        const int distance=lines==WHEEL_PAGESCROLL?viewportHeight:int(std::min<UINT>(lines,UINT(std::max(1,viewportHeight/lineHeight))))*lineHeight;
        ScrollTo(scrollOffset-steps*distance);
    }
    void ScrollCommand(WPARAM wp){int next=scrollOffset;switch(LOWORD(wp)){case SB_LINEUP:next-=lineHeight;break;case SB_LINEDOWN:next+=lineHeight;break;case SB_PAGEUP:next-=viewportHeight;break;case SB_PAGEDOWN:next+=viewportHeight;break;case SB_TOP:next=0;break;case SB_BOTTOM:next=geometry.contentHeight;break;case SB_THUMBTRACK:case SB_THUMBPOSITION:next=HIWORD(wp);break;}ScrollTo(next);}
    void SyncInputScroll(){
        POINT position{};SendMessageW(editor,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&position));
        const int count=int(SendMessageW(editor,EM_GETLINECOUNT,0,0)),total=count*lineHeight,page=std::clamp(count,2,10)*lineHeight;
        const int y=std::clamp(int(position.y),0,std::max(0,total-page));
        if(position.y!=y){position.y=y;SendMessageW(editor,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&position));}
        inputScroll.Update(total,page,y,dpi);
    }

    void ResetResults(){submission.Reset();headerHover=-1;if(editor)RefreshResults();}
    void RefreshResults(){const bool previousLayout=layingOut;layingOut=true;for(size_t i=0;i<results.size();++i){if(capture::WindowText(results[i])!=submission.cards[i].text){CHARRANGE selection{};POINT scroll{};SendMessageW(results[i],EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));SendMessageW(results[i],EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));SetWindowTextW(results[i],submission.cards[i].text.c_str());SendMessageW(results[i],EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));SendMessageW(results[i],EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));}}layingOut=previousLayout;Layout();}
    void Submit(){
        const auto text=capture::WindowText(editor);
        if(text.find_first_not_of(L" \t\r\n")==std::wstring::npos){SetStatus(L"请输入单词或文字。");return;}
        CancelPending();if(submission.providers.empty()){SetStatus(L"请配置翻译源");Layout();return;}submission.Submit(text);SetStatus({});
        scrollOffset=0;
        RefreshResults();
    }
    std::function<bool()> canCapture;
    TranslationCapture selection;
    uint64_t revision{};
    ULONGLONG menuCaptureDue{};
    std::shared_ptr<capture::Cancellation> cancellation;
    std::vector<std::future<OcrCompletion>> workers;
    enum class Pending {None,WaitKeys,PrepareCopy,Clipboard,Ocr};Pending pending{Pending::None};
    HWND target{},lastSelectionSource{};DWORD targetPid{},lastSelectionPid{},clearedSequence{};ULONGLONG deadline{};bool copyKeysDown{},readRetry{},copyListening{},transferringFocus{},copyInputPending{};
    InputWatch inputWatch;uint64_t copyInputSerial{};
    Impl(){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&gdiplus,&input,nullptr);}
    ~Impl(){Shutdown();if(gdiplus)Gdiplus::GdiplusShutdown(gdiplus);}
    int Dip(int n)const{return MulDiv(n,dpi,96);}
    void CancelPending(){menuCaptureDue=0;StopCopyWatch();ReleaseCopyKeys();submission.Cancel();++revision;if(cancellation)cancellation->requested=true;cancellation.reset();pending=Pending::None;}
    void LayoutNotice(){
        if(!notice||status.empty())return;
        RECT c{};GetClientRect(window,&c);HDC dc=GetDC(window);auto old=SelectObject(dc,font);
        RECT text{0,0,std::max(Dip(60),int(c.right)-Dip(92)),0};DrawTextW(dc,status.c_str(),-1,&text,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old);ReleaseDC(window,dc);
        int width=std::min(int(c.right)-Dip(24),int(text.right)+Dip(37)+Dip(11)+2),height=std::max(Dip(38),int(text.bottom)+Dip(18));
        SetWindowPos(notice,HWND_TOP,(c.right-width)/2,Dip(3),width,height,SWP_NOACTIVATE);
        SetWindowRgn(notice,CreateRoundRectRgn(0,0,width+1,height+1,Dip(16),Dip(16)),TRUE);
        InvalidateRect(notice,nullptr,FALSE);
    }
    static LRESULT CALLBACK NoticeProc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_NCHITTEST)return HTTRANSPARENT;
        if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);
            HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right,r.bottom);auto oldBitmap=SelectObject(buffer,bitmap);
            {Gdiplus::Graphics g(buffer);g.Clear(Gdiplus::Color(255,253,246,236));g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush orange(Gdiplus::Color(255,230,162,60));float d=float(self->Dip(16)),x=float(self->Dip(13)),y=(r.bottom-d)/2;
            g.FillEllipse(&orange,x,y,d,d);Gdiplus::Pen white(Gdiplus::Color(255,255,255,255),float(self->Dip(2)));white.SetStartCap(Gdiplus::LineCapRound);white.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&white,x+d/2,y+d*.25f,x+d/2,y+d*.55f);g.DrawLine(&white,x+d/2,y+d*.76f,x+d/2,y+d*.77f);}
            auto old=SelectObject(buffer,self->font);SetBkMode(buffer,TRANSPARENT);SetTextColor(buffer,RGB(190,125,35));RECT text{self->Dip(37),self->Dip(9),r.right-self->Dip(11),r.bottom-self->Dip(9)};
            DrawTextW(buffer,self->status.c_str(),-1,&text,DT_WORDBREAK|DT_NOPREFIX);SelectObject(buffer,old);BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
            SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);EndPaint(w,&ps);return 0;}
        return DefWindowProcW(w,m,wp,lp);
    }
    void SetStatus(std::wstring text){
        status=std::move(text);if(window)KillTimer(window,NoticeTimer);
        if(notice){ShowWindow(notice,SW_HIDE);if(!status.empty()&&IsWindowVisible(window)){LayoutNotice();ShowWindow(notice,SW_SHOWNA);SetTimer(window,NoticeTimer,3000,nullptr);}}
    }
    enum class EntryOutcome {Success,Empty,Failure,Cancelled};
    void FinishEntry(EntryOutcome outcome,std::wstring text={}){
        StopCopyWatch();pending=Pending::None;
        if(outcome==EntryOutcome::Cancelled){SetStatus(std::move(text));return;}
        if(outcome==EntryOutcome::Success){Replace(std::move(text));Show();Submit();return;}
        Replace(L"");Show();SetStatus(outcome==EntryOutcome::Empty?L"识别内容为空":std::move(text));
    }
    void Hide(){Trace(L"hide");++visibilityRevision;passive=activated=userSwitch=false;EndFrameDrag(false);CancelPending();StopVisibilityWatch();SetStatus({});shadow.Hide();if(window)ShowWindow(window,SW_HIDE);}
    void Shutdown(){
        if(shuttingDown)return;shuttingDown=true;
        CancelPending();StopVisibilityWatch();selection.Cancel();
        if(sourceSettings)sourceSettings->Close();submission.Cancel();
        for(int i=0;i<3;++i)if(keys[i]){UnregisterHotKey(owner,ManualHotkey+i);keys[i]=false;}
        for(auto& worker:workers)worker.wait();workers.clear();
        shadow.Close();if(window){KillTimer(window,Poll);DestroyWindow(window);}window=nullptr;
        if(font)DeleteObject(font);font=nullptr;
        if(richEdit)FreeLibrary(richEdit);richEdit=nullptr;
    }
    void StyleText(HWND text){
        const bool previous=changing;changing=true;
        IUnknown* rich{};ITextDocument* document{};
        SendMessageW(text,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(&rich));
        if(rich){rich->QueryInterface(__uuidof(ITextDocument),reinterpret_cast<void**>(&document));rich->Release();}
        if(document)document->Undo(tomSuspend,nullptr);
        CHARRANGE range{};SendMessageW(text,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
        POINT position{};SendMessageW(text,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&position));
        if(font)SendMessageW(text,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        SendMessageW(text,EM_SETSEL,0,-1);
        PARAFORMAT2 paragraph{};paragraph.cbSize=sizeof(paragraph);paragraph.dwMask=PFM_LINESPACING|PFM_SPACEBEFORE|PFM_SPACEAFTER;paragraph.bLineSpacingRule=4;paragraph.dyLineSpacing=MulDiv(lineHeight,1440,GetDpiForWindow(text));
        SendMessageW(text,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&paragraph));
        CHARFORMAT2W format{};format.cbSize=sizeof(format);format.dwMask=CFM_COLOR;format.crTextColor=PopupStyle::Text;SendMessageW(text,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));
        SendMessageW(text,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));SendMessageW(text,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&position));
        if(document){document->Undo(tomResume,nullptr);document->Release();}
        changing=previous;
    }
    void UpdateScrollbar(){Layout();}
    void Layout(){
        if(layingOut||!editor)return;
        layingOut=true;
        RECT c{};GetClientRect(window,&c);int width=c.right,height=c.bottom;
        const int barWidth=Dip(PopupStyle::ScrollbarWidth),contentWidth=std::max(Dip(100),width);
        const bool newFont=!font||fontDpi!=dpi;
        lineHeight=Dip(PopupStyle::LineHeight);
        if(newFont){
            if(font)DeleteObject(font);fontDpi=dpi;font=CreateFontW(-Dip(PopupStyle::FontSize),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
            StyleText(editor);for(HWND text:results)StyleText(text);

        }
        const PopupLayout columns(contentWidth,dpi,0,{},submission.cards);
        const int inputWidth=columns.input.right-columns.input.left,bodyWidth=contentWidth-Dip(50);
        auto measure=[&](HWND text,int textWidth){
            RECT r{};GetWindowRect(text,&r);const int oldLine=int(SendMessageW(text,EM_GETFIRSTVISIBLELINE,0,0));
            SetWindowPos(text,nullptr,0,0,std::max(1,textWidth),std::max(lineHeight*2,int(r.bottom-r.top)),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
            RECT format{1,1,std::max(2,textWidth-1),std::max(lineHeight*2,int(r.bottom-r.top))-1};
            // Both controls use the final text rectangle; scrollbars occupy existing padding.
            SendMessageW(text,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&format));
            const int lines=int(SendMessageW(text,EM_GETLINECOUNT,0,0));
            SendMessageW(text,EM_LINESCROLL,0,oldLine-int(SendMessageW(text,EM_GETFIRSTVISIBLELINE,0,0)));
            return std::max(1,lines);
        };
        const int inputLines=measure(editor,inputWidth);
        const int inputHeight=std::clamp(inputLines,2,10)*lineHeight+2;
        scrollbarShown=inputLines>10;
        std::vector<int> bodyHeights(results.size());for(size_t i=0;i<results.size();++i)bodyHeights[i]=measure(results[i],bodyWidth)*lineHeight+2;
        geometry=PopupLayout(contentWidth,dpi,inputHeight,bodyHeights,submission.cards);
        const int footer=Dip(8);
        if(!manualHeight&&!sizing){
            RECT r{};GetWindowRect(window,&r);POINT center{(r.left+r.right)/2,(r.top+r.bottom)/2};auto work=centerOnLayout?invocationWork:MouseWorkArea(center);InflateRect(&work,-Dip(14),-Dip(14));
            height=std::min<int>(geometry.contentHeight+Dip(44)+footer,std::min<int>(Dip(720),work.bottom-work.top));
            SetWindowPos(window,nullptr,centerOnLayout?work.left+(work.right-work.left-width)/2:std::clamp<int>(r.left,work.left,std::max<int>(work.left,work.right-width)),centerOnLayout?work.top+(work.bottom-work.top-height)/2:std::clamp<int>(r.top,work.top,work.bottom-height),width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        viewportHeight=std::max(1,height-Dip(44)-footer);
        // Do not synchronously paint the resized viewport with the old child
        // positions. Publish the completed layout and repaint it as one tree.
        MoveWindow(viewport,0,Dip(44),contentWidth,viewportHeight,FALSE);
        SetWindowPos(scrollbar,HWND_TOP,width-barWidth-Dip(1),Dip(44),barWidth,viewportHeight,SWP_NOACTIVATE);
        scrollOffset=std::clamp(scrollOffset,0,std::max(0,geometry.contentHeight-viewportHeight));
        outerScroll.Update(geometry.contentHeight,viewportHeight,scrollOffset,dpi);
        MoveWindow(Button(Pin),Dip(12),Dip(9),Dip(32),Dip(30),FALSE);PlaceContent();
        RECT inputFormat{1,1,std::max(2,inputWidth-1),inputHeight-1};SendMessageW(editor,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&inputFormat));
        for(size_t i=0;i<results.size();++i){RECT format{1,1,std::max(2,bodyWidth-1),bodyHeights[i]-1};SendMessageW(results[i],EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&format));}
        HRGN region=CreateRoundRectRgn(0,0,width+1,height+1,Dip(16),Dip(16));SetWindowRgn(window,region,FALSE);shadow.Sync(window,dpi);
        if(tooltip){TOOLINFOW tip{sizeof(tip)};tip.hwnd=window;tip.uId=StatusTip;tip.rect={Dip(12),height-Dip(28),width-Dip(12),height};SendMessageW(tooltip,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&tip));}
        LayoutNotice();layingOut=false;
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME);
    }
    static LRESULT CALLBACK ViewProc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(dc,true);EndPaint(w,&ps);return 0;}
        if(m==WM_COMMAND||m==WM_DRAWITEM||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORSTATIC||m==WM_NOTIFY)return SendMessageW(self->window,m,wp,lp);
        if(m==WM_MOUSEWHEEL){self->Wheel(wp,lp);return 0;}
        // Only the arrow button's rectangle may change expansion state.
        if(m==WM_MOUSEMOVE||m==WM_LBUTTONUP){POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)+self->scrollOffset};int hit=-1;
            if(self->submission.cards.empty()&&PtInRect(&self->geometry.configure,point)){if(m==WM_LBUTTONUP){try{self->OpenSettings(self->owner);}catch(...){MessageBoxW(self->owner,capture::CurrentError(L"无法打开翻译源设置").c_str(),L"PcTool",MB_OK|MB_ICONWARNING);}return 0;}SetCursor(LoadCursorW(nullptr,IDC_HAND));}
            for(int i=0;i<int(self->submission.cards.size());++i)if(self->submission.cards[i].CanExpand()&&PtInRect(&self->geometry.toggles[i],point))hit=i;
            if(m==WM_LBUTTONUP&&hit>=0){self->centerOnLayout=false;self->headerHover=hit;self->submission.cards[hit].Toggle();self->Layout();return 0;}
            if(hit!=self->headerHover){self->headerHover=hit;RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);}TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);
        }
        if(m==WM_MOUSELEAVE){self->headerHover=-1;RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);}
        if(m==WM_SETCURSOR&&self->headerHover>=0){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        return DefWindowProcW(w,m,wp,lp);
    }
    void Ensure(){
        if(window)return;
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PcTool.Translation";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.style=0;
        wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
        // A transient popup must also be visible before it gains foreground.
        // Create in the topmost band; background NOACTIVATE promotion alone
        // may be ignored. Pinning controls dismissal, not this display band.
        CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_COMPOSITED,wc.lpszClassName,L"中英翻译 · PcTool",WS_POPUP|WS_CLIPCHILDREN,0,0,450,330,nullptr,nullptr,wc.hInstance,this);
        if(!window)throw std::runtime_error("Cannot create translation window");
        DWMNCRENDERINGPOLICY ncPolicy=DWMNCRP_DISABLED;DwmSetWindowAttribute(window,DWMWA_NCRENDERING_POLICY,&ncPolicy,sizeof(ncPolicy));
        dpi=GetDpiForWindow(window);
        WNDCLASSW noticeClass{};noticeClass.hInstance=wc.hInstance;noticeClass.lpfnWndProc=NoticeProc;noticeClass.lpszClassName=L"PcTool.TranslationNotice";RegisterClassW(&noticeClass);
        notice=CreateWindowExW(WS_EX_NOACTIVATE,noticeClass.lpszClassName,L"",WS_CHILD|WS_CLIPSIBLINGS,0,0,1,1,window,reinterpret_cast<HMENU>(1201),wc.hInstance,this);
        WNDCLASSW viewClass{};viewClass.hInstance=wc.hInstance;viewClass.lpfnWndProc=ViewProc;viewClass.lpszClassName=L"PcTool.TranslationContent";viewClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&viewClass);
        viewport=CreateWindowExW(WS_EX_CONTROLPARENT,viewClass.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,window,reinterpret_cast<HMENU>(1110),wc.hInstance,this);
        scrollbar=outerScroll.Create(window,1111,[this](int position){ScrollTo(position);},RGB(255,255,255));
        richEdit=LoadLibraryW(L"Msftedit.dll");if(!richEdit)throw std::runtime_error("Cannot load system RichEdit");
        inputScroll.Create(viewport,1112,[this](int y){POINT position{0,y};SendMessageW(editor,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&position));SyncInputScroll();},PopupStyle::InputBackground);
        editor=CreateWindowExW(0,MSFTEDIT_CLASS,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,0,0,1,1,viewport,reinterpret_cast<HMENU>(INT_PTR(Input)),wc.hInstance,nullptr);
        SendMessageW(editor,EM_SETTEXTMODE,TM_RICHTEXT,0);
        SendMessageW(editor,EM_EXLIMITTEXT,0,4*1024*1024);
        SendMessageW(editor,EM_SETBKGNDCOLOR,0,PopupStyle::InputBackground);
        SendMessageW(editor,EM_SETEVENTMASK,0,ENM_CHANGE|ENM_SCROLL);
        SetWindowSubclass(editor,ControlProc,1,reinterpret_cast<DWORD_PTR>(this));
        tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,0,0,0,0,window,nullptr,wc.hInstance,nullptr);
        SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,320);
        TOOLINFOW statusInfo{sizeof(statusInfo)};statusInfo.uFlags=TTF_TRACK|TTF_ABSOLUTE;statusInfo.hwnd=window;statusInfo.uId=StatusTip;statusInfo.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&statusInfo));
        for(int id:{Pin}){
            HWND button=CreateWindowExW(0,L"BUTTON",L"置顶",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,window,reinterpret_cast<HMENU>(INT_PTR(id)),wc.hInstance,nullptr);
            SetWindowSubclass(button,ControlProc,1,reinterpret_cast<DWORD_PTR>(this));
            TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(button);tool.lpszText=LPSTR_TEXTCALLBACKW;SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        }
        RebuildResults();
        SetWindowPos(window,nullptr,0,0,Dip(PopupStyle::Width),Dip(330),SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE);
        SetTimer(window,Poll,30,nullptr);Layout();
    }
    void Show(bool focus=true){
        ++visibilityRevision;passive=!focus;activated=focus;userSwitch=false;Trace(focus?L"show-active":L"show-passive");
        POINT cursor{};GetCursorPos(&cursor);invocationWork=MouseWorkArea(cursor);Ensure();StartVisibilityWatch();manualHeight=false;scrollOffset=0;centerOnLayout=true;
        // SWP_SHOWWINDOW alone leaves WS_MINIMIZE set. Restore the actual
        // window before measuring/positioning, without stealing Ctrl+C focus.
        ShowWindow(window,focus?SW_SHOWNORMAL:SW_SHOWNOACTIVATE);
        auto work=invocationWork;InflateRect(&work,-Dip(14),-Dip(14));RECT r{};GetWindowRect(window,&r);
        const int width=std::min<int>(Dip(PopupStyle::Width),work.right-work.left),height=std::min<int>(r.bottom-r.top,work.bottom-work.top);
        // Stay above the source without stealing queued Ctrl+C. Unpinned
        // popups still dismiss on outside interaction through the input watch.
        SetWindowPos(window,HWND_TOPMOST,work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,SWP_SHOWWINDOW|(focus?0:SWP_NOACTIVATE));
        Layout();shadow.Sync(window,dpi);if(focus){SetForegroundWindow(window);SetFocus(editor);}
    }
    void Replace(std::wstring text){
        CancelPending();ResetResults();changing=true;SetWindowTextW(editor,text.c_str());changing=false;
        SendMessageW(editor,EM_SETSEL,text.size(),text.size());SendMessageW(editor,EM_SCROLLCARET,0,0);SetStatus({});
        UpdateScrollbar();
    }
    void ClearEntry(){Hide();Ensure();Replace(L"");scrollOffset=0;}
    void Manual(){lastSelectionSource=nullptr;lastSelectionPid=0;selection.Cancel();ClearEntry();Show();}
    void BeginOcr(capture::Image image){
        CancelPending();Ensure();SetStatus(L"正在识别…");pending=Pending::Ocr;
        cancellation=std::make_shared<capture::Cancellation>();cancellation->generation=revision;
        auto cancel=cancellation;const auto request=revision;
        workers.push_back(std::async(std::launch::async,[image=std::move(image),cancel,request]() {
            OcrCompletion result;result.revision=request;
            try{result.text=capture::RecognizeLocalDocument(image,*cancel).text;}
            catch(...){result.error=capture::CurrentError(L"文字识别失败");}
            return result;
        }));
    }
    void Capture(){
        lastSelectionSource=nullptr;lastSelectionPid=0;
        if(selection.Focus())return;
        if(canCapture&&!canCapture())return;
        ClearEntry();DwmFlush();
        try{if(!selection.Start([this](capture::Image image){BeginOcr(std::move(image));},[this]{CancelPending();})) {FinishEntry(EntryOutcome::Failure,L"无法创建截图选区。");}}
        catch(...){FinishEntry(EntryOutcome::Failure,capture::CurrentError(L"无法截取桌面"));}
    }
    void ReleaseCopyKeys(){
        if(!copyKeysDown)return;copyKeysDown=false;INPUT input[2]{};
        for(auto& key:input){key.type=INPUT_KEYBOARD;key.ki.dwFlags=KEYEVENTF_KEYUP;key.ki.dwExtraInfo=CopyInputTag;}
        input[0].ki.wVk='C';input[1].ki.wVk=VK_CONTROL;SendInput(2,input,sizeof(INPUT));
    }
    bool ReadClipboardText(std::wstring& text){
        if(!IsClipboardFormatAvailable(CF_UNICODETEXT))return true;
        auto memory=GetClipboardData(CF_UNICODETEXT);if(!memory)return false;
        const auto count=GlobalSize(memory)/sizeof(wchar_t);auto* value=static_cast<const wchar_t*>(GlobalLock(memory));
        if(!value)return false;text.assign(value,wcsnlen_s(value,count));GlobalUnlock(memory);return true;
    }
    void StopCopyWatch(){
        inputWatch.EndCopy();
        if(copyListening&&window)RemoveClipboardFormatListener(window);copyListening=false;readRetry=false;transferringFocus=false;copyInputPending=false;
    }
    void StopVisibilityWatch(){inputWatch.Stop();}
    void StartVisibilityWatch(){inputWatch.Invocation(visibilityRevision);inputWatch.Start(window);}
    bool WatchCopy(){copyInputSerial=inputWatch.Serial();copyListening=AddClipboardFormatListener(window)!=FALSE;return copyListening;}
    void FromSelection(bool fromMenu=false,shared_platform::MenuSource menuSource={}){
        Trace(L"Alt+E");++visibilityRevision;passive=true;activated=false;
        HWND original=fromMenu?menuSource.window:GetForegroundWindow();
        if(fromMenu){DWORD pid{};GetWindowThreadProcessId(original,&pid);if(!IsWindow(original)||!pid||pid!=menuSource.process){FinishEntry(EntryOutcome::Failure,L"原取词窗口已关闭或改变，请回到需要取词的应用重试。");return;}}
        const bool repeated=original==window;
        if(repeated){
            DWORD pid{};if(lastSelectionSource)GetWindowThreadProcessId(lastSelectionSource,&pid);
            if(!IsWindow(lastSelectionSource)||pid!=lastSelectionPid){FinishEntry(EntryOutcome::Failure,L"原取词窗口已关闭，请回到需要取词的应用重试。");return;}
            original=lastSelectionSource;
        }
        selection.Cancel();Ensure();Replace(L"");scrollOffset=0;
        target=original;GetWindowThreadProcessId(target,&targetPid);lastSelectionSource=target;lastSelectionPid=targetPid;
        pending=Pending::WaitKeys;deadline=GetTickCount64()+1500;
        transferringFocus=repeated||fromMenu;
        if(transferringFocus&&!SetForegroundWindow(target)){CopyFailed(L"无法激活原取词窗口，请回到原应用后重试。");return;}
        // Do not hide an existing popup while restoring the source application's focus.
        Show(false);SetStatus(L"识别内容为空");
        inputWatch.BeginCopy(revision,bindings[4].key);
        if(!WatchCopy()){CopyFailed(L"无法监听本次复制，请重试。");return;}
        StartCopy();
    }
    void ClipboardUpdated(){
        Trace(L"clipboard-update");
        if(copyInputPending||inputWatch.Serial()!=copyInputSerial||pending!=Pending::Clipboard||GetClipboardSequenceNumber()==clearedSequence)return;
        if(!SameCopyForeground(target)&&GetForegroundWindow()!=window){Trace(L"copy-source-changed");CopyFailed(L"取词来源窗口已改变，请回到原应用重试。");return;}
        if(!OpenClipboard(window)){
            if(!readRetry){readRetry=true;deadline=GetTickCount64()+1500;}
            else if(GetTickCount64()>deadline)CopyFailed(L"无法读取剪贴板，请关闭占用剪贴板的程序后重试。");
            return;
        }
        const DWORD sequence=GetClipboardSequenceNumber();DWORD sourcePid{};GetWindowThreadProcessId(GetClipboardOwner(),&sourcePid);
        if(sourcePid!=targetPid){CloseClipboard();Trace(L"copy-owner-rejected");CopyFailed(L"剪贴板内容来自其他程序，本次取词已取消，请重试。");return;}
        std::wstring text;const bool read=ReadClipboardText(text);CloseClipboard();
        if(sequence!=GetClipboardSequenceNumber()){readRetry=true;deadline=GetTickCount64()+1500;return;}
        Trace(!read?L"copy-read-failed":HasText(text)?L"copy-text-ready":L"copy-empty");
        if(!read)CopyFailed(L"无法读取复制的文字，请重试。");
        else if(!HasText(text))FinishEntry(EntryOutcome::Empty);
        else FinishEntry(EntryOutcome::Success,std::move(text));
    }
    void StartCopy(){
        if(!SameCopyForeground(target)){
            if(transferringFocus&&GetTickCount64()<=deadline)return;
            CopyFailed(L"无法激活原取词窗口，请回到原应用后重试。");return;
        }
        transferringFocus=false;
        if(pending==Pending::WaitKeys){
            for(int key:{int(bindings[4].key),VK_LMENU,VK_RMENU,VK_LCONTROL,VK_RCONTROL,VK_LSHIFT,VK_RSHIFT,VK_LWIN,VK_RWIN})if(GetAsyncKeyState(key)&0x8000)return;
            inputWatch.KeysReleased();pending=Pending::PrepareCopy;deadline=GetTickCount64()+1500;Trace(L"copy-keys-released");
            // Yield this dispatch so the source can consume the physical Alt-up.
            // Alt shortcuts can enter its menu loop on release (e.g. Notepad++).
            return;
        }
        GUITHREADINFO gui{sizeof(gui)};
        if(GetGUIThreadInfo(GetWindowThreadProcessId(target,nullptr),&gui)&&(gui.flags&(GUI_INMENUMODE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE))){
            DWORD_PTR ignored{};
            if(!SendMessageTimeoutW(target,WM_CANCELMODE,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&ignored)){CopyFailed(L"无法退出来源窗口的菜单状态，请关闭菜单后重试。");return;}
            Trace(L"copy-source-menu-dismissed");
        }
        if(!OpenClipboard(window)){if(GetTickCount64()>deadline)CopyFailed(L"剪贴板被占用，请稍后重试。");return;}
        const bool cleared=EmptyClipboard()!=FALSE;clearedSequence=GetClipboardSequenceNumber();CloseClipboard();
        if(!cleared){CopyFailed(L"无法清空剪贴板，请重试。");return;}
        INPUT input[4]{};for(auto& key:input){key.type=INPUT_KEYBOARD;key.ki.dwExtraInfo=CopyInputTag;}
        input[0].ki.wVk=VK_CONTROL;input[1].ki.wVk='C';input[2].ki.wVk='C';input[2].ki.dwFlags=KEYEVENTF_KEYUP;input[3].ki.wVk=VK_CONTROL;input[3].ki.dwFlags=KEYEVENTF_KEYUP;
        // Publish reception state before dispatch; clipboard delivery is asynchronous.
        copyKeysDown=true;pending=Pending::Clipboard;
        if(SendInput(4,input,sizeof(INPUT))!=4){CopyFailed(L"无法向目标程序发送复制按键，请检查目标程序权限。");return;}
        copyKeysDown=false;
        // The passive popup is already visible. Do not start a second visibility
        // generation here: that would invalidate a mouse cancellation queued during SendInput.
        Trace(L"copy-dispatched");
    }
    void CopyFailed(const wchar_t* message){ReleaseCopyKeys();FinishEntry(EntryOutcome::Failure,message);}
    void Tick(){
        if(menuCaptureDue){
            if(GetAsyncKeyState(VK_ESCAPE)&0x8000){menuCaptureDue=0;return;}
            if(GetTickCount64()>=menuCaptureDue){menuCaptureDue=0;Capture();return;}
        }
        if(userSwitch&&window&&IsWindowVisible(window)&&GetForegroundWindow()!=window&&!SameCopyForeground(target)){
            userSwitch=false;if(!pinned){Trace(L"user-switch");Hide();}else CancelPending();
        }
        HWND captured=GetCapture();
        if(captured&&std::find(results.begin(),results.end(),captured)!=results.end()&&(GetAsyncKeyState(VK_LBUTTON)&0x8000)){
            POINT cursor{};GetCursorPos(&cursor);RECT clip{};GetWindowRect(viewport,&clip);
            if(cursor.y<clip.top+Dip(12)||cursor.y>=clip.bottom-Dip(12)){
                const int before=scrollOffset;ScrollTo(scrollOffset+(cursor.y<clip.top+Dip(12)?-lineHeight:lineHeight));
                if(before!=scrollOffset){ScreenToClient(captured,&cursor);SendMessageW(captured,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(cursor.x,cursor.y));}
            }
        }

        const bool copying=pending==Pending::WaitKeys||pending==Pending::PrepareCopy||pending==Pending::Clipboard;
        if(copying&&inputWatch.Serial()!=copyInputSerial)copyInputPending=true;
        if(copying&&!copyInputPending&&!transferringFocus&&GetForegroundWindow()&&GetForegroundWindow()!=window&&!SameCopyForeground(target)){
            Trace(L"copy-focus-cancel");CopyFailed(L"取词来源窗口已改变，请回到原应用重试。");
        }
        if(!copyInputPending&&(pending==Pending::WaitKeys||pending==Pending::PrepareCopy))StartCopy();
        else if(!copyInputPending&&pending==Pending::Clipboard&&readRetry)ClipboardUpdated();
        for(auto it=workers.begin();it!=workers.end();){
            if(it->wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
            auto result=it->get();it=workers.erase(it);
            if(result.revision==revision&&pending==Pending::Ocr){
                pending=Pending::None;
                if(!result.error.empty())FinishEntry(EntryOutcome::Failure,result.error);
                else if(!HasText(result.text))FinishEntry(EntryOutcome::Empty);
                else {FinishEntry(EntryOutcome::Success,std::move(result.text));}
            }
        }
        if(submission.Poll())RefreshResults();
    }
    void Command(int id){
        if(id==Pin){pinned=!pinned;shadow.Sync(window,dpi);InvalidateRect(Button(Pin),nullptr,FALSE);}
    }
    void Paint(HDC output,bool content=false){
        RECT c{};GetClientRect(content?viewport:window,&c);HDC dc=CreateCompatibleDC(output);HBITMAP bitmap=CreateCompatibleBitmap(output,c.right,c.bottom);auto old=SelectObject(dc,bitmap);
        {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);g.Clear(Gdiplus::Color(255,255,255,255));
            auto card=[&](float x,float y,float w,float h,Gdiplus::Color color){Gdiplus::GraphicsPath path;capture::text_layout::AddRoundedRectangle(path,{x,y,w,h},float(Dip(8)));Gdiplus::SolidBrush brush(color);g.FillPath(&brush,&path);};
            if(content){g.TranslateTransform(0,float(-scrollOffset));
            const auto layout=Geometry();const int bottom=layout.inputBottom;
            card(float(Dip(12)),0.0F,float(c.right-Dip(24)),float(bottom),Gdiplus::Color(255,229,229,229));
            Gdiplus::Font title(L"Microsoft YaHei UI",float(Dip(13)),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255,70,75,82)),muted(Gdiplus::Color(255,115,120,130));
            if(submission.cards.empty()){const auto& r=layout.configure;card(float(r.left),float(r.top),float(r.right-r.left),float(r.bottom-r.top),Gdiplus::Color(255,237,237,237));Gdiplus::SolidBrush link(Gdiplus::Color(255,83,150,46));g.DrawString(L"请配置翻译源",-1,&title,Gdiplus::PointF(float(r.left+Dip(13)),float(r.top+Dip(8))),&link);}
            for(int i=0;i<int(submission.cards.size());++i){
                const auto& r=layout.cards[i];const int y=r.top;
                card(float(r.left),float(y),float(r.right-r.left),float(r.bottom-r.top),Gdiplus::Color(255,237,237,237));
                if(headerHover==i)card(float(r.left+Dip(2)),float(y+Dip(2)),float(r.right-r.left-Dip(4)),float(Dip(31)),Gdiplus::Color(255,221,221,221));
                Gdiplus::Pen arrow(Gdiplus::Color(255,95,100,108),float(dpi)*1.5F/96);
                arrow.SetStartCap(Gdiplus::LineCapRound);arrow.SetEndCap(Gdiplus::LineCapRound);
                const auto& toggle=layout.toggles[i];
                const float x=float(toggle.left+toggle.right)/2-Dip(4),cy=float(toggle.top+toggle.bottom)/2;
                if(submission.cards[i].expanded){g.DrawLine(&arrow,x,cy-2,x+Dip(4),cy+2);g.DrawLine(&arrow,x+Dip(4),cy+2,x+Dip(8),cy-2);}
                else{g.DrawLine(&arrow,x+2,cy-Dip(4),x+Dip(6),cy);g.DrawLine(&arrow,x+Dip(6),cy,x+2,cy+Dip(4));}
                int labelLeft=r.left+Dip(13);
                if(submission.cards[i].kind){
                    // The vector helper draws in HDC coordinates; account for
                    // the scrolling transform applied to the surrounding GDI+.
                    RECT icon{labelLeft,y+Dip(7)-scrollOffset,labelLeft+Dip(20),y+Dip(27)-scrollOffset};
                    DrawSourceIcon(dc,*submission.cards[i].kind,icon);labelLeft+=Dip(28);
                }
                const auto& label=submission.cards[i].name;Gdiplus::StringFormat format;format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);g.DrawString(label.c_str(),-1,&title,Gdiplus::RectF(float(labelLeft),float(y+Dip(8)),float(std::max(1,int(toggle.left)-Dip(8)-labelLeft)),float(Dip(24))),&format,&ink);
            }
            }
        }
        BitBlt(output,0,0,c.right,c.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    void DrawButton(const DRAWITEMSTRUCT& item){
        const int id=int(item.CtlID);RECT r=item.rcItem;
        if(id!=Pin)return;
        HBRUSH brush=CreateSolidBrush(RGB(255,255,255));FillRect(item.hDC,&r,brush);DeleteObject(brush);
        const bool selected=pinned;
        if(hover==id||selected||(item.itemState&ODS_SELECTED)){
            Gdiplus::Graphics g(item.hDC);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::SolidBrush fill(Gdiplus::Color(255,201,201,201));Gdiplus::GraphicsPath path;capture::text_layout::AddRoundedRectangle(path,{2,2,float(r.right-4),float(r.bottom-4)},float(Dip(5)));g.FillPath(&fill,&path);
        }
        if(!pinned){
            Gdiplus::Graphics g(item.hDC);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.TranslateTransform(float(r.right)/2.0F,float(r.bottom)/2.0F);g.ScaleTransform(float(dpi)/96,float(dpi)/96);
            Gdiplus::Pen pen(Gdiplus::Color(255,73,77,82),1.5F);pen.SetLineJoin(Gdiplus::LineJoinRound);pen.SetStartCap(Gdiplus::LineCapRound);pen.SetEndCap(Gdiplus::LineCapRound);
            Gdiplus::PointF points[]={{-4,-8},{4,-8},{3,-2},{6,2},{6,3},{-6,3},{-6,2},{-3,-2},{-4,-8}};g.DrawLines(&pen,points,9);g.DrawLine(&pen,0,3,0,9);
        }
        else capture::DrawToolbarIcon(item.hDC,capture::ToolbarIcon::Pin,r,dpi,selected?RGB(40,128,220):RGB(73,77,82),18);
        // Match the screenshot toolbar: no native dotted focus rectangle.
        // Keyboard focus and Tab navigation remain functional.
    }
    static LRESULT CALLBACK ControlProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
        auto* self=reinterpret_cast<Impl*>(data);const int id=GetDlgCtrlID(w);
        // A multiline EDIT does not send EN_CHANGE for WM_SETTEXT. Treat external
        // text replacement just like typing; our own committed OCR uses changing.
        if(id==Input&&m==WM_SETTEXT&&!self->changing){self->CancelPending();self->ResetResults();self->SetStatus({});}
        if(m==WM_KEYDOWN&&wp==VK_ESCAPE){if(self->frameDrag)self->EndFrameDrag(true);else{self->Trace(L"editor-escape");self->Hide();}return 0;}
        if((id==Input||std::find(self->results.begin(),self->results.end(),w)!=self->results.end())&&m==WM_KEYDOWN&&wp=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(w,EM_SETSEL,0,-1);return 0;}
        if(id==Input&&m==WM_PASTE){SendMessageW(w,EM_PASTESPECIAL,CF_UNICODETEXT,0);return 0;}
        if(id==Input&&m==WM_KEYDOWN&&(GetKeyState(VK_CONTROL)&0x8000)&&(wp=='B'||wp=='I'||wp=='U'))return 0;
        if(id==Input&&m==WM_IME_STARTCOMPOSITION)self->composing=true;
        if(id==Input&&m==WM_IME_ENDCOMPOSITION)self->composing=false;
        if(id==Input&&m==WM_KEYDOWN&&wp==VK_RETURN){
            self->suppressReturn=self->composing;
            if(!self->composing&&!(GetKeyState(VK_SHIFT)&0x8000)){self->suppressReturn=true;self->Submit();return 0;}
        }
        if(id==Input&&m==WM_CHAR&&wp==L'\r'&&self->suppressReturn){self->suppressReturn=false;return 0;}
        if(id==Input&&m==WM_KEYUP&&wp==VK_RETURN)self->suppressReturn=false;
        if(m==WM_KEYDOWN&&wp==VK_TAB){HWND next=GetNextDlgTabItem(self->window,w,(GetKeyState(VK_SHIFT)&0x8000)!=0);if(next)SetFocus(next);return 0;}
        if(id!=Input&&m==WM_MOUSEMOVE){if(self->hover!=id){self->hover=id;InvalidateRect(w,nullptr,FALSE);}TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);}
        if(id!=Input&&m==WM_MOUSELEAVE){self->hover=0;InvalidateRect(w,nullptr,FALSE);}
        if(m==WM_MOUSEWHEEL){
            self->Wheel(wp,lp,id==Input);return 0;
        }
        auto result=DefSubclassProc(w,m,wp,lp);
        if((id==Input||std::find(self->results.begin(),self->results.end(),w)!=self->results.end())&&m==WM_SETTEXT){self->StyleText(w);self->UpdateScrollbar();}
        if(id==Input&&(m==WM_VSCROLL||m==WM_KEYUP||m==WM_LBUTTONUP||m==WM_MOUSEMOVE))self->SyncInputScroll();
        if(id==Input&&m==WM_PAINT&&GetWindowTextLengthW(w)==0){HDC dc=GetDC(w);auto old=SelectObject(dc,self->font);SetTextColor(dc,RGB(158,163,173));SetBkMode(dc,TRANSPARENT);RECT r{};SendMessageW(w,EM_GETRECT,0,reinterpret_cast<LPARAM>(&r));r.left+=self->Dip(4);DrawTextW(dc,L"请输入单词或文字",-1,&r,DT_LEFT|DT_TOP|DT_NOPREFIX);SelectObject(dc,old);ReleaseDC(w,dc);}
        return result;
    }
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_NCCALCSIZE)return 0;
        if(m==WM_NCPAINT)return 0;
        if(m==WM_NCACTIVATE)return TRUE;
        if(m==WM_NCLBUTTONDOWN&&(wp==HTCAPTION||(wp>=HTLEFT&&wp<=HTBOTTOMRIGHT))){self->centerOnLayout=false;self->frameDrag=int(wp);self->dragManualHeight=self->manualHeight;GetWindowRect(w,&self->dragStart);GetCursorPos(&self->dragCursor);self->sizing=true;SetCapture(w);return 0;}
        if(m==WM_MOUSEMOVE&&self->frameDrag){self->MoveFrame();return 0;}
        if(m==WM_LBUTTONUP&&self->frameDrag){self->EndFrameDrag(false);return 0;}
        if((m==WM_CANCELMODE||m==WM_CAPTURECHANGED)&&self->frameDrag){self->EndFrameDrag(m==WM_CANCELMODE);return 0;}
        if(m==WM_KEYDOWN&&wp==VK_ESCAPE&&self->frameDrag){self->EndFrameDrag(true);return 0;}
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(dc);EndPaint(w,&ps);return 0;}
        if(m==WM_WINDOWPOSCHANGED&&!self->layingOut){self->shadow.Sync(w,self->dpi);}
        if(m==WM_SIZE&&self->editor){self->Layout();return 0;}
        if(m==WM_DPICHANGED){self->dpi=HIWORD(wp);auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
        if(m==WM_VSCROLL&&reinterpret_cast<HWND>(lp)==self->scrollbar){self->ScrollCommand(wp);return 0;}
        if(m==WM_MOUSEWHEEL){self->Wheel(wp,lp);return 0;}
        if(m==WM_ENTERSIZEMOVE){self->centerOnLayout=false;self->sizing=true;RECT r{};GetWindowRect(w,&r);self->sizeStartHeight=r.bottom-r.top;}
        if(m==WM_EXITSIZEMOVE){self->sizing=false;RECT r{};GetWindowRect(w,&r);if(r.bottom-r.top!=self->sizeStartHeight)self->manualHeight=true;self->Layout();}
        if(m==WM_GETMINMAXINFO){auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={self->Dip(360),self->Dip(170)};return 0;}
        if(m==WM_MOVING||m==WM_SIZING){
            POINT cursor{};GetCursorPos(&cursor);RECT work=MouseWorkArea(cursor);InflateRect(&work,-self->Dip(14),-self->Dip(14));auto* r=reinterpret_cast<RECT*>(lp);
            const int width=std::min<int>(r->right-r->left,work.right-work.left),height=std::min<int>(r->bottom-r->top,work.bottom-work.top);
            r->left=std::clamp<int>(r->left,work.left,work.right-width);r->top=std::clamp<int>(r->top,work.top,work.bottom-height);r->right=r->left+width;r->bottom=r->top+height;return TRUE;
        }
        if(m==WM_NCHITTEST){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            // The outer scrollbar shares the right padding with the resize edge.
            // Let Windows route this strip to the child instead of resizing.
            RECT track{};if(IsWindowVisible(self->scrollbar)&&GetWindowRect(self->scrollbar,&track)&&PtInRect(&track,p))return HTCLIENT;
            ScreenToClient(w,&p);RECT c{};GetClientRect(w,&c);int edge=self->Dip(6);bool l=p.x<edge,r=p.x>=c.right-edge,t=p.y<edge,b=p.y>=c.bottom-edge;
            if(t&&l)return HTTOPLEFT;if(t&&r)return HTTOPRIGHT;if(b&&l)return HTBOTTOMLEFT;if(b&&r)return HTBOTTOMRIGHT;if(l)return HTLEFT;if(r)return HTRIGHT;if(t)return HTTOP;if(b)return HTBOTTOM;
            if(p.y<self->Dip(42)&&p.x>self->Dip(48))return HTCAPTION;return HTCLIENT;
        }
        if(m==WM_ACTIVATE){
            if(LOWORD(wp)==WA_INACTIVE)PostMessageW(w,CheckInactive,WPARAM(self->visibilityRevision),0);
            else{self->Trace(L"activated");if(!self->passive)self->activated=true;}
            return 0;
        }
        // Activation notifications are not evidence of user interaction. In
        // passive mode only a real click (or an explicit Show(true)) opts in to
        // normal focus-loss dismissal; transient source activation cannot do it.
        if(m==WM_MOUSEACTIVATE){self->passive=false;self->activated=true;}
        if(m==CheckInactive){
            if(wp!=WPARAM(self->visibilityRevision))return 0;
            if(!self->pinned&&(lp==1||(!self->passive&&self->activated&&!self->transferringFocus&&GetForegroundWindow()&&GetForegroundWindow()!=w))){self->Trace(lp?L"outside-click":L"deactivated");self->Hide();}
            return 0;
        }
        if(m==WM_CLOSE||(m==WM_KEYDOWN&&wp==VK_ESCAPE)){self->Trace(m==WM_CLOSE?L"window-close":L"window-escape");self->Hide();return 0;}
        if(m==WM_TIMER&&wp==NoticeTimer){self->SetStatus({});return 0;}
        if(m==WM_CLIPBOARDUPDATE){self->ClipboardUpdated();return 0;}
        if(m==InputWatch::CancelCopy){
            if(wp==WPARAM(self->revision)&&(self->pending==Pending::WaitKeys||self->pending==Pending::PrepareCopy||self->pending==Pending::Clipboard)){
                self->Trace(lp?L"copy-key-cancel":L"copy-mouse-cancel");self->CancelPending();self->SetStatus({});
            }
            return 0;
        }
        if(m==InputWatch::Mouse||m==InputWatch::Key){
            if(wp!=WPARAM(self->visibilityRevision))return 0;
            if(m==InputWatch::Mouse){
                POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                // Hit testing belongs on the UI thread, never inside a low-level hook.
                HWND hit=WindowFromPoint(point);const bool inside=hit==w||IsChild(w,hit);
                if(!inside&&!self->pinned){self->Trace(L"outside-click");self->Hide();return 0;}
            }else{
                if(LOWORD(lp)==VK_ESCAPE){self->Trace(L"input-escape");if(self->frameDrag)self->EndFrameDrag(true);else self->Hide();return 0;}
                if(HIWORD(lp))self->userSwitch=true;
            }
            return 0;
        }
        if(m==WM_TIMER&&wp==Poll){self->Tick();return 0;}
        if(m==WM_COMMAND){
            if(LOWORD(wp)==Input&&HIWORD(wp)==EN_CHANGE&&!self->changing){self->CancelPending();self->ResetResults();self->SetStatus({});self->UpdateScrollbar();}
            else if(HIWORD(wp)==BN_CLICKED)self->Command(LOWORD(wp));return 0;
        }
        if(m==WM_CTLCOLORSTATIC&&GetDlgCtrlID(reinterpret_cast<HWND>(lp))>=ResultBase){SetTextColor(reinterpret_cast<HDC>(wp),PopupStyle::Text);SetBkColor(reinterpret_cast<HDC>(wp),PopupStyle::ResultBackground);static HBRUSH background=CreateSolidBrush(PopupStyle::ResultBackground);return reinterpret_cast<LRESULT>(background);}
        if(m==WM_CTLCOLOREDIT){SetTextColor(reinterpret_cast<HDC>(wp),PopupStyle::Text);SetBkColor(reinterpret_cast<HDC>(wp),PopupStyle::InputBackground);static HBRUSH background=CreateSolidBrush(PopupStyle::InputBackground);return reinterpret_cast<LRESULT>(background);}
        if(m==WM_DRAWITEM){self->DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;}
        if(m==WM_NOTIFY){auto* header=reinterpret_cast<NMHDR*>(lp);if(header->code==TTN_GETDISPINFOW){auto* tip=reinterpret_cast<NMTTDISPINFOW*>(lp);if(header->idFrom==StatusTip){tip->lpszText=self->status.data();return 0;}tip->lpszText=const_cast<wchar_t*>(self->pinned?L"取消置顶":L"置顶保留窗口");return 0;}}
        if(m==WM_NCDESTROY){self->window=nullptr;self->editor=nullptr;SetWindowLongPtrW(w,GWLP_USERDATA,0);}
        return DefWindowProcW(w,m,wp,lp);
    }
};
TranslationController::TranslationController():impl_(std::make_unique<Impl>()){}
TranslationController::~TranslationController()=default;
void TranslationController::Configure(HWND owner,bool enabled,std::function<bool()> canCapture,bool showErrors){Configure(owner,enabled,std::move(canCapture),showErrors,hotkeys::Defaults);}
void TranslationController::Configure(HWND owner,bool enabled,std::function<bool()> canCapture,bool showErrors,const hotkeys::Bindings& bindings){
    auto& s=*impl_;s.owner=owner;s.enabled=enabled;s.canCapture=std::move(canCapture);
    try{s.LoadSources();}catch(...){if(showErrors)MessageBoxW(owner,capture::CurrentError(L"读取翻译源配置失败").c_str(),L"PcTool",MB_OK|MB_ICONWARNING);}
    std::wstring failed;
    for(int i=0;i<3;++i){
        if((!enabled||s.bindings[i+2]!=bindings[i+2]||hotkeys::Empty(bindings[i+2]))&&s.keys[i]){UnregisterHotKey(owner,ManualHotkey+i);s.keys[i]=false;}
        if(enabled&&!s.keys[i]&&!hotkeys::Empty(bindings[i+2])){s.keys[i]=RegisterHotKey(owner,ManualHotkey+i,bindings[i+2].modifiers|MOD_NOREPEAT,bindings[i+2].key)!=FALSE;if(!s.keys[i]){if(!failed.empty())failed+=L"、";failed+=hotkeys::Text(bindings[i+2]);}}
    }
    s.bindings=bindings;
    if(showErrors&&!failed.empty())MessageBoxW(owner,(L"无法注册翻译快捷键："+failed+L"。可能已被其他软件占用，其余已注册入口仍可用。").c_str(),L"PcTool · 中英翻译",MB_OK|MB_ICONWARNING);
}
bool TranslationController::HandleHotkey(WPARAM id){
    if(id<ManualHotkey||id>SelectionHotkey)return false;
    auto& s=*impl_;if(!s.enabled)return true;
    Invoke(static_cast<Entry>(id-ManualHotkey));
    return true;
}
void TranslationController::Invoke(Entry entry,bool fromMenu,shared_platform::MenuSource source){
    auto& s=*impl_;try{
        s.menuCaptureDue=0;
        if(entry==Entry::Input)s.Manual();
        else if(entry==Entry::Capture){
            if(fromMenu){
                // A dismissed native menu can remain in DWM's selection fade even
                // after its HWND becomes invisible. Let that animation finish;
                // the normal UI timer keeps this wait cancellable and nonblocking.
                s.Hide();s.Ensure();s.menuCaptureDue=GetTickCount64()+300;
            }else s.Capture();
        }else if(entry==Entry::Selection)s.FromSelection(fromMenu,source);
    }
    catch(...){MessageBoxW(s.owner,capture::CurrentError(L"无法打开中英翻译").c_str(),L"PcTool",MB_OK|MB_ICONERROR);}
}
bool TranslationController::FocusCapture(){return impl_->selection.Focus();}
void TranslationController::Shutdown(){impl_->Shutdown();}
void TranslationController::ShowSourceSettings(HWND owner){try{impl_->OpenSettings(owner);}catch(...){MessageBoxW(owner,capture::CurrentError(L"无法打开翻译源设置").c_str(),L"PcTool",MB_OK|MB_ICONWARNING);}}
void TranslationController::SetProviders(std::vector<std::shared_ptr<TranslationProvider>> providers){impl_->sourcesLoaded=true;impl_->ApplyProviders(std::move(providers));}
}
