#include "long_capture/long_capture_session.h"
#include "long_capture/scroll_trace.h"
#include <psapi.h>
#include <winrt/base.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

// Real input, Desktop Duplication and preview paint are measured together.
// The synthetic source provides an independent pixel/position oracle.
namespace {
using namespace capture;
using namespace capture::scroll_trace;
struct RestoreCursor {POINT point{};bool valid{GetCursorPos(&point)!=FALSE};~RestoreCursor(){if(valid)SetCursorPos(point.x,point.y);}};
struct Trace {
    std::mutex mutex;
    std::vector<Event> events;
    std::atomic<uint64_t> action{};
    static inline Trace* current{};
    static void Record(const Event& e) noexcept {
        auto* t=current;if(!t)return;
        if(e.stage==Stage::Dispatch)t->action=e.actionId;
        try {std::lock_guard<std::mutex> lock(t->mutex);t->events.push_back(e);}catch(...){std::terminate();}
    }
    Trace(){events.reserve(40000);current=this;SetCallback(Record);}
    ~Trace(){SetCallback(nullptr);current=nullptr;}
    std::vector<Event> Snapshot(){std::lock_guard<std::mutex> lock(mutex);return events;}
};
uint32_t Pixel(int x,int row){uint32_t s=uint32_t(row)*1664525u+uint32_t(x)*1013904223u;s^=s>>13;s*=1274126177u;return 0xff000000|(s&0xffffff);}
struct Source {
    HWND window{};int offset{3000},target{3000},remainder{},delay{},limit{100000};bool smooth{},whole{};
    uint64_t action{};int receives{},changes{};
    std::map<uint64_t,int> positions;
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* s=reinterpret_cast<Source*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){s=static_cast<Source*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);s->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
        if(!s)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_MOUSEWHEEL){
            s->action=Trace::current->action.load();++s->receives;
            Emit(Stage::SourceReceive,s->action);
            const int delta=GET_WHEEL_DELTA_WPARAM(wp);
            s->remainder-=delta*(s->whole?1:60);
            const int pixels=(s->remainder/120)*(s->whole?60:1);s->remainder%=120;
            s->target=std::clamp(s->offset+pixels,0,s->limit);
            if(s->delay)SetTimer(w,1,s->delay,nullptr);
            else if(s->smooth)SetTimer(w,2,16,nullptr);
            else s->Apply(s->target);
            return 0;
        }
        if(m==WM_TIMER){
            if(wp==1){KillTimer(w,1);if(s->smooth)SetTimer(w,2,16,nullptr);else s->Apply(s->target);}
            if(wp==2){s->Apply(s->offset+std::clamp(s->target-s->offset,-6,6));if(s->offset==s->target)KillTimer(w,2);}
            return 0;
        }
        if(m==WM_PAINT){
            PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);Image image(900,740);
            std::fill(image.pixels.begin(),image.pixels.end(),0xffeceef0);
            for(int y=40;y<700;++y)for(int x=0;x<900;++x)image.pixels[size_t(y)*900+x]=Pixel(x,s->offset+y-40);
            BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),900,-740,1,32,BI_RGB};
            SetDIBitsToDevice(dc,0,0,900,740,0,0,0,740,image.pixels.data(),&bi,DIB_RGB_COLORS);EndPaint(w,&ps);
            Emit(Stage::SourcePaint,s->action,0,0,0,0,900,s->offset);return 0;
        }
        return DefWindowProcW(w,m,wp,lp);
    }
    void Apply(int next){if(next!=offset){offset=next;++changes;positions[action]=offset;InvalidateRect(window,nullptr,FALSE);UpdateWindow(window);}}
    Source(){WNDCLASSW c{};c.lpfnWndProc=Proc;c.hInstance=GetModuleHandleW(nullptr);c.hCursor=LoadCursorW(nullptr,IDC_ARROW);c.lpszClassName=L"PcTool.ScrollPerformanceFixture";RegisterClassW(&c);
        window=CreateWindowExW(WS_EX_TOPMOST,c.lpszClassName,L"PcTool scroll performance fixture",WS_POPUP|WS_VISIBLE,120,100,900,740,nullptr,nullptr,c.hInstance,this);
        winrt::check_bool(window!=nullptr);SetWindowPos(window,HWND_TOPMOST,120,100,900,740,SWP_SHOWWINDOW);SetForegroundWindow(window);UpdateWindow(window);}
    ~Source(){if(window)DestroyWindow(window);}
};
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
template<class F>void Wait(F ready,int timeout,const char* error){const auto end=GetTickCount64()+timeout;while(!ready()&&GetTickCount64()<end)Pump();if(!ready())throw std::runtime_error(error);}
void Wheel(int direction){INPUT i{};i.type=INPUT_MOUSE;i.mi.dwFlags=MOUSEEVENTF_WHEEL;i.mi.mouseData=DWORD(direction*120);winrt::check_bool(SendInput(1,&i,sizeof(i))==1);}
bool Moved(StitchResult r){return r==StitchResult::Appended||r==StitchResult::Prepended||r==StitchResult::Revisited;}
void Verify(const Image& image,int low,int high){
    if(image.width!=900||image.height!=740+high-low)throw std::runtime_error("Performance fixture interval/size mismatch");
    for(int y=0;y<image.height;++y)for(int x=0;x<900;++x){const uint32_t expected=y<40||y>=image.height-40?0xffeceef0:Pixel(x,low+y-40);
        if(image.pixels[size_t(y)*900+x]!=expected)throw std::runtime_error("Performance fixture pixel mismatch");}
}
}

