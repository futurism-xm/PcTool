#include "shared/platform/app_storage.h"
#include "recording/recording_session.h"
#include "recording/screen_recorder.h"
#include "shared/ui/toolbar_icons.h"
#include "shared/annotation/annotation_style.h"
#include <objidl.h>
#include <gdiplus.h>
#include <imm.h>
#include <winrt/base.h>

#include <filesystem>
#include <thread>
#include <array>

namespace capture {
namespace {
constexpr COLORREF keyColor=RGB(1,2,3);
constexpr UINT PointerUpdateMessage=WM_APP+42;
enum class Phase { Prepare, Initializing, Countdown, Recording, Stopping, Finished };
constexpr int Pen=101, Box=102, Oval=103, Arrow=104, TextTool=105, Laser=106,
    Undo=107, Clear=108, System=109, Mic=110, Pause=111, Cancel=112, Finish=113, Cursor=114, Fps=115;
struct Button { int id; RECT rect; std::wstring tip; };
class Session final : public RecordingSession {
public:
    Session(RECT region,std::function<void(bool)> completion,std::function<void(RecordingResult)> result,RecordingFormat format):format_(format),region_(region),completion_(std::move(completion)),result_(std::move(result)),renderer_(GetDpiForSystem()) {
        try {
        std::wstring error; if(!CheckCaptureRegion(region,error)) throw std::runtime_error(winrt::to_string(error));
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromRect(&region,MONITOR_DEFAULTTONEAREST),&mi); monitor_=mi.rcMonitor;
        if(!Create(Gif()?L"PcTool · GIF 录制":L"PcTool · 屏幕录制",290,48,WS_POPUP,WS_EX_TOPMOST|WS_EX_TOOLWINDOW)) winrt::throw_last_error();
        if(Gif()) { state_.systemEnabled=false; state_.micEnabled=false; }
        Exclude(window_);

        WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=SurfaceProc;
        wc.lpszClassName=L"PcTool.RecordingSurface"; wc.hCursor=LoadCursorW(nullptr,IDC_CROSS); RegisterClassW(&wc);
        shade_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
            wc.lpszClassName,L"PcTool · 录屏准备遮罩",WS_POPUP,monitor_.left,monitor_.top,monitor_.right-monitor_.left,monitor_.bottom-monitor_.top,nullptr,nullptr,wc.hInstance,this);
        display_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
            wc.lpszClassName,L"PcTool · 录屏边框",WS_POPUP,monitor_.left,monitor_.top,monitor_.right-monitor_.left,monitor_.bottom-monitor_.top,nullptr,nullptr,wc.hInstance,this);
        // A layered child inherits capture exclusion from display_, while its
        // alpha and transparent hit testing work across application threads.
        // Windows 10 rejects affinity on a separate UpdateLayeredWindow popup.
        annotations_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
            wc.lpszClassName,L"PcTool · 录屏平滑标注",WS_CHILD,region_.left-monitor_.left,region_.top-monitor_.top,Width(),Height(),display_,nullptr,wc.hInstance,this);
        pointerWindow_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
            wc.lpszClassName,L"PcTool · 录屏激光光点",WS_CHILD,0,0,21,21,display_,nullptr,wc.hInstance,this);
        input_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED,wc.lpszClassName,L"PcTool · 录屏标注",WS_POPUP,0,0,1,1,window_,nullptr,wc.hInstance,this);
        if(!display_ || !input_ || !annotations_ || !pointerWindow_ || !shade_) winrt::throw_last_error();
        SetLayeredWindowAttributes(display_,keyColor,255,LWA_COLORKEY|LWA_ALPHA);
        SetLayeredWindowAttributes(shade_,0,105,LWA_ALPHA);
        SetLayeredWindowAttributes(input_,0,1,LWA_ALPHA);
        UpdateAnnotationSurface(RecordingAnnotations{});
        Exclude(display_); Exclude(input_); Exclude(shade_);
        Layout(); UpdateInput(); ShowWindow(display_,SW_SHOWNOACTIVATE); ShowWindow(window_,SW_SHOW);
        SetTimer(window_,1,33,nullptr);
        } catch(...) {
            completion_={}; StopPointerTracking(); for(HWND w:{input_,pointerWindow_,annotations_,display_,shade_}) if(w && IsWindow(w)) DestroyWindow(w);
            input_=display_=annotations_=pointerWindow_=shade_=nullptr; throw;
        }
    }
    ~Session() override {
        StopPointerTracking();
        completion_={}; state_.RequestStop(RecordingStopReason::Shutdown);
        if(worker_.joinable()) worker_.join();
        if(!discardHandled_ && state_.finished && state_.stopReason==RecordingStopReason::Discard) {
            const auto error=DiscardRecordingFiles(state_);
            if(!error.empty()) MessageBoxW(window_,error.c_str(),L"PcTool · 放弃录制",MB_OK|MB_ICONERROR);
        }
        if(tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_); tooltip_=nullptr;
        panel_.reset(); CloseText(false);
        for(HWND w:{input_,pointerWindow_,annotations_,display_,shade_}) if(w) DestroyWindow(w);
        if(window_) DestroyWindow(window_);
       
    }
    bool Active() const override { return Window() && phase_!=Phase::Finished; }
