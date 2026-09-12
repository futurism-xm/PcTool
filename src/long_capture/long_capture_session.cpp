#include "long_capture/scroll_stitcher.h"
#include "long_capture/long_capture_session.h"
#include "capture/image_editor.h"
#include "shared/ui/capture_overlay_ui.h"
#include <winrt/base.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <future>
namespace {
using namespace capture;
constexpr UINT MouseNotice=WM_APP+81;
constexpr int Edit=11,Cancel=12,Download=13,Finish=14;
constexpr uint64_t MinimumStepMs=350,PendingLifetimeMs=150;
// Hit-testable, almost transparent surface: cursor motion remains free, but
// the source never receives hover/click messages. Wheels are sent by the gate.
class InputShield final:public ToolWindow {
public:
    InputShield(HWND owner,RECT region){
        if(!Create(L"PcTool · 长截图输入保护",region.right-region.left,region.bottom-region.top,WS_POPUP,
            WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED))throw std::runtime_error("Cannot protect scroll input");
        SetWindowLongPtrW(window_,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(owner));
        if(!SetLayeredWindowAttributes(window_,0,1,LWA_ALPHA)||!SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE))throw std::runtime_error("Cannot exclude scroll input shield");
        SetWindowPos(window_,HWND_TOPMOST,region.left,region.top,region.right-region.left,region.bottom-region.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        UpdateWindow(window_);
    }
    bool OpenWheelPoint(POINT screen){
        RECT r{};GetClientRect(window_,&r);ScreenToClient(window_,&screen);
        if(!PtInRect(&r,screen))return false;
        HRGN full=CreateRectRgnIndirect(&r),point=CreateRectRgn(screen.x,screen.y,screen.x+1,screen.y+1);
        const int combined=CombineRgn(full,full,point,RGN_DIFF);DeleteObject(point);
        if(combined==ERROR||!SetWindowRgn(window_,full,FALSE)){DeleteObject(full);return false;}
        return true; // Windows now owns full.
    }
    void CloseWheelPoint(){SetWindowRgn(window_,nullptr,FALSE);}
protected:
    LRESULT Handle(UINT m,WPARAM w,LPARAM l)override{
        if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(m==WM_NCHITTEST)return HTCLIENT;
        if(m>=WM_MOUSEFIRST&&m<=WM_MOUSELAST)return 0;
        if(m==WM_MOUSEHOVER||m==WM_MOUSELEAVE)return 0;
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(window_,&ps);FillRect(dc,&ps.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));EndPaint(window_,&ps);return 0;}
        return ToolWindow::Handle(m,w,l);
    }
};
struct ScrollInput {
    uint64_t id{},time{},lastDelivery{}; POINT point{}; int direction{},units{};
    HWND source{},target{}; DWORD process{}; RECT sourceBounds{};
};
struct LongState {
    std::atomic<bool> finish{},cancel{},done{};
    mutable std::mutex mutex; Image initial,preview,result; int height{};
    uint64_t version{},imageVersion{},completedId{},cancelledId{}; bool failed{},limited{},atEnd{},atTop{},observed{},inFlight{},frameChanged{},baselineReady{};
    ScrollAnalysis analysis; ScrollInput request; std::vector<Annotation> annotations;
    std::optional<POINT> preparationAnchor;
    StitchResult last{StitchResult::Unchanged}; std::wstring status;
    LongCaptureMode mode{LongCaptureMode::Manual}; int restoreTargetY{}; UINT dpi{96};
};
bool Moved(StitchResult r){return r==StitchResult::Appended||r==StitchResult::Prepended||r==StitchResult::Revisited;}
bool ChangedImage(StitchResult r){return r==StitchResult::First||r==StitchResult::Appended||r==StitchResult::Prepended;}
bool Failure(StitchResult r){return r==StitchResult::Unmatched||r==StitchResult::RegionChanged||r==StitchResult::Limit;}
std::wstring SourceError(const ScrollInput& input){
    if(!input.source)return {};
    DWORD pid{};GetWindowThreadProcessId(input.source,&pid);
    if(!IsWindow(input.source)||pid!=input.process)return L"来源窗口已关闭，可编辑、下载或完成";
    RECT bounds{};GetWindowRect(input.source,&bounds);
    if(!EqualRect(&bounds,&input.sourceBounds)||!IsWindow(input.target)||GetAncestor(input.target,GA_ROOT)!=input.source)return L"来源窗口或布局已改变，请回到原区域继续";
    if(GetForegroundWindow()!=input.source)return L"来源窗口已失去焦点，请回到原滚动区域继续";
    if(IsHungAppWindow(input.source))return L"来源窗口暂时无响应，已暂停滚动";
    return {};
}
void CaptureLong(RECT region,LongState& state) {
    bool apartment=false;ScrollStitcher stitcher;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);apartment=true;
        BorderlessCapture capture(region,false);ScrollEndDetector boundary;
        Image candidate,examined,actionBaseline;uint64_t stableAt=GetTickCount64(),finishAt=0,seenId=0;
        bool limited=false,active=false,atEnd=false,atTop=false,liveFrame=false,baselineReady=false; uint64_t completingId{}; ScrollInput input;
        std::optional<POINT> anchor;std::wstring published;const auto start=GetTickCount64();
        auto publish=[&](StitchResult result,const std::wstring& status,bool imageChanged,bool force=false){
            if(!force&&!imageChanged&&!completingId&&status==published)return;published=status;
            Image thumbnail;if(imageChanged)thumbnail=stitcher.Preview(450,450);
            std::lock_guard<std::mutex> lock(state.mutex);
            if(completingId){
                if(completingId<=state.cancelledId){completingId=0;atEnd=atTop=false;return;}
                state.completedId=completingId;state.inFlight=false;completingId=0;
            }
            state.last=result;state.status=status;state.height=stitcher.Height();state.limited=limited;
            state.atEnd=atEnd;state.atTop=atTop;state.analysis=stitcher.Analysis();state.analysis.result=result;
            if(imageChanged){state.preview=std::move(thumbnail);++state.imageVersion;}++state.version;
        };
        // Publish the action acknowledgment together with its final coordinates/result.
        auto complete=[&]{active=false;completingId=input.id;};
        if(!state.initial.Empty()){candidate=state.initial;stitcher.Push(state.initial);state.initial={};publish(StitchResult::First,L"",true);}
        while(!state.cancel){
            const auto now=GetTickCount64();bool newAction=false;
            {std::lock_guard<std::mutex> lock(state.mutex);
                if(!baselineReady)anchor=state.preparationAnchor;
                if(state.request.id!=seenId){input=state.request;seenId=input.id;active=input.id>state.cancelledId;newAction=active;state.observed=true;state.frameChanged=false;}
                else if(active)input.lastDelivery=state.request.lastDelivery;
                if(active&&input.id<=state.cancelledId){active=false;boundary.Reset();}
            }
            if(newAction){anchor=POINT{input.point.x-region.left,input.point.y-region.top};actionBaseline=candidate;examined={};stableAt=now;boundary.Reset();atEnd=atTop=false;publish(StitchResult::Unchanged,L"",false,true);}
            Image frame;
            if(capture.Next(frame)){
                liveFrame=true;
                const auto area=stitcher.Analysis().locked?std::optional<POINT>{}:anchor;
                if(stitcher.Difference(candidate,frame,area)>0.8)stableAt=now;
                if(active){const bool changed=stitcher.Difference(actionBaseline,frame,area)>0.8;std::lock_guard<std::mutex> lock(state.mutex);state.frameChanged=changed;}
                candidate=std::move(frame);
            }
            // The selector's frozen image predates the shield and may contain a
            // hover panel or an older page layout. It is only a temporary preview.
            // Establish the first stitch frame from this same live capture stream
            // BEFORE permitting any wheel delivery; never rebase after scrolling.
            if(!baselineReady&&liveFrame&&now-start>=300&&now-stableAt>=200){
                stitcher=ScrollStitcher();const auto first=stitcher.Push(candidate);
                limited=first==StitchResult::Limit;examined={};
                publish(first,limited?L"已达到长图容量上限，可编辑、下载或完成":L"",true,true);
                baselineReady=true;
                std::lock_guard<std::mutex> lock(state.mutex);state.baselineReady=true;
            }
            if(!baselineReady&&now-start>=2500)
                publish(StitchResult::Unchanged,L"正在等待初始画面稳定，请稍候",false);
            const auto sourceError=SourceError(input);
            if(!sourceError.empty()){
                boundary.Reset();atEnd=atTop=false;if(active)complete();
                publish(StitchResult::RegionChanged,sourceError,false);
            }else if(baselineReady&&!candidate.Empty()&&!limited&&seenId&&now-stableAt>=200){
                StitchResult result=StitchResult::Unchanged;
                // Before region lock, an animation elsewhere is not evidence
                // that this wheel moved the requested list. In particular, don't
                // reject a queued pre-scroll frame as a failed first overlap.
                const bool targetChanged=stitcher.Analysis().locked||stitcher.Difference(actionBaseline,candidate,anchor)>0.8;
                if(targetChanged&&(examined.Empty()||stitcher.Difference(examined,candidate)>0.6)){result=stitcher.Push(candidate,anchor);examined=candidate;}
                if(Moved(result)){
                    boundary.Reset();atEnd=atTop=false;if(active)complete();
                    publish(result,L"",ChangedImage(result),true);
                }else if(Failure(result)){
                    boundary.Reset();atEnd=atTop=false;if(active)complete();
                    std::wstring status;
                    if(result==StitchResult::RegionChanged)status=L"滚动区域或布局已改变，请回到原区域继续";
                    else if(result==StitchResult::Limit){limited=true;status=L"已达到长图容量上限，可编辑、下载或完成";}
                    else status=stitcher.Analysis().ambiguous?L"内容图案重复，无法可靠定位，请调整选区":L"重叠不足或布局无法匹配，已暂停；请缓慢回到已截取位置";
                    publish(result,status,false,true);
                }else if(active){
                    // Only an actually dispatched complete action can confirm a boundary.
                    // Fragmentary native wheel delivery is completed by the UI controller.
                    int delivered{};{std::lock_guard<std::mutex> lock(state.mutex);delivered=state.request.units;}
                    if(delivered>=120){
                        boundary.Observe(input.id,input.lastDelivery,input.direction,StitchResult::Unchanged,true);
                        if(boundary.Ready(now,stableAt)){
                            boundary.Notified();atTop=input.direction>0;atEnd=!atTop;complete();
                            const auto text=!stitcher.Analysis().locked?L"未检测到可滚动内容":atTop?L"已到顶部":L"已到底部，可编辑、下载或完成";
                            publish(StitchResult::Unchanged,text,false,true);
                        }
                    }
                }
            }
            if(active&&now-input.time>=2500){
                boundary.Reset();atEnd=atTop=false;complete();publish(StitchResult::Unmatched,L"未能确认稳定的滚动位置，已暂停；等待画面稳定后继续",false,true);
            }
            if(state.finish){if(!finishAt)finishAt=now;if(now-finishAt>=300&&(!active||now-finishAt>=1500))break;}
            if(candidate.Empty()&&now-start>10000)throw winrt::hresult_error(E_ABORT,L"无法取得屏幕图像");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if(!state.cancel&&stitcher.Height()>0){state.result=stitcher.Flatten();stitcher.MapAnnotations(state.annotations,state.dpi);}
    }catch(...){std::lock_guard<std::mutex> lock(state.mutex);state.status=CurrentError(L"长截图采集失败");state.failed=true;
        if(!state.cancel&&stitcher.Height()>0){try{state.result=stitcher.Flatten();stitcher.MapAnnotations(state.annotations,state.dpi);}catch(...){}}}
    if(apartment)winrt::uninit_apartment();state.done=true;
}
class SessionWindow final:public LongCaptureSession {
public:
    SessionWindow(RECT region,std::shared_ptr<ImageDocument> document,std::function<void(bool)> completion,std::function<void(std::shared_ptr<ImageDocument>)> open)
        :region_(region),document_(std::move(document)),completion_(std::move(completion)),open_(std::move(open)) {
        if(document_->image.Empty()){auto frozen=FreezeDesktop();state_.initial=frozen.image.Crop(region.left-frozen.bounds.left,region.top-frozen.bounds.top,region.right-region.left,region.bottom-region.top);}
        else state_.initial=std::move(document_->image);
        if(!Create(L"PcTool · 长截图",300,200,WS_POPUP,WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))throw std::runtime_error("Cannot create long capture preview");
        if(!SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE))throw std::runtime_error("Cannot exclude preview");
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&region_,MONITOR_DEFAULTTONEAREST),&mi);work_=mi.rcWork;monitor_=mi.rcMonitor;
        try {
            surround_.Show(window_,region_,monitor_,dpi_);
            shield_=std::make_unique<InputShield>(window_,region_);
            toolbar_=std::make_unique<OverlayToolbar>(window_,L"PcTool · 长截图工具栏",std::vector<OverlayButton>{{Edit,ToolbarIcon::None,L"编辑",L"停止采集并编辑",48},{Download,ToolbarIcon::Save,L"",L"下载"},{Cancel,ToolbarIcon::Cancel,L"",L"取消"},{Finish,ToolbarIcon::Finish,L"",L"复制完整长图",68}},[this](int id){Command(id);});
            toolbar_->PlaceNear(region_,work_,dpi_);
            label_=std::make_unique<CaptureLabel>(window_,L"PcTool · 长图尺寸");hint_=std::make_unique<CaptureLabel>(window_,L"PcTool · 长图提示");
            hint_->Show(L"滚动鼠标轴或单击，开始截长图",region_,work_,dpi_,true);
            StartHooks();
            state_.annotations=document_->annotations;state_.dpi=document_->dpi;worker_=std::thread([this]{CaptureLong(region_,state_);});SetTimer(window_,1,30,nullptr);
        }catch(...){StopHooks();state_.cancel=true;if(worker_.joinable())worker_.join();throw;}
    }
    ~SessionWindow()override{completion_={};StopHooks();state_.cancel=true;if(worker_.joinable())worker_.join();shield_.reset();toolbar_.reset();hint_.reset();label_.reset();surround_.Close();if(window_)DestroyWindow(window_);}
    bool Active()const override{return Window()&&!completed_;}
    LongCaptureProgress Progress()const override{std::lock_guard<std::mutex> lock(state_.mutex);return {state_.height,state_.analysis,state_.status,state_.atEnd,state_.atTop,state_.mode,state_.inFlight,state_.request.id,state_.completedId,state_.restoreTargetY};}