void RunScrollSessionPerformance(const std::wstring& prefix,int samples){
    if(samples<1||samples>1000)throw std::runtime_error("Expected 1..1000 scroll samples");
    std::ofstream csv(std::filesystem::path(prefix+L"-actions.csv"));
    std::ofstream raw(std::filesystem::path(prefix+L"-trace.csv"));
    if(!csv||!raw)throw std::runtime_error("Cannot open scroll performance output");
    csv<<"scenario,action,sample_kind,direction,pixels,added_height,input_dispatch_ms,dispatch_receive_ms,receive_first_source_paint_ms,first_source_paint_capture_ms,frame_stitch_wait_ms,stitch_ms,preview_ms,input_publish_ms,input_paint_ms,dispatch_publish_ms,dispatch_paint_ms,interval_ms\n";
    raw<<"scenario,stage,action,version,input,qpc,direction,units,height,result,supplement\n";
    const double ticks=double(Frequency())/1000;
    RestoreCursor restoreCursor;
    for(const std::string scenario:{"immediate","smooth","delayed","whole","automatic"}){
        Trace trace;Source source;source.smooth=scenario=="smooth";source.delay=scenario=="delayed"?250:0;source.whole=scenario=="whole";
        RECT bounds{};GetWindowRect(source.window,&bounds);auto document=std::make_shared<ImageDocument>();bool finished=false;
        auto session=OpenLongCaptureSession(bounds,document,[&](bool ok){finished=ok;},[](auto){});
        SetCursorPos(bounds.left+450,bounds.top+370);
        std::map<uint64_t,int> displacement,added;int low=source.offset,high=source.offset;
        // First action includes preparation. Three further actions warm up the
        // steady state; reverse once halfway through to exercise revisits.
        if(scenario=="automatic"){
            source.limit=3000+15+(samples+3)*30;
            INPUT clicks[2]{};for(auto& click:clicks)click.type=INPUT_MOUSE;
            clicks[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;clicks[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
            winrt::check_bool(SendInput(2,clicks,sizeof(INPUT))==2);
            Wait([&]{return session->Progress().atEnd;},samples*2000+10000,"Automatic measured scroll did not reach its boundary");
            if(source.offset!=source.limit)throw std::runtime_error("Automatic measured source stopped at wrong offset");
            int previous=3000;
            for(const auto& [id,offset]:source.positions){displacement[id]=offset-previous;const int range=high-low;low=std::min(low,offset);high=std::max(high,offset);added[id]=high-low-range;previous=offset;}
        }else for(int n=0;n<samples+4;++n){
            const auto before=session->Progress().actionId;const int oldOffset=source.offset;
            Wheel(n>=4+samples/2?1:-1);
            Wait([&]{const auto p=session->Progress();return p.actionId>before&&p.completedActionId>=p.actionId&&!p.inFlight;},6000,"Measured scroll did not finish");
            const auto p=session->Progress();if(!Moved(p.analysis.result)||p.analysis.currentY!=source.offset-3000)throw std::runtime_error("Measured scroll did not match source position");
            displacement[p.actionId]=source.offset-oldOffset;const int range=high-low;low=std::min(low,source.offset);high=std::max(high,source.offset);added[p.actionId]=high-low-range;
        }
        // Edit exports to our callback without touching clipboard or user data.
        SendMessageW(session->Window(),WM_COMMAND,11,0);Wait([&]{return finished;},6000,"Measured capture export did not finish");
        session.reset();Verify(document->image,low,high);
        const auto events=trace.Snapshot();
        std::map<uint64_t,std::map<Stage,Event>> actions;std::map<uint64_t,Event> inputs,paints;
        for(const auto& e:events){raw<<scenario<<','<<int(e.stage)<<','<<e.actionId<<','<<e.imageVersion<<','<<e.inputId<<','<<e.qpc<<','<<e.direction<<','<<e.units<<','<<e.height<<','<<e.result<<','<<e.supplement<<'\n';
            if(e.stage==Stage::Input)inputs[e.inputId]=e;
            if(e.stage==Stage::Paint){if(!paints.count(e.imageVersion))paints[e.imageVersion]=e;}
            if(!e.actionId)continue;
            auto& action=actions[e.actionId];
            // Keep the first source paint and captured change. Smooth scrolling
            // can paint repeatedly; its remaining animation is in the wait span.
            if(e.stage==Stage::Dispatch||e.stage==Stage::FrameChanged||e.stage==Stage::SourceReceive||e.stage==Stage::SourcePaint){if(!action.count(e.stage))action[e.stage]=e;}
            else action[e.stage]=e;
        }
        int64_t lastDispatch=0;
        for(auto& [id,a]:actions){if(!a.count(Stage::Dispatch)||!a.count(Stage::Publish)||!Moved(StitchResult(a[Stage::Publish].result)))continue;
            const auto dispatch=a[Stage::Dispatch];if(inputs.count(dispatch.inputId))a[Stage::Input]=inputs[dispatch.inputId];
            const auto published=a[Stage::Publish];if(paints.count(published.imageVersion)&&paints[published.imageVersion].qpc>=published.qpc)a[Stage::Paint]=paints[published.imageVersion];
            auto span=[&](Stage begin,Stage end){return a.count(begin)&&a.count(end)?double(a[end].qpc-a[begin].qpc)/ticks:-1;};
            csv<<scenario<<','<<id<<','<<(id==1?"first":id<=4?"warmup":"measured")<<','<<dispatch.direction<<','<<displacement[id]<<','<<added[id]<<','<<span(Stage::Input,Stage::Dispatch)<<','<<span(Stage::Dispatch,Stage::SourceReceive)<<','<<span(Stage::SourceReceive,Stage::SourcePaint)<<','<<span(Stage::SourcePaint,Stage::FrameChanged)<<','<<span(Stage::FrameChanged,Stage::StitchBegin)<<','<<span(Stage::StitchBegin,Stage::StitchEnd)<<','<<span(Stage::PreviewBegin,Stage::PreviewEnd)<<','<<span(Stage::Input,Stage::Publish)<<','<<span(Stage::Input,Stage::Paint)<<','<<span(Stage::Dispatch,Stage::Publish)<<','<<span(Stage::Dispatch,Stage::Paint)<<','<<(lastDispatch?double(dispatch.qpc-lastDispatch)/ticks:0)<<'\n';lastDispatch=dispatch.qpc;
        }
        std::cout<<"SCROLL PERFORMANCE "<<scenario<<" actions="<<displacement.size()<<" receives="<<source.receives<<" pixel_oracle=PASS\n";
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
        std::cout<<"SCROLL PROCESS MEMORY peak_working_set_bytes="<<memory.PeakWorkingSetSize<<" peak_commit_bytes="<<memory.PeakPagefileUsage<<'\n';
}

void RunScrollInputDesktopRegression(){
    RestoreCursor restoreCursor;
    for(bool whole:{false,true}){
        Trace trace;Source source;source.whole=whole;RECT bounds{};GetWindowRect(source.window,&bounds);
        auto document=std::make_shared<ImageDocument>();bool finished=false;
        auto session=OpenLongCaptureSession(bounds,document,[&](bool ok){finished=ok;},[](auto){});
        SetCursorPos(bounds.left+450,bounds.top+370);
        Wait([&]{const auto events=trace.Snapshot();return std::any_of(events.begin(),events.end(),[](const auto& e){return e.stage==Stage::BaselineReady;});},5000,"Live baseline did not become ready");
        Wheel(-1);Wait([&]{return session->Progress().actionId==1;},1000,"First input was not dispatched");
        const auto secondAt=GetTickCount64()+100;Wait([&]{return GetTickCount64()>=secondAt;},1000,"Clock did not advance");Wheel(-1);
        Wait([&]{const auto p=session->Progress();return p.actionId==2&&p.completedActionId==2&&!p.inFlight;},4000,"Input received while waiting was lost");
        const int highest=source.offset,count=source.receives;const auto idleUntil=GetTickCount64()+700;
        Wait([&]{return GetTickCount64()>=idleUntil;},1500,"Idle clock did not advance");
        if(source.receives!=count)throw std::runtime_error("Stopped input replayed beyond the one pending intent");
        Wheel(1);Wait([&]{const auto p=session->Progress();return p.actionId==3&&p.completedActionId==3&&!p.inFlight;},4000,"Reverse input did not finish");
        if(source.offset>=highest)throw std::runtime_error("Reverse input retained old direction");
        const int lowest=std::min(3000,source.offset);
        SendMessageW(session->Window(),WM_COMMAND,11,0);Wait([&]{return finished;},5000,"Regression export did not finish");session.reset();
        Verify(document->image,lowest,highest);
        std::cout<<"SCROLL INPUT REGRESSION "<<(whole?"whole":"fractional")<<": waiting input retained, one-slot tail, reverse and exact pixels PASS\n";
    }
}