private:
    static void Exclude(HWND w) {
        if(!SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE)) throw std::runtime_error(winrt::to_string(ErrorMessage(L"无法排除录屏窗口",HRESULT_FROM_WIN32(GetLastError()))+L"\n"+WindowText(w)));
    }
    bool Gif() const {return format_==RecordingFormat::Gif;}
    int Width() const { return region_.right-region_.left; }
    int Height() const { return region_.bottom-region_.top; }
    void Complete(bool success) { auto callback=std::move(completion_); if(callback) callback(success); }
    void ResetTooltips() {
        if(!tooltip_) return;
        SendMessageW(tooltip_,TTM_POP,0,0);
        for(int id:tooltipIds_) { TOOLINFOW tool{sizeof(tool)}; tool.hwnd=window_; tool.uId=id; SendMessageW(tooltip_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tool)); }
        tooltipIds_.clear();
    }
    void UpdateTooltips() {
        if(!tooltip_) tooltip_=CreateToolbarTooltipWindow(window_,dpi_,true);
        if(!tooltip_) return;
        SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,Dip(240));
        for(const auto& button:buttons_) {
            TOOLINFOW tool{sizeof(tool)}; tool.hwnd=window_; tool.uId=button.id; tool.uFlags=TTF_SUBCLASS;
            tool.rect=button.rect; tool.lpszText=LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool)); tooltipIds_.push_back(button.id);
        }
    }
    void Layout(bool autoPlace=true) {
        ResetTooltips(); buttons_.clear(); separators_.clear(); int x=0; int h=Dip(ToolbarHeightDip);
        auto add=[&](int id,const wchar_t* tip,int width=ToolbarButtonDip) { buttons_.push_back({id,{x,0,x+Dip(width),Dip(ToolbarHeightDip)},tip}); x+=Dip(width); };
        auto separate=[&] { separators_.push_back(x-1); };
        {
            if(phase_==Phase::Recording || phase_==Phase::Stopping) {
                add(Pen,L"画笔"); add(Box,L"矩形"); add(Oval,L"椭圆"); add(Arrow,L"箭头"); add(TextTool,L"文本");
                separate(); add(Undo,L"撤销"); add(Clear,L"清空标注"); separate();
            }
            if(Gif()) { add(Fps,L"帧率（录制前调整）",160); }
            else { add(System,state_.systemEnabled?L"禁用系统声音":L"开启系统声音");
            add(Mic,state_.micEnabled?L"禁用麦克风":L"开启麦克风"); }
            add(Cursor,state_.cursorEnabled?L"显示光标":L"隐藏光标"); add(Laser,L"激光指示");
            if(phase_==Phase::Recording || phase_==Phase::Stopping) add(Pause,state_.paused?L"继续":L"暂停");
            add(Cancel,phase_==Phase::Recording || phase_==Phase::Stopping?L"放弃录制（不保存）":L"取消"); add(Finish,phase_==Phase::Prepare?L"开始录制":L"结束录制",phase_==Phase::Recording || phase_==Phase::Stopping?120:96);
        }
        const int maxWidth=monitor_.right-monitor_.left;
        if(x>maxWidth) {
            separators_.clear(); int next=0,row=0,maxUsed=0;
            for(auto& button:buttons_) {
                const int width=button.rect.right-button.rect.left;
                if(next+width>maxWidth && next>0) { ++row; next=0; }
                button.rect={next,Dip(row*ToolbarHeightDip),next+width,Dip((row+1)*ToolbarHeightDip)};
                next+=width; maxUsed=std::max(maxUsed,next);
            }
            x=std::min(maxWidth,maxUsed); h+=Dip(row*ToolbarHeightDip);
        }
        RECT old{}; GetWindowRect(window_,&old);
        int left=old.left,top=old.top;
        if(autoPlace && !barMoved_) {
            left=region_.right-x; top=region_.bottom+Dip(8);
            if(top+h>monitor_.bottom) top=region_.top-h-Dip(8);
            if(top<monitor_.top) top=region_.bottom-h-Dip(8);
        }
        left=std::clamp(left,int(monitor_.left),std::max(int(monitor_.left),int(monitor_.right)-x));
        top=std::clamp(top,int(monitor_.top),std::max(int(monitor_.top),int(monitor_.bottom)-h));
        SetWindowPos(window_,HWND_TOPMOST,left,top,x,h,SWP_NOACTIVATE);
        InvalidateRect(window_,nullptr,FALSE); InvalidateRect(display_,nullptr,FALSE);
        UpdatePanel(); UpdateShade(); UpdateTooltips();
    }
    void UpdateInput() {
        UpdatePointerMode();
        const bool prepare=phase_==Phase::Prepare;
        const bool draw=phase_==Phase::Recording && tool_!=0;
        if(prepare || draw) {
            const int pad=prepare?Dip(8):0;
            SetWindowPos(input_,HWND_TOPMOST,region_.left-pad,region_.top-pad,Width()+pad*2,Height()+pad*2,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            SetWindowPos(window_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            if(draw) SetForegroundWindow(input_);
        } else { if(GetCapture()==input_) ReleaseCapture(); ShowWindow(input_,SW_HIDE); }
        InvalidateRect(display_,nullptr,FALSE);
    }
    void Publish() {
        UpdatePointerMode();
        auto snapshot=std::make_shared<RecordingAnnotations>(); snapshot->version=++version_; snapshot->dpi=dpi_; snapshot->shapes=shapes_;
        if(drawing_) snapshot->shapes.push_back(working_);
        if(textHost_ && !draft_.text.empty()) snapshot->shapes.push_back(draft_);
        { std::lock_guard<std::mutex> lock(state_.mutex); state_.annotations=snapshot; }
        if(phase_==Phase::Recording) UpdateAnnotationSurface(*snapshot);
        else ShowWindow(annotations_,SW_HIDE);
        for(const auto& button:buttons_) if(button.id==Undo || button.id==Clear)
            InvalidateRect(window_,&button.rect,FALSE);
    }
    void UpdateAnnotationSurface(const RecordingAnnotations& snapshot) {
        if(snapshot.shapes.empty() && !snapshot.laser.visible) { ShowWindow(annotations_,SW_HIDE); return; }
        // Bound the transparent surface to actual ink; allocating two monitor-
        // sized mattes on every pointer event causes avoidable drawing latency.
        RECT ink{Width(),Height(),0,0};
        const auto include=[&](POINT p,int padding) {
            ink.left=std::min(ink.left,p.x-padding); ink.top=std::min(ink.top,p.y-padding);
            ink.right=std::max(ink.right,p.x+padding+1); ink.bottom=std::max(ink.bottom,p.y+padding+1);
        };
        for(const auto& shape:snapshot.shapes) {
            const int padding=std::max(Dip(32),shape.size*4);
            include(shape.start,padding); include(shape.end,padding);
            for(POINT p:shape.points) include(p,padding);
        }
        if(snapshot.laser.visible) include(snapshot.laser.point,2*snapshot.laser.radius+2);
        ink.left=std::clamp<LONG>(ink.left,0,Width()-1); ink.top=std::clamp<LONG>(ink.top,0,Height()-1);
        ink.right=std::clamp<LONG>(ink.right,ink.left+1,Width()); ink.bottom=std::clamp<LONG>(ink.bottom,ink.top+1,Height());
        auto surface=RenderRecordingSurface(ink.right-ink.left,ink.bottom-ink.top,snapshot,renderer_,-ink.left,-ink.top);
        Image pixels; pixels.width=surface.width; pixels.height=surface.height; pixels.pixels=std::move(surface.pixels);
        PresentOverlay(pixels,{ink.left,ink.top});
    }
    void PresentOverlay(const Image& pixels,POINT offset={},HWND target=nullptr) {
        if(!target) target=annotations_;
        HBITMAP bitmap=ToBitmap(pixels); if(!bitmap) throw std::bad_alloc();
        HDC memory=CreateCompatibleDC(nullptr); auto old=SelectObject(memory,bitmap);
        POINT destination{region_.left+offset.x,region_.top+offset.y},source{}; SIZE size{pixels.width,pixels.height};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        const BOOL updated=UpdateLayeredWindow(target,nullptr,&destination,&size,memory,&source,0,&blend,ULW_ALPHA);
        const DWORD error=GetLastError(); SelectObject(memory,old); DeleteDC(memory); DeleteObject(bitmap);
        if(!updated) winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        if(phase_==Phase::Recording || phase_==Phase::Countdown) ShowWindow(target,SW_SHOWNOACTIVATE);
    }
    static LRESULT CALLBACK PointerHook(int code,WPARAM message,LPARAM data) {
        auto* session=pointerOwner_;
        if(code==HC_ACTION && session) {
            session->pointerPosition_=reinterpret_cast<MSLLHOOKSTRUCT*>(data)->pt;
            if(!session->pointerPending_)
                session->pointerPending_=PostMessageW(session->window_,PointerUpdateMessage,0,0)!=FALSE;
        }
        return CallNextHookEx(nullptr,code,message,data);
    }
    void StopPointerTracking() {
        if(pointerHook_) { UnhookWindowsHookEx(pointerHook_); pointerHook_=nullptr; }
        if(pointerOwner_==this) pointerOwner_=nullptr;
        if(window_) KillTimer(window_,2);
        pointerPending_=false;
        if(pointerWindow_) ShowWindow(pointerWindow_,SW_HIDE);
    }
    void UpdatePointerMode() {
        const bool enabled=(phase_==Phase::Recording || phase_==Phase::Prepare) && state_.cursorEnabled && laserEnabled_ && !state_.stop && !state_.finished;
        { std::lock_guard<std::mutex> lock(state_.mutex); state_.laserMode=enabled; if(!enabled) state_.liveLaser={}; }
        if(!enabled) { StopPointerTracking(); laser_={}; return; }
        if(!pointerHook_) {
            pointerHook_=SetWindowsHookExW(WH_MOUSE_LL,PointerHook,GetModuleHandleW(nullptr),0);
            if(pointerHook_) pointerOwner_=this;
            // Also handles stationary-pointer visibility changes and is a
            // fallback when low-level mouse observation is unavailable.
            SetTimer(window_,2,16,nullptr);
        }
        GetCursorPos(&pointerPosition_); UpdateLaser(pointerPosition_);
    }
    void UpdateLaser(POINT point) {
        if((phase_!=Phase::Recording && phase_!=Phase::Prepare) || !laserEnabled_ || !state_.cursorEnabled || state_.stop || state_.finished) return;
        RECT bar{}; GetWindowRect(window_,&bar);
        const bool visible=PtInRect(&region_,point) && !OverControl(point);
        point.x-=region_.left; point.y-=region_.top;
        const int radius=Dip(AnnotationLineSizes[laserStyle_.lineIndex]*2);
        if(pointerReady_ && laser_.color==laserStyle_.color && laser_.radius==radius && visible==laser_.visible && (!visible || (point.x==laser_.point.x && point.y==laser_.point.y))) return;
        if(laser_.color!=laserStyle_.color || laser_.radius!=radius) pointerReady_=false;
        laser_={point,visible,laserStyle_.color,radius};
        { std::lock_guard<std::mutex> lock(state_.mutex); state_.liveLaser=laser_; }
        if(!visible) { ShowWindow(pointerWindow_,SW_HIDE); return; }
        const int extent=2*radius+2,side=2*extent+1;
        if(!pointerReady_) {
            RecordingAnnotations dot; dot.laser={{extent,extent},true,laser_.color,radius};
            auto surface=RenderRecordingSurface(side,side,dot,renderer_);
            Image pixels; pixels.width=side; pixels.height=side; pixels.pixels=std::move(surface.pixels);
            PresentOverlay(pixels,{point.x-extent,point.y-extent},pointerWindow_); pointerReady_=true;
        }
        // Moving the cached laser sprite neither copies the annotation history
        // nor repaints its potentially screen-sized transparent surface.
        SetWindowPos(pointerWindow_,HWND_TOP,region_.left-monitor_.left+point.x-extent,
            region_.top-monitor_.top+point.y-extent,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    void UpdateCountdown() {
        const int digit=int(std::clamp<int64_t>((countdown_-ClockNow()+9999999)/10000000,1,3));
        if(digit==countdownDigit_) return;
        countdownDigit_=digit;
        Image pixels(Width(),Height()); std::fill(pixels.pixels.begin(),pixels.pixels.end(),0);
        {
            Gdiplus::Bitmap bitmap(Width(),Height(),Width()*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(pixels.pixels.data()));
            Gdiplus::Graphics g(&bitmap); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            const float radius=float(Dip(62)),cx=Width()/2.0f,cy=Height()/2.0f;
            Gdiplus::SolidBrush dark(Gdiplus::Color(210,35,39,44)),white(Gdiplus::Color(255,255,255,255));
            g.FillEllipse(&dark,cx-radius,cy-radius,radius*2,radius*2);
            Gdiplus::FontFamily family(L"Segoe UI"); Gdiplus::Font font(&family,float(Dip(70)),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
            Gdiplus::StringFormat format; format.SetAlignment(Gdiplus::StringAlignmentCenter); format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const auto label=std::to_wstring(digit); g.DrawString(label.c_str(),-1,&font,Gdiplus::RectF(cx-radius,cy-radius,radius*2,radius*2),&format,&white);
        }
        PresentOverlay(pixels);
    }
    void Begin() {
        std::wstring error; if(!CheckCaptureRegion(region_,error)) { MessageBoxW(window_,error.c_str(),L"PcTool",MB_OK|MB_ICONERROR); return; }
        RecordingOptions options; options.region=region_; options.format=format_; options.frameRate=frameRate_; options.path=app_storage::Unique(Gif()?L"Gif":L"Recordings",Gif()?L".gif":L".mp4").wstring(); options.waitForStart=true;
        options.systemAudio=state_.systemEnabled; options.microphone=state_.micEnabled;
        // Resolve defaults now so a later toggle cannot follow a changed system default.
        if(!Gif()) { options.systemDevice=DefaultAudioDevice(false); options.microphoneDevice=DefaultAudioDevice(true); }
        phase_=Phase::Initializing; UpdateInput(); Layout();
        try { worker_=std::thread([this,options]{RecordScreen(options,state_);}); }
        catch(...) { state_.error=CurrentError(L"无法启动录制"); state_.finished=true; }
    }
    void Stop(RecordingStopReason reason) {
        if(phase_==Phase::Stopping) return;
        state_.RequestStop(reason);
        CloseText(reason==RecordingStopReason::Save); drawing_=false; tool_=panelTool_=0; laserEnabled_=false;
        if(panel_) panel_->Hide();
        phase_=Phase::Stopping; Publish(); UpdateInput(); Layout();
    }
    void Tick() {
        if(phase_==Phase::Initializing && state_.ready && !state_.finished) { phase_=Phase::Countdown; countdown_=ClockNow()+30000000; Layout(); }
        if(phase_==Phase::Countdown) {
            if(ClockNow()>=countdown_) { phase_=Phase::Recording; ShowWindow(annotations_,SW_HIDE); UpdateInput(); state_.start=true; Layout(); }
            else UpdateCountdown();
            InvalidateRect(display_,nullptr,FALSE);
        }
        if(phase_==Phase::Recording) {
            InvalidateRect(window_,nullptr,FALSE);
        }
        if(state_.finished && phase_!=Phase::Finished) {
            if(worker_.joinable()) worker_.join();
            RecordingResult result; { std::lock_guard<std::mutex> lock(state_.mutex); result={state_.savedPath,state_.error,format_}; }
            StopPointerTracking(); CloseText(false); ShowWindow(display_,SW_HIDE); ShowWindow(input_,SW_HIDE); ShowWindow(annotations_,SW_HIDE); ShowWindow(shade_,SW_HIDE); if(panel_) panel_->Hide();
            phase_=Phase::Finished;
            if(state_.stopReason==RecordingStopReason::Discard) {
                discardHandled_=true;
                const auto deletionError=DiscardRecordingFiles(state_);
                if(!deletionError.empty()) MessageBoxW(window_,deletionError.c_str(),L"PcTool · 放弃录制",MB_OK|MB_ICONERROR);
                Complete(false); DestroyWindow(window_); return;
            }
            ShowWindow(window_,SW_HIDE); Complete(!result.path.empty());
            if(result_ && !result.path.empty()) {
                try { result_(result); }
                catch(...) { MessageBoxW(window_,(CurrentError(L"无法打开录制预览")+L"\n视频保留于："+result.path).c_str(),L"PcTool",MB_OK|MB_ICONERROR); }
            }
            else if(!result.error.empty()) MessageBoxW(window_,result.error.c_str(),L"PcTool · 录制失败",MB_OK|MB_ICONWARNING);
            DestroyWindow(window_);
        }
    }
    int Hit(POINT p) const { for(const auto& b:buttons_) if(PtInRect(&b.rect,p)) return b.id; return 0; }
    bool ButtonEnabled(int id) const {
        if(id==Fps) return phase_==Phase::Prepare;
        if(id==Laser && !state_.cursorEnabled) return false;
        if(id==Undo || id==Clear)
            return phase_==Phase::Recording && (!shapes_.empty() || (textHost_ && !draft_.text.empty()));
        return phase_==Phase::Prepare || phase_==Phase::Recording || id==Cancel;
    }
    bool DragEdge(POINT p) const {
        RECT r{}; GetClientRect(window_,&r); const int edge=Dip(4);
        return PtInRect(&r,p) && (p.x<=edge || r.right-p.x<=edge || p.y<=edge || r.bottom-p.y<=edge);
    }
    bool OverControl(POINT screen) const {
        struct Search {const Session* self;POINT point;bool found; } search{this,screen,false};
        EnumThreadWindows(GetWindowThreadProcessId(window_,nullptr),[](HWND candidate,LPARAM value)->BOOL {
            auto& s=*reinterpret_cast<Search*>(value);
            if(candidate==s.self->input_||candidate==s.self->display_||candidate==s.self->shade_||!IsWindowVisible(candidate))return TRUE;
            HWND owner=candidate;while(owner&&owner!=s.self->window_)owner=GetWindow(owner,GW_OWNER);
            RECT bounds{};if(owner&&GetWindowRect(candidate,&bounds)&&PtInRect(&bounds,s.point)){s.found=true;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&search));
        return search.found;
    }
    void UpdateShade() {
        if(!shade_) return;
        if(phase_!=Phase::Prepare && phase_!=Phase::Initializing) { ShowWindow(shade_,SW_HIDE); return; }
        HRGN outside=CreateRectRgn(0,0,monitor_.right-monitor_.left,monitor_.bottom-monitor_.top);
        HRGN inside=CreateRectRgn(region_.left-monitor_.left,region_.top-monitor_.top,region_.right-monitor_.left,region_.bottom-monitor_.top);
        CombineRgn(outside,outside,inside,RGN_DIFF); DeleteObject(inside);
        if(!SetWindowRgn(shade_,outside,TRUE)) DeleteObject(outside);
        SetWindowPos(shade_,display_,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    void UpdatePanel() {
        if((phase_!=Phase::Recording && !(phase_==Phase::Prepare && panelTool_==Laser)) || panelTool_<Pen || panelTool_>Laser) { if(panel_) panel_->Hide(); return; }
        if(!panel_) panel_=std::make_unique<AnnotationStylePanel>(window_,[this](AnnotationStyle style){
            if(panelTool_==Laser) { laserStyle_=style; pointerReady_=false; GetCursorPos(&pointerPosition_); UpdateLaser(pointerPosition_); }
            else {
                style_=style;
                if(textHost_) { draft_.color=style_.color; draft_.size=MulDiv(style_.fontPoints,int(dpi_),72); RefreshText(true); }
            }
            InvalidateRect(window_,nullptr,FALSE);
        },[this]{Escape();});
        RECT bar{}; GetWindowRect(window_,&bar); panel_->Update(panelTool_==Laser?laserStyle_:style_,panelTool_==TextTool,dpi_,bar,monitor_);
    }    void Command(int id) {
        if(!ButtonEnabled(id)) return;
        if(id==Cancel) {
            if(phase_==Phase::Prepare) { Complete(false); DestroyWindow(window_); }
            else if(phase_==Phase::Finished) DestroyWindow(window_);
            else Stop(RecordingStopReason::Discard);
            return;
        }
        if(phase_==Phase::Stopping || phase_==Phase::Finished || phase_==Phase::Initializing || phase_==Phase::Countdown) return;
        if(id==Cursor) {
            state_.cursorEnabled=!state_.cursorEnabled.load();
            if(!state_.cursorEnabled) { laserEnabled_=false; if(panelTool_==Laser) panelTool_=tool_; }
            UpdatePointerMode();
        }
        else if(id==Laser) {
            laserEnabled_=!laserEnabled_; panelTool_=laserEnabled_?Laser:tool_;
            UpdatePointerMode();
        }
        else if(id==System) state_.systemEnabled=!state_.systemEnabled.load();
        else if(id==Mic) state_.micEnabled=!state_.micEnabled.load();
        else if(id==Finish) { if(phase_==Phase::Prepare) Begin(); else Stop(RecordingStopReason::Save); return; }
        else if(phase_==Phase::Recording) {
            if(id>=Pen && id<=TextTool) { CloseText(true); drawing_=false; tool_=tool_==id?0:id; panelTool_=tool_; Publish(); UpdateInput(); }
            else if(id==Pause) state_.paused=!state_.paused.load();
            else if(id==Undo) { CloseText(true); if(!shapes_.empty()) shapes_.pop_back(); Publish(); }
            else if(id==Clear) { CloseText(false); shapes_.clear(); laser_={}; drawing_=false; Publish(); }
        }
        Layout(false);
    }
    void Escape() { if(panel_ && panel_->CloseFontDropdown()) return; if(phase_==Phase::Recording) { CloseText(true); drawing_=false; tool_=panelTool_=0; Publish(); UpdateInput(); UpdatePanel(); InvalidateRect(window_,nullptr,FALSE); } else if(phase_!=Phase::Finished) Command(Cancel); }
    void CloseText(bool commit) {
        if(!textHost_) return;
        if(edit_) draft_.text=WindowText(edit_);
        HWND old=textHost_; textHost_=nullptr;
        if(commit && !draft_.text.empty()) shapes_.push_back(draft_);
        DestroyWindow(old); edit_=nullptr; composing_=false; if(textFont_) { DeleteObject(textFont_); textFont_=nullptr; }
        Publish();
    }
    void RefreshText(bool fontChanged=false) {
        if(!edit_ || textUpdating_) return;
        textUpdating_=true; draft_.text=WindowText(edit_);
        renderer_.SetDpi(dpi_); renderer_.LayoutText(draft_,Width(),Height());
        const int width=std::max(1L,draft_.end.x-draft_.start.x),height=std::max(1L,draft_.end.y-draft_.start.y);
        if(fontChanged || !textFont_) {
            HFONT font=CreateFontW(-draft_.size,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei UI");
            SendMessageW(edit_,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            if(textFont_) DeleteObject(textFont_); textFont_=font;
        }
        SetWindowPos(textHost_,nullptr,region_.left+draft_.start.x-1,region_.top+draft_.start.y-1,width+2,height+2,SWP_NOZORDER|SWP_NOACTIVATE);
        MoveWindow(edit_,0,0,width,height,TRUE);
        RECT content{0,0,width,height}; SendMessageW(edit_,EM_SETRECT,0,reinterpret_cast<LPARAM>(&content));
        SendMessageW(edit_,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
        textUpdating_=false; UpdateIme(); Publish();
    }
    void UpdateIme() {
        if(!edit_) return; POINT caret{}; GetCaretPos(&caret);
        HIMC context=ImmGetContext(edit_); if(!context) return;
        COMPOSITIONFORM composition{}; composition.dwStyle=CFS_POINT; composition.ptCurrentPos=caret;
        ImmSetCompositionWindow(context,&composition);
        CANDIDATEFORM candidate{}; candidate.dwStyle=CFS_EXCLUDE; candidate.ptCurrentPos=caret;
        candidate.rcArea={caret.x,caret.y,caret.x+1,caret.y+draft_.size+Dip(4)};
        ImmSetCandidateWindow(context,&candidate); ImmReleaseContext(edit_,context);
    }
    void BeginText(POINT p) {
        CloseText(true); draft_={}; draft_.tool=Tool::Text; draft_.color=style_.color;
        draft_.size=MulDiv(style_.fontPoints,int(dpi_),72); draft_.start=p; draft_.end=p;
        renderer_.SetDpi(dpi_); renderer_.LayoutText(draft_,Width(),Height());
        textHost_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"PcTool.RecordingSurface",L"PcTool · 录屏文字",WS_POPUP|WS_BORDER,
            region_.left+draft_.start.x-1,region_.top+draft_.start.y-1,2,2,window_,nullptr,GetModuleHandleW(nullptr),this);
        if(!textHost_) winrt::throw_last_error(); Exclude(textHost_);
        edit_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,
            0,0,1,1,textHost_,reinterpret_cast<HMENU>(1),GetModuleHandleW(nullptr),nullptr);
        if(!edit_) winrt::throw_last_error();
        SetWindowSubclass(edit_,EditProc,1,reinterpret_cast<DWORD_PTR>(this)); RefreshText(true);
        ShowWindow(textHost_,SW_SHOW); SetFocus(edit_); UpdateIme();
    }
    static LRESULT CALLBACK EditProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<Session*>(data);
        if(m==WM_IME_STARTCOMPOSITION) self->composing_=true;
        if(m==WM_IME_ENDCOMPOSITION) self->composing_=false;
        if(m==WM_KEYDOWN && !self->composing_ && (wp==VK_ESCAPE || (wp==VK_RETURN && GetKeyState(VK_CONTROL)<0))) { self->Escape(); return 0; }
        if(m==WM_NCDESTROY) RemoveWindowSubclass(w,EditProc,1);
        const auto result=DefSubclassProc(w,m,wp,lp);
        if(m==WM_KEYUP || m==WM_LBUTTONUP || m==WM_IME_STARTCOMPOSITION) self->UpdateIme();
        return result;
    }    void Pointer(UINT m,LPARAM l) {
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ClientToScreen(input_,&p);
        if(phase_==Phase::Prepare) {
            if(m==WM_LBUTTONDOWN) { draggingSelection_=true; down_=p; original_=region_; handle_=renderer_.HitSelection(region_,p,monitor_); SetCapture(input_); }
            else if(m==WM_MOUSEMOVE && draggingSelection_) { region_=renderer_.AdjustSelection(original_,down_,p,handle_,monitor_); UpdateInput(); Layout(); }
            else if(m==WM_LBUTTONUP) { draggingSelection_=false; ReleaseCapture(); }
            return;
        }
        if(phase_!=Phase::Recording || !tool_) return;
        p.x=std::clamp<LONG>(p.x-region_.left,0,Width()-1); p.y=std::clamp<LONG>(p.y-region_.top,0,Height()-1);
        if(m==WM_LBUTTONDOWN) {
            if(tool_==TextTool) { BeginText(p); return; }
            drawing_=true; working_={}; working_.color=style_.color; working_.size=Dip(AnnotationLineSizes[style_.lineIndex]); working_.start=working_.end=p; working_.points={p};
            working_.tool=tool_==Pen?Tool::Pen:tool_==Box?Tool::Rectangle:tool_==Oval?Tool::Ellipse:Tool::Arrow; SetCapture(input_);
        }
        if(drawing_) {
            working_.end=p; if(tool_==Pen && (working_.points.back().x!=p.x || working_.points.back().y!=p.y)) working_.points.push_back(p);
            if(m==WM_LBUTTONUP) {
                const bool valid=tool_==Pen?working_.points.size()>1:(std::abs(p.x-working_.start.x)>=2 || std::abs(p.y-working_.start.y)>=2);
                if(valid) shapes_.push_back(working_); drawing_=false; ReleaseCapture();
            }
            Publish();
        }
    }
    void DrawIcon(HDC dc,const Button& b) {
        COLORREF color=ButtonEnabled(b.id)?ToolbarInk:ToolbarDisabled;
        if((b.id==System && !state_.systemEnabled)||(b.id==Mic && !state_.micEnabled)) color=ToolbarDisabled;
        ToolbarIcon icon=ToolbarIcon::None;
        switch(b.id) {
        case Pen: icon=ToolbarIcon::Pen; break;
        case Box: icon=ToolbarIcon::Rectangle; break;
        case Oval: icon=ToolbarIcon::Ellipse; break;
        case Arrow: icon=ToolbarIcon::Arrow; break;
        case TextTool: icon=ToolbarIcon::Text; break;
        case Laser: icon=ToolbarIcon::Laser; break;
        case Cursor: icon=state_.cursorEnabled?ToolbarIcon::Cursor:ToolbarIcon::CursorHidden; break;
        case Undo: icon=ToolbarIcon::Undo; break;
        case Clear: icon=ToolbarIcon::Clear; break;
        case System: icon=state_.systemEnabled?ToolbarIcon::System:ToolbarIcon::SystemMuted; break;
        case Mic: icon=state_.micEnabled?ToolbarIcon::Mic:ToolbarIcon::MicMuted; break;
        case Pause: icon=state_.paused?ToolbarIcon::Resume:ToolbarIcon::Pause; break;
        case Cancel: icon=ToolbarIcon::Cancel; break;
        }
        DrawToolbarIcon(dc,icon,b.rect,dpi_,color);
    }

    void PaintBar(HDC target) {
        RECT r{}; GetClientRect(window_,&r); HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,r.right,r.bottom); auto old=SelectObject(dc,bitmap);
        HBRUSH bg=CreateSolidBrush(ToolbarBackground); FillRect(dc,&r,bg); DeleteObject(bg); SetBkMode(dc,TRANSPARENT);
        HFONT font=CreateToolbarFont(dpi_); auto oldFont=SelectObject(dc,font);
        HBRUSH border=CreateSolidBrush(ToolbarBorder); FrameRect(dc,&r,border); DeleteObject(border);
        HBRUSH divider=CreateSolidBrush(ToolbarDivider);
        for(int x:separators_) { RECT line{x,Dip(8),x+1,Dip(ToolbarHeightDip-8)}; FillRect(dc,&line,divider); }
        DeleteObject(divider);
        for(const auto& b:buttons_) {
            if(b.id==Fps) {
                const int left=b.rect.left+Dip(10),right=b.rect.right-Dip(60),y=(b.rect.top+b.rect.bottom)/2;
                const int x=left+MulDiv(frameRate_-1,right-left,29);
                {
                    Gdiplus::Graphics graphics(dc);
                    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    Gdiplus::Color trackColor; trackColor.SetFromCOLORREF(ToolbarDivider);
                    Gdiplus::Pen track(trackColor,2.0f*dpi_/96.0f);
                    track.SetStartCap(Gdiplus::LineCapRound); track.SetEndCap(Gdiplus::LineCapRound);
                    graphics.DrawLine(&track,float(left),float(y),float(right),float(y));
                    const COLORREF color=ButtonEnabled(Fps)?RGB(45,159,240):ToolbarDisabled;
                    Gdiplus::Color thumbColor; thumbColor.SetFromCOLORREF(color);
                    Gdiplus::SolidBrush thumb(thumbColor);
                    const float radius=5.0f*dpi_/96.0f;
                    graphics.FillEllipse(&thumb,float(x)-radius,float(y)-radius,radius*2,radius*2);
                }
                RECT label{right+Dip(8),b.rect.top,b.rect.right,b.rect.bottom};SetTextColor(dc,ToolbarInk);
                const auto text=std::to_wstring(frameRate_)+L" FPS";DrawTextW(dc,text.c_str(),-1,&label,DT_SINGLELINE|DT_VCENTER|DT_CENTER);continue;
            }
            if(b.id==Finish) {
                RECT rect=b.rect; InflateRect(&rect,-Dip(2),-Dip(3)); std::wstring label;
                {
                    const bool recording=phase_==Phase::Recording || phase_==Phase::Stopping;
                    DrawToolbarButtonState(dc,b.rect,dpi_,phase_==Phase::Prepare || phase_==Phase::Recording,false,b.id==hover_,b.id==pressed_,recording?RGB(238,72,67):RGB(45,159,240)); SetTextColor(dc,RGB(255,255,255));
                    if(phase_==Phase::Prepare) label=L"开始录制";
                    else if(phase_==Phase::Initializing) label=L"正在初始化…";
                    else if(phase_==Phase::Countdown) label=L"准备录制";
                    else if(phase_==Phase::Stopping) label=state_.stopReason==RecordingStopReason::Discard?L"正在放弃…":L"正在封装…";
                    else { auto seconds=state_.elapsed.load()/10000000; wchar_t time[64]{}; swprintf_s(time,L"%02lld:%02lld:%02lld 结束",seconds/3600,seconds/60%60,seconds%60); label=time; }
                }
                DrawTextW(dc,label.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            } else {
                DrawToolbarButtonState(dc,b.rect,dpi_,ButtonEnabled(b.id),b.id==Laser?laserEnabled_:b.id==tool_,b.id==hover_,b.id==pressed_);
                DrawIcon(dc,b);
            }
        }
        SelectObject(dc,oldFont); DeleteObject(font); BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    }
    void PaintDisplay(HDC target) {
        RECT r{}; GetClientRect(display_,&r); HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,r.right,r.bottom); auto old=SelectObject(dc,bitmap);
        HBRUSH bg=CreateSolidBrush(keyColor); FillRect(dc,&r,bg); DeleteObject(bg);
        RECT selected=region_; OffsetRect(&selected,-monitor_.left,-monitor_.top);
        HPEN pen=CreatePen(PS_SOLID,std::max(1,Dip(1)),RGB(255,50,55)); auto oldPen=SelectObject(dc,pen); auto oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc,selected.left-1,selected.top-1,selected.right+1,selected.bottom+1); SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(pen);
        if(phase_==Phase::Prepare) {
            HBRUSH red=CreateSolidBrush(RGB(255,50,55));
            for(int x:{int(selected.left),int((selected.left+selected.right)/2),int(selected.right)}) for(int y:{int(selected.top),int((selected.top+selected.bottom)/2),int(selected.bottom)}) {
                if(x==(selected.left+selected.right)/2 && y==(selected.top+selected.bottom)/2) continue;
                RECT dot{x-Dip(2),y-Dip(2),x+Dip(3),y+Dip(3)}; FillRect(dc,&dot,red);
            }
            DeleteObject(red); SetBkColor(dc,RGB(35,40,44)); SetTextColor(dc,RGB(255,255,255));
            HFONT font=CreateFontW(-Dip(14),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei UI"); auto oldFont=SelectObject(dc,font);
            auto text=std::to_wstring(Width())+L" × "+std::to_wstring(Height()); TextOutW(dc,selected.left,std::max(0L,selected.top-Dip(23)),text.c_str(),int(text.size())); SelectObject(dc,oldFont); DeleteObject(font);
        }
        BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    }
    static LRESULT CALLBACK SurfaceProc(HWND w,UINT m,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Session*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE) { self=static_cast<Session*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if(!self) return DefWindowProcW(w,m,wp,lp);
        try {
            if(m==WM_PAINT) { PAINTSTRUCT ps{}; auto dc=BeginPaint(w,&ps); if(w==self->display_) self->PaintDisplay(dc); else if(w==self->annotations_) {} else { RECT r{}; GetClientRect(w,&r); FillRect(dc,&r,w==self->shade_?static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)):GetSysColorBrush(COLOR_WINDOW)); } EndPaint(w,&ps); return 0; }
            if(m==WM_PRINTCLIENT && w==self->display_) { self->PaintDisplay(reinterpret_cast<HDC>(wp)); return 0; }
            if(m==WM_NCHITTEST && (w==self->annotations_ || w==self->pointerWindow_)) return HTTRANSPARENT;
            if(m==WM_NCHITTEST && w==self->input_ && self->OverControl({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)})) return HTTRANSPARENT;
            if(m==WM_ERASEBKGND) return 1;
            if(m==WM_LBUTTONDOWN || m==WM_MOUSEMOVE || m==WM_LBUTTONUP) { if(w==self->input_) self->Pointer(m,lp); return 0; }
            if(m==WM_CAPTURECHANGED && w==self->input_) { self->draggingSelection_=false; if(self->drawing_) { self->drawing_=false; self->Publish(); } return 0; }
            if(m==WM_KEYDOWN && wp==VK_ESCAPE) { self->Escape(); return 0; }
            if(m==WM_CTLCOLOREDIT) { SetTextColor(reinterpret_cast<HDC>(wp),self->draft_.color); SetBkColor(reinterpret_cast<HDC>(wp),RGB(255,255,255)); return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH)); }
            if(m==WM_COMMAND && HIWORD(wp)==EN_CHANGE && w==self->textHost_) { self->RefreshText(); return 0; }
            if(m==WM_CLOSE) { self->Escape(); return 0; }
            if(m==WM_SETCURSOR && w==self->input_) {
                LPCWSTR cursor=self->tool_==TextTool?IDC_IBEAM:IDC_CROSS;
                if(self->phase_==Phase::Prepare) {
                    POINT point{}; GetCursorPos(&point); int handle=self->renderer_.HitSelection(self->region_,point,self->monitor_);
                    cursor=handle==1 || handle==3?IDC_SIZEWE:handle==2 || handle==4?IDC_SIZENS:handle==5 || handle==8?IDC_SIZENWSE:handle==6 || handle==7?IDC_SIZENESW:IDC_SIZEALL;
                }
                SetCursor(LoadCursorW(nullptr,cursor)); return TRUE;
            }
        } catch(...) { MessageBoxW(self->window_,CurrentError(L"录屏操作失败").c_str(),L"PcTool",MB_OK|MB_ICONERROR); }
        return DefWindowProcW(w,m,wp,lp);
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l) override {
        if(m==WM_NOTIFY && reinterpret_cast<NMHDR*>(l)->hwndFrom==tooltip_ && reinterpret_cast<NMHDR*>(l)->code==TTN_GETDISPINFOW) {
            auto* info=reinterpret_cast<NMTTDISPINFOW*>(l);
            const auto it=std::find_if(buttons_.begin(),buttons_.end(),[&](const Button& b){return UINT_PTR(b.id)==info->hdr.idFrom;});
            if(it!=buttons_.end()) info->lpszText=const_cast<wchar_t*>(it->tip.c_str());
            return 0;
        }
        if(m==WM_DESTROY) { tool_=panelTool_=0; laserEnabled_=false; StopPointerTracking(); if(shade_) ShowWindow(shade_,SW_HIDE); if(panel_) panel_->Hide(); if(display_) ShowWindow(display_,SW_HIDE); if(annotations_) ShowWindow(annotations_,SW_HIDE); if(input_) ShowWindow(input_,SW_HIDE); CloseText(false); return 0; }
        if(m==PointerUpdateMessage) { pointerPending_=false; UpdateLaser(pointerPosition_); return 0; }
        if(m==WM_TIMER) { if(w==2) { GetCursorPos(&pointerPosition_); UpdateLaser(pointerPosition_); } else Tick(); return 0; }
        if(m==WM_PAINT) { PAINTSTRUCT ps{}; auto dc=BeginPaint(window_,&ps); PaintBar(dc); EndPaint(window_,&ps); return 0; }
        if(m==WM_PRINTCLIENT) { PaintBar(reinterpret_cast<HDC>(w)); return 0; }
        if(m==WM_ERASEBKGND) return 1;
        if(m==WM_CAPTURECHANGED) { sliding_=false; pressed_=0; movingBar_=false; InvalidateRect(window_,nullptr,FALSE); return 0; }
        if(m==WM_COMMAND) { Command(LOWORD(w)); return 0; }
        if(m==WM_CLOSE) { Command(Cancel); return 0; }
        if(m==WM_KEYDOWN && w==VK_ESCAPE) { Escape(); return 0; }
        if(m==WM_SETCURSOR && LOWORD(l)==HTCLIENT) {
            POINT p{}; GetCursorPos(&p); ScreenToClient(window_,&p);
            SetCursor(LoadCursorW(nullptr,DragEdge(p)?IDC_SIZEALL:ButtonEnabled(Hit(p))?IDC_HAND:IDC_ARROW)); return TRUE;
        }
        if(m==WM_RBUTTONUP) { const int id=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); if(id>=Pen && id<=Laser && ButtonEnabled(id)) { if(id==Laser? !laserEnabled_:tool_!=id) Command(id); else { panelTool_=id; UpdatePanel(); } } return 0; }
        if(m==WM_LBUTTONDOWN || m==WM_LBUTTONDBLCLK) {
            POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; const int hit=Hit(p);
            if(hit==Fps && ButtonEnabled(Fps) && !DragEdge(p)) { sliding_=true;SetCapture(window_);Slide(p.x);return 0; }
            movingBar_=DragEdge(p) || !hit; pressed_=!movingBar_ && ButtonEnabled(hit)?hit:0;
            if(movingBar_) { GetCursorPos(&barDown_); GetWindowRect(window_,&barOriginal_); }
            if(tooltip_) SendMessageW(tooltip_,TTM_POP,0,0); InvalidateRect(window_,nullptr,FALSE); SetCapture(window_); return 0;
        }
        if(m==WM_MOUSEMOVE) {
            if(sliding_) { Slide(GET_X_LPARAM(l));return 0; }
            if(movingBar_) {
                POINT p{}; GetCursorPos(&p); const int width=barOriginal_.right-barOriginal_.left,height=barOriginal_.bottom-barOriginal_.top;
                const int x=std::clamp<int>(barOriginal_.left+p.x-barDown_.x,monitor_.left,std::max(monitor_.left,monitor_.right-width));
                const int y=std::clamp<int>(barOriginal_.top+p.y-barDown_.y,monitor_.top,std::max(monitor_.top,monitor_.bottom-height));
                SetWindowPos(window_,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); barMoved_=true; UpdatePanel();
            } else { int hit=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); if(hit!=hover_) { hover_=hit; InvalidateRect(window_,nullptr,FALSE); } TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,window_,0}; TrackMouseEvent(&t); }
            return 0;
        }
        if(m==WM_MOUSELEAVE) { hover_=0; if(tooltip_) SendMessageW(tooltip_,TTM_POP,0,0); InvalidateRect(window_,nullptr,FALSE); return 0; }
        if(m==WM_LBUTTONUP) { if(sliding_) { Slide(GET_X_LPARAM(l));sliding_=false;ReleaseCapture();return 0; } const int hit=Hit({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); const int command=pressed_; pressed_=0; movingBar_=false; ReleaseCapture(); InvalidateRect(window_,nullptr,FALSE); if(command && command==hit) Command(command); return 0; }
        if(m==WM_DPICHANGED) {
            auto result=ToolWindow::Handle(m,w,l); Layout(); UpdateInput();
            if(textHost_) RefreshText();
            if(phase_==Phase::Countdown) { countdownDigit_=0; UpdateCountdown(); }
            if(phase_==Phase::Recording) Publish();
            return result;
        }
        return ToolWindow::Handle(m,w,l);
    }
    Phase phase_{Phase::Prepare}; RECT region_{},monitor_{},original_{},barOriginal_{};
    void Slide(int x) {
        if(phase_!=Phase::Prepare)return;
        for(const auto& b:buttons_)if(b.id==Fps) {const int left=b.rect.left+Dip(10),right=b.rect.right-Dip(60);frameRate_=std::clamp(1+MulDiv(x-left,29,std::max(1,right-left)),1,30);InvalidateRect(window_,&b.rect,FALSE);break;}
    }
    RecordingFormat format_; int frameRate_{15}; bool sliding_{};
    HWND display_{},input_{},annotations_{},pointerWindow_{},shade_{},textHost_{},edit_{}; HFONT textFont_{};
    HHOOK pointerHook_{}; inline static thread_local Session* pointerOwner_{};
    POINT pointerPosition_{}; bool pointerPending_{},pointerReady_{};
    std::function<void(bool)> completion_; std::function<void(RecordingResult)> result_; std::unique_ptr<AnnotationStylePanel> panel_; AnnotationRenderer renderer_; RecordingState state_; std::thread worker_;
    HWND tooltip_{}; std::vector<int> tooltipIds_;
    std::vector<Button> buttons_; std::vector<int> separators_; AnnotationStyle style_,laserStyle_;
    int tool_{},panelTool_{}; bool laserEnabled_{};
    int hover_{},pressed_{},handle_{},countdownDigit_{};
    bool textUpdating_{},composing_{},drawing_{},draggingSelection_{},movingBar_{},barMoved_{},discardHandled_{}; POINT down_{},barDown_{};
    Annotation working_,draft_; std::vector<Annotation> shapes_; LaserPointer laser_; uint64_t version_{}; int64_t countdown_{};
};
}
std::unique_ptr<RecordingSession> OpenRecordingSession(RECT region,std::function<void(bool)> completion,std::function<void(RecordingResult)> result,RecordingFormat format) { return std::make_unique<Session>(region,std::move(completion),std::move(result),format); }
}