private:
    void CancelAction(){
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.cancelledId=std::max(state_.cancelledId,state_.request.id);
        state_.completedId=std::max(state_.completedId,state_.request.id);state_.inFlight=false;inFlightId_=0;
    }
    void StartHooks(){
        std::promise<bool> ready;auto result=ready.get_future();
        inputWorker_=std::thread([this,ready=std::move(ready)]()mutable{
            // A dedicated pump keeps the low-level hook responsive while the UI
            // redraws a long preview. A stalled hook would let raw wheels escape.
            owner_=this;inputThreadId_=GetCurrentThreadId();MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
            mouse_=SetWindowsHookExW(WH_MOUSE_LL,MouseHook,GetModuleHandleW(nullptr),0);
            keyboard_=SetWindowsHookExW(WH_KEYBOARD_LL,KeyHook,GetModuleHandleW(nullptr),0);
            ready.set_value(mouse_&&keyboard_);
            if(mouse_&&keyboard_)while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
            if(mouse_)UnhookWindowsHookEx(mouse_);if(keyboard_)UnhookWindowsHookEx(keyboard_);mouse_=keyboard_=nullptr;owner_=nullptr;
        });
        if(!result.get()){if(inputWorker_.joinable())inputWorker_.join();throw std::runtime_error("Cannot observe scrolling capture input");}
    }
    void StopMouse(){automatic_=restoring_=false;pendingDirection_=0;CancelAction();mouseEnabled_=false;}
    void StopHooks(){StopMouse();if(inputWorker_.joinable()){PostThreadMessageW(inputThreadId_,WM_QUIT,0,0);inputWorker_.join();}}
    bool InControls(POINT p)const{RECT r{};return toolbar_&&GetWindowRect(toolbar_->Window(),&r)&&PtInRect(&r,p);}
    static LRESULT CALLBACK MouseHook(int code,WPARAM message,LPARAM data){
        auto* s=owner_;if(code==HC_ACTION&&s&&s->mouseEnabled_&&!s->state_.finish){auto* e=reinterpret_cast<MSLLHOOKSTRUCT*>(data);
            const bool inside=PtInRect(&s->region_,e->pt),controls=s->InControls(e->pt);
            if(message==WM_LBUTTONDOWN&&!inside&&!controls)PostMessageW(s->window_,MouseNotice,3,0);
            if(inside&&!controls){
                // Chromium synchronously hit-tests the wheel point. During the
                // short delivery aperture, do not let physical movement expose
                // the source to hover. Clicks remain consumed by the usual gate.
                if(message==WM_MOUSEMOVE&&s->wheelDispatching_)return 1;
                if(message==WM_LBUTTONDOWN||message==WM_LBUTTONUP){if(message==WM_LBUTTONDOWN)PostMessageW(s->window_,MouseNotice,1,MAKELPARAM(e->pt.x,e->pt.y));return 1;}
                if(message==WM_RBUTTONDOWN||message==WM_RBUTTONUP){if(message==WM_RBUTTONUP)PostMessageW(s->window_,WM_COMMAND,Cancel,0);return 1;}
                if(message==WM_MBUTTONDOWN||message==WM_MBUTTONUP||message==WM_XBUTTONDOWN||message==WM_XBUTTONUP||message==WM_MOUSEHWHEEL)return 1;
                if(message==WM_MOUSEWHEEL){const int delta=SHORT(HIWORD(e->mouseData));if(delta){std::lock_guard<std::mutex> lock(s->inputMutex_);s->wheelDirection_=delta>0?1:-1;s->wheelPoint_=e->pt;s->wheelTime_=GetTickCount64();if(!s->wheelNotice_){s->wheelNotice_=true;PostMessageW(s->window_,MouseNotice,2,0);}}return 1;}
            }
        }return CallNextHookEx(nullptr,code,message,data);
    }
    static LRESULT CALLBACK KeyHook(int code,WPARAM message,LPARAM data){auto* s=owner_;if(code==HC_ACTION&&s&&reinterpret_cast<KBDLLHOOKSTRUCT*>(data)->vkCode==VK_ESCAPE){if(message==WM_KEYDOWN)PostMessageW(s->window_,WM_COMMAND,Cancel,0);return 1;}return CallNextHookEx(nullptr,code,message,data);}
    void Hint(const std::wstring& text,int milliseconds=3000){if(!hint_)return;hint_->Show(text,region_,work_,dpi_,true);hintUntil_=GetTickCount64()+milliseconds;}
    void Pause(bool show=false){automatic_=restoring_=false;pendingDirection_=0;pendingReverse_=false;paused_=true;CancelAction();SetMode();if(show)Hint(L"已暂停自动滚动，单击选区继续",1600);}
    void SetMode(){std::lock_guard<std::mutex> lock(state_.mutex);state_.restoreTargetY=restoreTargetY_;state_.mode=state_.inFlight?LongCaptureMode::Waiting:restoring_?LongCaptureMode::Restoring:automatic_?LongCaptureMode::Automatic:paused_?LongCaptureMode::Paused:LongCaptureMode::Manual;}
    HWND TargetChild(HWND root,POINT screen){
        HWND parent=root;
        for(int depth=0;depth<16;++depth){POINT point=screen;ScreenToClient(parent,&point);HWND child=RealChildWindowFromPoint(parent,point);if(!child||child==parent||!IsWindowVisible(child)||!IsWindowEnabled(child))break;parent=child;}
        return parent;
    }
    HWND SourceBelowShield(POINT point)const{
        // Resolve the visible source without briefly hiding the shield (which
        // would deliver hover to the page). Never substitute an unrelated app.
        for(HWND w=GetTopWindow(nullptr);w;w=GetWindow(w,GW_HWNDNEXT)){
            if(!IsWindowVisible(w)||IsIconic(w)||GetAncestor(w,GA_ROOTOWNER)==window_||(GetWindowLongPtrW(w,GWL_EXSTYLE)&WS_EX_TRANSPARENT))continue;
            RECT bounds{};if(!GetWindowRect(w,&bounds)||!PtInRect(&bounds,point))continue;
            HRGN shape=CreateRectRgn(0,0,0,0);const int kind=GetWindowRgn(w,shape);
            const bool hit=kind==ERROR||PtInRegion(shape,point.x-bounds.left,point.y-bounds.top);DeleteObject(shape);
            if(hit)return w;
        }
        return nullptr;
    }
    bool ResolveTarget(POINT point,const ScrollAnalysis& analysis){
        if(analysis.locked){
            if(!target_.source||!IsWindow(target_.source)){Hint(L"来源窗口已关闭，可编辑、下载或完成");return false;}
            target_.point={region_.left+(analysis.region.left+analysis.region.right)/2,region_.top+(analysis.region.top+analysis.region.bottom)/2};
            // Keep the identified child, with a stable coordinate inside the locked body.
            return true;
        }
        HWND root=GetAncestor(WindowFromPoint(point),GA_ROOT);DWORD pid{};GetWindowThreadProcessId(root,&pid);
        if(!root||(pid==GetCurrentProcessId()&&(root==window_||(shield_&&root==shield_->Window())))){root=SourceBelowShield(point);GetWindowThreadProcessId(root,&pid);}
        if(!IsWindow(root)||root==window_||!pid){Hint(L"未检测到可滚动内容，请移到滚动区域操作");return false;}
        target_={};target_.point=point;target_.source=root;target_.target=TargetChild(root,point);target_.process=pid;GetWindowRect(root,&target_.sourceBounds);return true;
    }
    bool Dispatch(int direction,int units,uint64_t id,bool supplement=false){
        const auto error=SourceError(target_);if(!error.empty()){Pause();Hint(error);return false;}
        const auto now=GetTickCount64();ScrollInput request=target_;request.id=id;request.direction=direction;request.units=units;request.time=now;request.lastDelivery=now;
        {std::lock_guard<std::mutex> lock(state_.mutex);
            if(supplement){
                if(!state_.inFlight||state_.request.id!=id||state_.completedId>=id||state_.cancelledId>=id)return false;
                state_.request.units+=units;state_.request.lastDelivery=now;
            }
            else{state_.request=request;state_.inFlight=true;state_.frameChanged=false;state_.observed=true;state_.atEnd=state_.atTop=false;}
            // Queue delivery while holding the request lock: completion cannot race a supplement.
            const auto key=MAKEWPARAM(0,SHORT(direction*units));
            bool sent=false;wchar_t sourceClass[128]{};GetClassNameW(target_.source,sourceClass,128);
            if(std::wstring_view(sourceClass).find(L"Chrome_WidgetWin_")==0){
                // Chromium's RerouteMouseWheel drops messages when an unrelated
                // overlay is returned by WindowFromPoint(lParam). Reveal only
                // that one hit-test pixel until bounded synchronous delivery
                // returns; never hide the shield or move the user's cursor.
                wheelDispatching_=true;
                if(shield_&&shield_->OpenWheelPoint(target_.point)){
                    if(GetAncestor(WindowFromPoint(target_.point),GA_ROOT)==target_.source){
                        DWORD_PTR result{};sent=SendMessageTimeoutW(target_.target,WM_MOUSEWHEEL,key,MAKELPARAM(target_.point.x,target_.point.y),SMTO_ABORTIFHUNG|SMTO_BLOCK|SMTO_ERRORONEXIT,100,&result)!=0;
                    }
                    shield_->CloseWheelPoint();
                }
                wheelDispatching_=false;
            }else sent=PostMessageW(target_.target,WM_MOUSEWHEEL,key,MAKELPARAM(target_.point.x,target_.point.y))!=0;
            if(!sent){
                state_.inFlight=false;state_.completedId=id;state_.cancelledId=id;
            }else{
                lastDispatch_=now;if(!supplement){inFlightId_=id;issuedDirection_=direction;issuedUnits_=units;issuedAt_=now;}
                else issuedUnits_+=units;
                state_.mode=LongCaptureMode::Waiting;
                return true;
            }
        }
            Pause();Hint(L"无法向来源窗口发送滚动，已暂停");return false;
    }
    void RequestManual(int direction,POINT point,uint64_t time){
        {std::lock_guard<std::mutex> lock(state_.mutex);state_.preparationAnchor=POINT{point.x-region_.left,point.y-region_.top};}
        automatic_=restoring_=false;paused_=false;
        // A boundary acknowledgment may still be waiting for the UI timer.
        // Consume it before storing this NEW gesture, otherwise Advance clears
        // the new direction as if it belonged to the completed boundary action.
        if(inFlightId_&&Progress().completedActionId>=inFlightId_){pendingDirection_=0;Advance();}
        pendingReverse_=inFlightId_&&direction!=issuedDirection_;
        pendingDirection_=direction;pendingPoint_=point;pendingAt_=time;SetMode();Advance();
    }
    void ToggleAutomatic(POINT point){
        const auto latest=Progress();
        if(inFlightId_&&latest.completedActionId>=inFlightId_&&(latest.atTop||latest.atEnd)){automatic_=restoring_=false;pendingDirection_=0;Advance();}
        if(automatic_||restoring_){Pause(true);return;}
        auto progress=Progress();if(!ResolveTarget(point,progress.analysis)){Pause();return;}
        {std::lock_guard<std::mutex> lock(state_.mutex);state_.preparationAnchor=POINT{target_.point.x-region_.left,target_.point.y-region_.top};}
        SetForegroundWindow(target_.source);if(GetForegroundWindow()!=target_.source){Pause();Hint(L"无法激活目标窗口，已暂停");return;}
        const auto error=SourceError(target_);if(!error.empty()){Pause();Hint(error);return;}
        pendingDirection_=0;automatic_=true;paused_=false;restoreTargetY_=progress.analysis.locked?progress.analysis.maxY-progress.analysis.viewportHeight:0;
        restoring_=progress.analysis.locked&&progress.analysis.currentY<restoreTargetY_;
        Hint(restoring_?L"正在返回上次向下截取的位置，单击停止":L"正在自动向下截取，单击停止",1600);SetMode();Advance();
    }
    void Advance(){
        if(action_||completed_||state_.done)return;
        {std::lock_guard<std::mutex> lock(state_.mutex);if(!state_.baselineReady)return;}
        const auto now=GetTickCount64();auto progress=Progress();
        if(inFlightId_&&progress.completedActionId>=inFlightId_){
            if(Moved(progress.analysis.result)&&progress.analysis.displacement){
                pixelsPerUnit_=double(std::abs(progress.analysis.displacement))/std::max(1,issuedUnits_);
                if(progress.analysis.viewportHeight>0){const int desired=int(progress.analysis.viewportHeight/5.0/std::max(0.01,pixelsPerUnit_));wheelUnits_=std::clamp((desired/15)*15,15,60);}
            }
            inFlightId_=0;
            if(Failure(progress.analysis.result)){automatic_=restoring_=false;pendingDirection_=0;pendingReverse_=false;paused_=true;}
            else if(progress.atEnd||progress.atTop){
                automatic_=restoring_=false;paused_=true;
                if(!pendingReverse_)pendingDirection_=0;
            }
            if(automatic_){restoreTargetY_=progress.analysis.maxY-progress.analysis.viewportHeight;restoring_=progress.analysis.currentY<restoreTargetY_;}
            SetMode();
        }
        if(inFlightId_){
            bool changed{};uint64_t delivery{};{std::lock_guard<std::mutex> lock(state_.mutex);changed=state_.frameChanged;delivery=state_.request.lastDelivery;}
            // Legacy controls accumulate wheel deltas until a full 120-unit notch.
            // Complete that notch within the same action instead of reporting a false edge.
            if(issuedUnits_<120&&!changed&&now-delivery>=MinimumStepMs)Dispatch(issuedDirection_,120-issuedUnits_,inFlightId_,true);
            return;
        }
        if(now-lastDispatch_<MinimumStepMs)return;
        if(pendingDirection_){
            const int direction=pendingDirection_;pendingDirection_=0;
            // Preserve the one initial wheel intent while the live baseline is
            // being prepared. Later burst input keeps its normal short lifetime.
            if(lastDispatch_&&!pendingReverse_&&now-pendingAt_>PendingLifetimeMs){SetMode();return;}
            pendingReverse_=false;
            if(!ResolveTarget(pendingPoint_,progress.analysis)){Pause();return;}
            if(!Dispatch(direction,wheelUnits_,++inputId_))Pause();return;
        }
        if(automatic_){
            if(progress.analysis.locked){target_.point={region_.left+(progress.analysis.region.left+progress.analysis.region.right)/2,region_.top+(progress.analysis.region.top+progress.analysis.region.bottom)/2};restoreTargetY_=progress.analysis.maxY-progress.analysis.viewportHeight;restoring_=progress.analysis.currentY<restoreTargetY_;}
            int units=wheelUnits_;
            if(restoring_&&pixelsPerUnit_>0)units=std::clamp(int((restoreTargetY_-progress.analysis.currentY)/pixelsPerUnit_/15)*15,15,wheelUnits_);
            if(!Dispatch(-1,units,++inputId_))Pause();
        }
        SetMode();
    }
    void Complete(bool success){if(completed_)return;completed_=true;StopHooks();shield_.reset();surround_.Close();toolbar_.reset();hint_.reset();label_.reset();auto callback=std::move(completion_);if(callback)callback(success);}
    void Command(int id){
        if(completed_||action_)return;
        if(id==Cancel){action_=Cancel;StopHooks();state_.cancel=true;return;}
        if(id!=Edit&&id!=Download&&id!=Finish)return;
        action_=id;StopMouse();state_.finish=true;for(int command:{Edit,Download,Finish})toolbar_->State(command,false);Hint(L"正在生成长图…");
    }
    void PreviewLayout(){
        if(preview_.Empty())return;
        toolbar_->PlaceNear(region_,work_,dpi_);
        const int gap=Dip(8);int maxWidth=Dip(300),maxHeight=Dip(240);bool above=region_.top-work_.top>=Dip(90);
        bool right=!above&&work_.right-region_.right>=Dip(90),left=!above&&!right&&region_.left-work_.left>=Dip(90);
        if(above)maxHeight=std::min(maxHeight,int(region_.top-work_.top)-gap*2);
        else if(right)maxWidth=std::min(maxWidth,int(work_.right-region_.right)-gap*2);
        else if(left)maxWidth=std::min(maxWidth,int(region_.left-work_.left)-gap*2);
        else {maxWidth=Dip(140);maxHeight=Dip(160);}
        const double scale=std::min(double(std::max(1,maxWidth))/preview_.width,double(std::max(1,maxHeight))/preview_.height);
        int width=std::max(2,int(preview_.width*scale)),height=std::max(2,int(preview_.height*scale));
        int x=above?region_.right-width:right?region_.right+gap:left?region_.left-width-gap:work_.right-width-gap;
        int y=above?region_.top-height-gap:region_.top;
        x=std::clamp<int>(x,work_.left,std::max(work_.left,work_.right-width));y=std::clamp<int>(y,work_.top,std::max(work_.top,work_.bottom-height));
        const RECT controls=toolbar_->Bounds();RECT proposed{x,y,x+width,y+height},overlap{};
        if(IntersectRect(&overlap,&proposed,&controls)){
            if(controls.bottom+gap+height<=work_.bottom)y=controls.bottom+gap;
            else if(controls.top-gap-height>=work_.top)y=controls.top-gap-height;
        }
        SetWindowPos(window_,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);InvalidateRect(window_,nullptr,FALSE);
        RECT thumbnail{x,y,x+width,y+height};surround_.Show(window_,region_,monitor_,dpi_,&thumbnail);
        toolbar_->PlaceNear(region_,work_,dpi_);
    }
    void Tick(){
        if(processing_||completed_)return;
        struct Guard{bool& flag;Guard(bool& f):flag(f){flag=true;}~Guard(){flag=false;}} guard(processing_);
        int height{};bool observed{};uint64_t version{},imageVersion{};std::wstring status;StitchResult last;
        {std::lock_guard<std::mutex> lock(state_.mutex);height=state_.height;observed=state_.observed;version=state_.version;imageVersion=state_.imageVersion;status=state_.status;last=state_.last;if(imageVersion!=previewVersion_)preview_=state_.preview;}
        if(imageVersion!=previewVersion_){previewVersion_=imageVersion;PreviewLayout();label_->Show(std::to_wstring(region_.right-region_.left)+L" × "+std::to_wstring(height),region_,work_,dpi_,false);}
        if(version!=version_){version_=version;
            if(!status.empty()){if(Failure(last))Pause();Hint(status);}
            else if(observed){ShowWindow(hint_->Window(),SW_HIDE);hintUntil_=0;}
        }
        Advance();
        const auto now=GetTickCount64();if(hintUntil_&&now>=hintUntil_&&!action_){ShowWindow(hint_->Window(),SW_HIDE);hintUntil_=0;}
        if(state_.done&&!completed_){
            if(worker_.joinable())worker_.join();
            if(action_==Cancel||state_.cancel){Complete(false);DestroyWindow(window_);return;}
            if(!resultReady_&&state_.result.Empty()){hint_->Show(status.empty()?L"没有取得可用图像，请取消后重试":status,region_,work_,dpi_,true);action_=0;return;}
            if(!resultReady_){document_->image=std::move(state_.result);document_->annotations=std::move(state_.annotations);resultReady_=true;}
            if(state_.failed&&!failureReported_){failureReported_=true;MessageBoxW(window_,(status+L"\n已保留此前采集的图像。").c_str(),L"PcTool · 长截图",MB_OK|MB_ICONWARNING);}
            const int pending=std::exchange(action_,0);
            if(pending==Edit){ShowWindow(window_,SW_HIDE);try{open_(document_);}catch(...){ShowWindow(window_,SW_SHOWNOACTIVATE);action_=0;throw;}Complete(true);DestroyWindow(window_);return;}
            if(pending==Finish){auto output=RenderDocument(*document_);if(!CopyImageFile(window_,output)){action_=0;hint_->Show(L"复制失败，请检查剪贴板后重试",region_,work_,dpi_,true);}else{Complete(true);DestroyWindow(window_);return;}}
            else if(pending==Download){auto path=ChoosePath(window_,false);if(!path.empty()){if(SavePng(RenderDocument(*document_),path)){Complete(true);DestroyWindow(window_);return;}MessageBoxW(window_,L"保存失败，请检查路径或可用空间。",L"PcTool",MB_OK|MB_ICONERROR);}action_=0;}
            for(int command:{Edit,Download,Finish})toolbar_->State(command,true);
        }
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l)override{
        if(m==WM_TIMER){Tick();return 0;}
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT||m==WM_PRINTCLIENT){PAINTSTRUCT ps{};HDC dc=m==WM_PAINT?BeginPaint(window_,&ps):reinterpret_cast<HDC>(w);RECT r{};GetClientRect(window_,&r);if(!preview_.Empty()){BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),preview_.width,-preview_.height,1,32,BI_RGB};SetStretchBltMode(dc,HALFTONE);StretchDIBits(dc,0,0,r.right,r.bottom,0,0,preview_.width,preview_.height,preview_.pixels.data(),&bi,DIB_RGB_COLORS,SRCCOPY);}HBRUSH line=CreateSolidBrush(RGB(40,190,220));FrameRect(dc,&r,line);DeleteObject(line);if(m==WM_PAINT)EndPaint(window_,&ps);return 0;}
        if(m==MouseNotice){
            if(action_||state_.done)return 0;
            if(w==1)ToggleAutomatic({GET_X_LPARAM(l),GET_Y_LPARAM(l)});
            else if(w==2){int direction{};POINT point{};uint64_t time{};{std::lock_guard<std::mutex> lock(inputMutex_);wheelNotice_=false;direction=wheelDirection_;point=wheelPoint_;time=wheelTime_;}RequestManual(direction,point,time);}
            else if(w==3)Pause();return 0;
        }
        if(m==WM_COMMAND){Command(LOWORD(w));return 0;}
        if(m==WM_CLOSE){Command(Cancel);return 0;}
        if(m==WM_DPICHANGED){dpi_=HIWORD(w);surround_.Show(window_,region_,monitor_,dpi_);PreviewLayout();if(label_)label_->Scale(dpi_);if(hint_)hint_->Scale(dpi_);return 0;}
        return ToolWindow::Handle(m,w,l);
    }
    RECT region_{},work_{},monitor_{};CaptureSurround surround_;std::unique_ptr<InputShield> shield_;std::unique_ptr<OverlayToolbar> toolbar_;std::unique_ptr<CaptureLabel> label_,hint_;
    std::shared_ptr<ImageDocument> document_;std::function<void(bool)> completion_;std::function<void(std::shared_ptr<ImageDocument>)> open_;
    bool processing_{},completed_{},automatic_{},restoring_{},paused_{},resultReady_{},failureReported_{},pendingReverse_{},wheelNotice_{};
    int action_{},wheelDirection_{},pendingDirection_{},wheelUnits_{30},issuedDirection_{},issuedUnits_{},restoreTargetY_{};
    uint64_t version_{},previewVersion_{},hintUntil_{},inputId_{},inFlightId_{},lastDispatch_{},issuedAt_{},pendingAt_{},wheelTime_{};
    POINT wheelPoint_{},pendingPoint_{};double pixelsPerUnit_{};ScrollInput target_;
    LongState state_;Image preview_;std::thread worker_,inputWorker_;std::mutex inputMutex_;std::atomic<bool> mouseEnabled_{true},wheelDispatching_{false};DWORD inputThreadId_{};
    HHOOK mouse_{},keyboard_{};inline static thread_local SessionWindow* owner_{};
};
}
namespace capture {
std::unique_ptr<LongCaptureSession> OpenLongCaptureSession(RECT region,std::shared_ptr<ImageDocument> document,std::function<void(bool)> completion,std::function<void(std::shared_ptr<ImageDocument>)> open){return std::make_unique<SessionWindow>(region,std::move(document),std::move(completion),std::move(open));}
}
