#include "system_tools/paint.h"
#include "shared/platform/app_storage.h"
#include "long_capture/scroll_stitcher.h"
#include "long_capture/scroll_annotation_mapping.h"
#include <dwmapi.h>
#include "long_capture/long_capture_session.h"
#include "shared/async/cancellation.h"
#include "capture/image_editor.h"
#include "ocr/ocr_window.h"
#include "shared/platform/capture_platform.h"
#include "shared/ui/capture_ui.h"
#include "app/capture_tools.h"
#include "ocr/ocr_service.h"
#ifdef PCTOOL_OCR_EVALUATION
#include "ocr/ocr_evaluation.h"
#endif
#include "recording/screen_recorder.h"
#include "capture/screenshot_overlay.h"
#include "recording/recording_session.h"
#include "recording/recording_preview.h"
#include <shellapi.h>
#include <richedit.h>
#include "shared/ui/toolbar_icons.h"
#include "shared/annotation/annotation_style.h"
#include <filesystem>
#include <set>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <future>
#include <fstream>

using namespace capture;
static void GifTest(RECT region,const std::wstring& prefix);
void RunScreenshotInteractionTests(const std::wstring& prefix);
void RunWindowSnapTests();
void CompareOcr(const std::wstring& prefix);
static int fixtureClicks=0;
static bool scrollFixture=false;
static int scrollFixtureWheelUnits=0;
static int fixtureOffset=0;
static bool partialFixture=false,partialSmooth=false;static int partialLimit=180,partialTarget=0;
struct ScrollDelivery {ULONGLONG time{};int delta{},before{},target{};POINT point{};HWND window{};DWORD postedTime{};};
static bool bidirectionalFixture=false,bidirectionalWhole=false;
static int bidirectionalDelay=0,bidirectionalRemainder=0;
static int startupAnimation{};
static HWND bidirectionalChild{};
static int scrollSourceMoves{},scrollSourceOtherButtons{};
static std::vector<ScrollDelivery> scrollDeliveries;
static RECT BidirectionalBody(){return bidirectionalWhole?RECT{0,40,900,700}:RECT{320,330,500,530};}
static uint32_t ScrollFixturePixel(int x,int row){uint32_t seed=uint32_t(row)*1664525u+uint32_t(x)*1013904223u;seed^=seed>>13;seed*=1274126177u;return 0xff000000|(seed&0xffffff);}
static Image BidirectionalImage(int offset){
    Image image(900,740);std::fill(image.pixels.begin(),image.pixels.end(),0xffeceef0);
    // Fixed text-like glyphs deliberately occur beside the original viewport,
    // so repeated/stretched navigation is caught by independent composition.
    for(int y=20;y<700;y+=45)for(int yy=y;yy<std::min(y+14,700);++yy)for(int x=20;x<120;++x)image.pixels[size_t(yy)*900+x]=0xff123456;
    const auto body=BidirectionalBody();for(int y=body.top;y<body.bottom;++y)for(int x=body.left;x<body.right;++x)image.pixels[size_t(y)*900+x]=ScrollFixturePixel(x-body.left,y-body.top+offset);
    if(startupAnimation)for(int y=100;y<180;++y)for(int x=20;x<180;++x)image.pixels[size_t(y)*900+x]=startupAnimation%2?0xff305070:0xfff0a020;
    for(int x=0;x<900;++x)image.pixels[size_t(715)*900+x]=0xff556677;return image;
}
static void RepaintScrollFixture(HWND root){InvalidateRect(root,nullptr,FALSE);UpdateWindow(root);if(IsWindow(bidirectionalChild)){InvalidateRect(bidirectionalChild,nullptr,FALSE);UpdateWindow(bidirectionalChild);}}
static void ReceiveControlledWheel(HWND target,WPARAM wp,LPARAM lp){
    const DWORD postedTime=DWORD(GetMessageTime());
    POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)},local=screen;HWND root=GetAncestor(target,GA_ROOT);ScreenToClient(root,&local);const auto body=BidirectionalBody();
    const int delta=GET_WHEEL_DELTA_WPARAM(wp),before=fixtureOffset;
    if(PtInRect(&body,local)){bidirectionalRemainder-=delta*60;const int pixels=bidirectionalRemainder/WHEEL_DELTA;bidirectionalRemainder%=WHEEL_DELTA;partialTarget=std::clamp(fixtureOffset+pixels,0,partialLimit);
        if(bidirectionalDelay)SetTimer(root,902,bidirectionalDelay,nullptr);else if(partialSmooth)SetTimer(root,901,16,nullptr);else{fixtureOffset=partialTarget;RepaintScrollFixture(root);}}
    scrollDeliveries.push_back({GetTickCount64(),delta,before,partialTarget,screen,target,postedTime});
}
static LRESULT CALLBACK NestedScrollProc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==WM_MOUSEMOVE)++scrollSourceMoves;
    if(m==WM_MBUTTONDOWN||m==WM_XBUTTONDOWN)++scrollSourceOtherButtons;
    if(m==WM_MOUSEWHEEL){ReceiveControlledWheel(w,wp,lp);return 0;}
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_LBUTTONDOWN){++fixtureClicks;return 0;}
    if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);const auto body=BidirectionalBody();auto image=BidirectionalImage(fixtureOffset).Crop(body.left,body.top,body.right-body.left,body.bottom-body.top);BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),image.width,-image.height,1,32,BI_RGB};SetDIBitsToDevice(dc,0,0,image.width,image.height,0,0,0,image.height,image.pixels.data(),&bi,DIB_RGB_COLORS);EndPaint(w,&ps);return 0;}
    return DefWindowProcW(w,m,wp,lp);
}
static Image PartialFixtureImage(int offset){
    Image image(900,740);std::fill(image.pixels.begin(),image.pixels.end(),0xffeceef0);
    for(int y=20;y<300;y+=45)for(int yy=y;yy<y+14;++yy)for(int x=20;x<120;++x)image.pixels[size_t(yy)*900+x]=0xff123456;
    for(int y=330;y<530;++y)for(int x=320;x<500;++x){uint32_t seed=uint32_t(y-330+offset)*1664525u+uint32_t(x-320)*1013904223u;seed^=seed>>13;seed*=1274126177u;image.pixels[size_t(y)*900+x]=0xff000000|(seed&0xffffff);}
    for(int x=0;x<900;++x)image.pixels[size_t(700)*900+x]=0xff556677;return image;
}
static LRESULT CALLBACK FixtureProc(HWND w,UINT m,WPARAM wp,LPARAM lp) {
    if(bidirectionalFixture&&m==WM_MOUSEMOVE)++scrollSourceMoves;
    if(bidirectionalFixture&&(m==WM_MBUTTONDOWN||m==WM_XBUTTONDOWN))++scrollSourceOtherButtons;
    if(m==WM_PAINT) {
        PAINTSTRUCT ps{}; auto target=BeginPaint(w,&ps); RECT r{}; GetClientRect(w,&r);
        // Present the fixture atomically: Desktop Duplication must not sample
        // between the white background fill and the text/colored rectangle.
        HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,r.right),std::max(1L,r.bottom)); auto previous=SelectObject(dc,bitmap);
        const auto present=[&] { BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY); SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); EndPaint(w,&ps); };
        FillRect(dc,&r,GetSysColorBrush(COLOR_WINDOW));
        if(bidirectionalFixture){auto frame=BidirectionalImage(fixtureOffset);BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),frame.width,-frame.height,1,32,BI_RGB};SetDIBitsToDevice(dc,0,0,frame.width,frame.height,0,0,0,frame.height,frame.pixels.data(),&bi,DIB_RGB_COLORS);present();return 0;}
        if(partialFixture){auto frame=PartialFixtureImage(fixtureOffset);BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),frame.width,-frame.height,1,32,BI_RGB};SetDIBitsToDevice(dc,0,0,frame.width,frame.height,0,0,0,frame.height,frame.pixels.data(),&bi,DIB_RGB_COLORS);present();return 0;}
        if(scrollFixture) {
            Image frame(641,361);
            for(int y=0;y<361;++y) for(int x=0;x<641;++x) {
                const int row=y<20?y:y>=345?y:y+fixtureOffset;
                uint32_t seed=uint32_t(row)*1664525u+uint32_t(x)*1013904223u;
                seed^=seed>>13; seed*=1274126177u;
                frame.pixels[size_t(y)*641+x]=0xff000000|(seed&0xffffff);
            }
            BITMAPINFO bi{}; bi.bmiHeader={sizeof(BITMAPINFOHEADER),641,-361,1,32,BI_RGB};
            SetDIBitsToDevice(dc,0,0,641,361,0,0,0,361,frame.pixels.data(),&bi,DIB_RGB_COLORS);
            present(); return 0;
        }
        HFONT font=CreateFontW(-32,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei");
        auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(0,0,0));
        TextOutW(dc,20,20,L"PcTool 12345 中文识别测试",21);
        auto time=L"Video test "+std::to_wstring(GetTickCount64()); TextOutW(dc,20,90,time.c_str(),int(time.size()));
        RECT block{20,160,250,300}; SetDCBrushColor(dc,GetPropW(w,L"PreviewLateFrame")?RGB(30,50,210):RGB(210,30,50)); FillRect(dc,&block,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SelectObject(dc,old); DeleteObject(font); present(); return 0;
    }
    if(m==WM_MOUSEWHEEL && bidirectionalFixture){ReceiveControlledWheel(w,wp,lp);return 0;}
    if(m==WM_TIMER&&wp==903&&bidirectionalFixture){++startupAnimation;RepaintScrollFixture(w);return 0;}
    if(m==WM_TIMER&&wp==902&&bidirectionalFixture){KillTimer(w,902);if(partialSmooth)SetTimer(w,901,16,nullptr);else{fixtureOffset=partialTarget;RepaintScrollFixture(w);}return 0;}
    if(m==WM_TIMER&&wp==901&&bidirectionalFixture){fixtureOffset+=std::clamp(partialTarget-fixtureOffset,-6,6);RepaintScrollFixture(w);if(fixtureOffset==partialTarget)KillTimer(w,901);return 0;}
    if(m==WM_MOUSEWHEEL && partialFixture){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&p);RECT r{320,330,500,530};if(PtInRect(&r,p)){partialTarget=std::clamp(fixtureOffset-GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*60,0,partialLimit);if(partialSmooth)SetTimer(w,901,16,nullptr);else{fixtureOffset=partialTarget;InvalidateRect(w,nullptr,FALSE);UpdateWindow(w);}}return 0;}
    if(m==WM_TIMER&&wp==901&&partialFixture){fixtureOffset+=std::clamp(partialTarget-fixtureOffset,-6,6);InvalidateRect(w,nullptr,FALSE);UpdateWindow(w);if(fixtureOffset==partialTarget)KillTimer(w,901);return 0;}
    if(m==WM_MOUSEWHEEL && scrollFixture){scrollFixtureWheelUnits+=GET_WHEEL_DELTA_WPARAM(wp);const int notches=scrollFixtureWheelUnits/WHEEL_DELTA;scrollFixtureWheelUnits%=WHEEL_DELTA;fixtureOffset=std::clamp(fixtureOffset-notches*60,0,480);InvalidateRect(w,nullptr,FALSE);UpdateWindow(w);return 0;}
    if(m==WM_LBUTTONDOWN) { ++fixtureClicks; return 0; }
    if(m==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w,m,wp,lp);
}
static int64_t StreamEnd(const std::wstring& path,DWORD stream) {
    winrt::com_ptr<IMFSourceReader> reader; winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(),nullptr,reader.put()));
    winrt::check_hresult(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS),FALSE));
    winrt::check_hresult(reader->SetStreamSelection(stream,TRUE));
    int64_t last=0; int samples=0;
    while(true) {
        DWORD actual=0,flags=0; LONGLONG time=0; winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(reader->ReadSample(stream,0,&actual,&flags,&time,sample.put()));
        if(sample) { LONGLONG duration=0; sample->GetSampleDuration(&duration); last=time+duration; ++samples; }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if(!samples) throw std::runtime_error("No samples in output stream");
    return last;
}
static Image VideoPreview(const std::wstring& path,int64_t desired=5000000) {
    winrt::com_ptr<IMFAttributes> attributes; winrt::check_hresult(MFCreateAttributes(attributes.put(),1));
    winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE));
    winrt::com_ptr<IMFSourceReader> reader; winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(),attributes.get(),reader.put()));
    winrt::com_ptr<IMFMediaType> output; winrt::check_hresult(MFCreateMediaType(output.put()));
    winrt::check_hresult(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
    winrt::check_hresult(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32));
    winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,output.get()));
    winrt::com_ptr<IMFMediaType> actual;
    winrt::check_hresult(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),actual.put()));
    UINT32 width=0,height=0; winrt::check_hresult(MFGetAttributeSize(actual.get(),MF_MT_FRAME_SIZE,&width,&height));
    const UINT32 displayWidth=width,displayHeight=height;
    winrt::com_ptr<IMFSample> sample;
    for(int i=0;i<3000;++i) {
        sample=nullptr;
        DWORD stream=0,flags=0; LONGLONG time=0;
        winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&stream,&flags,&time,sample.put()));
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM) break;
        if(sample && time>=desired) break; // skip the test fixture's DWM opening animation
    }
    if(!sample) throw std::runtime_error("No decoded video frame");
    actual=nullptr;
    winrt::check_hresult(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),actual.put()));
    winrt::check_hresult(MFGetAttributeSize(actual.get(),MF_MT_FRAME_SIZE,&width,&height));
    winrt::com_ptr<IMFMediaBuffer> buffer; winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
    BYTE* bytes=nullptr; DWORD size=0; LONG stride=0;
    auto buffer2d=buffer.try_as<IMF2DBuffer>();
    if(buffer2d) winrt::check_hresult(buffer2d->Lock2D(&bytes,&stride));
    else {
        winrt::check_hresult(buffer->Lock(&bytes,nullptr,&size));
        UINT32 rawStride=width*4; actual->GetUINT32(MF_MT_DEFAULT_STRIDE,&rawStride); stride=LONG(rawStride);
        if(stride<0) bytes+=size_t(height-1)*(-stride);
    }
    Image image{int(width),int(height)};
    for(UINT32 y=0;y<height;++y) std::memcpy(image.pixels.data()+size_t(y)*width,bytes+ptrdiff_t(y)*stride,size_t(width)*4);
    if(buffer2d) buffer2d->Unlock2D(); else buffer->Unlock();
    return image.Crop(0,0,std::min(int(displayWidth),image.width),std::min(int(displayHeight),image.height));
}
static void PumpFor(int ms) {
    const auto until=GetTickCount64()+ms;
    while(GetTickCount64()<until) { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
static void FocusNativeFixture(HWND fixture){
    SetForegroundWindow(fixture);PumpFor(30);if(GetForegroundWindow()==fixture)return;
    // Foreground activation is restricted for a fresh CTest child process.
    // Give our own visible test window real input before installing capture
    // hooks, rather than weakening production source validation.
    POINT point{10,10};ClientToScreen(fixture,&point);
    if(GetAncestor(WindowFromPoint(point),GA_ROOT)!=fixture)throw std::runtime_error("Independent native fixture is covered before focus setup");
    SetCursorPos(point.x,point.y);INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;winrt::check_bool(SendInput(2,click,sizeof(INPUT))==2);PumpFor(100);
    if(GetForegroundWindow()!=fixture)throw std::runtime_error("Could not activate independent native test fixture");
}
static Image WindowSnapshot(HWND w,const std::wstring& path) {
    RECT r{}; GetClientRect(w,&r); Image image(r.right,r.bottom); HBITMAP bitmap=ToBitmap(image); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
    SendMessageW(w,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT); SelectObject(dc,old); DeleteDC(dc); image=FromBitmap(bitmap); DeleteObject(bitmap); SavePng(image,path); return image;
}
static void CALLBACK CancelSaveDialog(HWND,UINT,UINT_PTR,DWORD) {
    EnumWindows([](HWND w,LPARAM)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(w,&pid); wchar_t cls[64]{}; GetClassNameW(w,cls,64);
        if(pid==GetCurrentProcessId() && wcscmp(cls,L"#32770")==0 && GetDlgItem(w,IDCANCEL)) PostMessageW(w,WM_COMMAND,IDCANCEL,0);
        return TRUE;
    },0);
}
static std::set<std::filesystem::path> PendingVideos() {
    std::set<std::filesystem::path> result;
    auto dir=app_storage::Cache()/L"Recordings";
    if(std::filesystem::exists(dir)) for(const auto& e:std::filesystem::directory_iterator(dir)) if(e.path().extension()==L".mp4") result.insert(e.path());
    return result;
}
static double AudioRms(const std::wstring& path,int64_t begin,int64_t end) {
    winrt::com_ptr<IMFSourceReader> reader; winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(),nullptr,reader.put()));
    winrt::com_ptr<IMFMediaType> type; winrt::check_hresult(MFCreateMediaType(type.put()));
    type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio); type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_Float);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2); type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000);
    winrt::check_hresult(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),nullptr,type.get()));
    double sum=0; size_t count=0;
    while(true) {
        DWORD stream=0,flags=0; LONGLONG time=0; winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM),0,&stream,&flags,&time,sample.put()));
        if(time>end || (flags&MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if(sample && time>=begin) {
            winrt::com_ptr<IMFMediaBuffer> buffer; sample->ConvertToContiguousBuffer(buffer.put()); BYTE* data=nullptr; DWORD size=0; buffer->Lock(&data,nullptr,&size);
            for(size_t i=0;i<size/sizeof(float);++i) { const double value=reinterpret_cast<float*>(data)[i]; sum+=value*value; ++count; }
            buffer->Unlock();
        }
    }
    if(!count) throw std::runtime_error("No audio samples in requested interval"); return std::sqrt(sum/count);
}
struct TestTone {
    HWAVEOUT device{}; WAVEHDR header{}; std::vector<int16_t> samples;
    TestTone() {
        WAVEFORMATEX format{WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
        if(waveOutOpen(&device,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR) throw std::runtime_error("Test tone output unavailable");
        samples.resize(48000*2*9); for(size_t i=0;i<samples.size()/2;++i) samples[i*2]=samples[i*2+1]=int16_t(1800*std::sin(i*6.283185307179586*440/48000));
        header.lpData=reinterpret_cast<LPSTR>(samples.data()); header.dwBufferLength=DWORD(samples.size()*2);
        waveOutPrepareHeader(device,&header,sizeof(header)); waveOutWrite(device,&header,sizeof(header));
    }
    ~TestTone() { if(device) { waveOutReset(device); waveOutUnprepareHeader(device,&header,sizeof(header)); waveOutClose(device); } }
};
static void LiveAudioTest(RECT region,const std::wstring& path) {
    RecordingOptions options; options.region=region; options.path=path; options.waitForStart=true; options.systemAudio=true; options.microphone=false;
    RecordingState state; std::thread worker([&]{RecordScreen(options,state);});
    auto wait=GetTickCount64()+5000; while(!state.ready && !state.finished && GetTickCount64()<wait) PumpFor(20);
    if(!state.ready) { state.stop=true; worker.join(); throw std::runtime_error("Recorder failed readiness: "+winrt::to_string(state.error)); }
    if(state.elapsed!=0) { state.stop=true; worker.join(); throw std::runtime_error("Recorder clock advanced before start"); }
    TestTone tone; const auto started=GetTickCount64(); state.start=true;
    while(!state.finished) {
        const auto elapsed=GetTickCount64()-started;
        state.systemEnabled=elapsed<1000 || (elapsed>=3000 && elapsed<4000) || elapsed>=5500;
        state.micEnabled=(elapsed>=2000 && elapsed<3000) || elapsed>=5500;
        state.paused=elapsed>=5000 && elapsed<5500;
        if(elapsed>=7000) state.stop=true;
        PumpFor(10);
    }
    worker.join(); if(!state.error.empty()) throw std::runtime_error(winrt::to_string(state.error));
    winrt::check_hresult(MFStartup(MF_VERSION));
    const auto audible=AudioRms(path,3500000,8500000),muted=AudioRms(path,13000000,18000000),resumed=AudioRms(path,58000000,62000000),mutedAgain=AudioRms(path,43000000,48000000);
    std::cout<<"LIVE AUDIO rms="<<audible<<" muted="<<muted<<" resumed="<<resumed<<" mutedAgain="<<mutedAgain<<std::endl;
    if(audible<0.0001 || resumed<0.0001 || muted>0.00001 || mutedAgain>0.00001) throw std::runtime_error("Live mute or resume audio failed");
    const auto videoEnd=StreamEnd(path,DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM)),audioEnd=StreamEnd(path,DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM));
    if(std::abs(videoEnd-audioEnd)>2000000 || videoEnd<62000000 || videoEnd>68000000) throw std::runtime_error("Live audio pause timeline mismatch");
    MFShutdown(); std::cout<<"LIVE AUDIO PASS: ready gate, source toggles, silent AAC intervals, pause and timestamps\n";
}
static void IconTest(const std::wstring& prefix) {
    ScreenshotOverlay gdiplusLifetime(GetModuleHandleW(nullptr));
    const ToolbarIcon icons[]={ToolbarIcon::Pen,ToolbarIcon::Rectangle,ToolbarIcon::Ellipse,ToolbarIcon::Arrow,ToolbarIcon::Text,ToolbarIcon::Laser,ToolbarIcon::Cursor,ToolbarIcon::CursorHidden,ToolbarIcon::Undo,ToolbarIcon::Clear,ToolbarIcon::System,ToolbarIcon::SystemMuted,ToolbarIcon::Mic,ToolbarIcon::MicMuted,ToolbarIcon::Pause,ToolbarIcon::Resume,ToolbarIcon::Cancel,ToolbarIcon::Grip,ToolbarIcon::LongCapture,ToolbarIcon::Ocr,ToolbarIcon::Gif};
    for(UINT dpi:{96,144,192}) {
        const int cell=MulDiv(36,dpi,96); Image image(cell*int(std::size(icons)),cell*3);
        std::fill(image.pixels.begin(),image.pixels.end(),0xffffffff);
        HBITMAP bitmap=ToBitmap(image); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
        for(int row=0;row<3;++row) for(int i=0;i<int(std::size(icons));++i) {
            RECT rect{i*cell,row*cell,(i+1)*cell,(row+1)*cell};
            DrawToolbarIcon(dc,icons[i],rect,dpi,row==0?RGB(0,0,0):row==1?RGB(20,135,230):RGB(145,150,158));
        }
        SelectObject(dc,old); DeleteDC(dc); image=FromBitmap(bitmap); DeleteObject(bitmap);
        for(int i:{0,2,8,9,10,11,13,14,18}) {
            int blended=0,ink=0;
            for(int y=0;y<cell;++y) for(int x=i*cell;x<(i+1)*cell;++x) { auto value=image.pixels[size_t(y)*image.width+x]&255; if(value<250) ++ink; if(value>5 && value<250) ++blended; }
            if(ink<15 || blended<10) throw std::runtime_error("Toolbar curve/diagonal lost antialiasing");
        }
        SavePng(image,prefix+L"-dpi"+std::to_wstring(dpi)+L".png");
    }
    std::cout<<"ICON PASS: native DPI vector rendering, antialiased edges, enabled/selected/muted colors\n";
}
static bool unexpectedDiscardDialog=false;
static void CALLBACK WatchDiscardDialogs(HWND,UINT,UINT_PTR,DWORD) {
    EnumWindows([](HWND w,LPARAM)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(w,&pid); if(pid!=GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
        wchar_t cls[64]{},title[128]{}; GetClassNameW(w,cls,64); GetWindowTextW(w,title,128);
        if(wcscmp(cls,L"#32770")==0 || wcscmp(title,L"PcTool · 取消录制")==0) {
            unexpectedDiscardDialog=true; PostMessageW(w,WM_CLOSE,0,0);
        }
        return TRUE;
    },0);
}
static void DiscardUiTest(RECT region,const std::wstring& prefix) {
    unexpectedDiscardDialog=false;
    const auto timer=SetTimer(nullptr,0,40,WatchDiscardDialogs);
    for(int scenario=0;scenario<5;++scenario) {
        const auto previousFiles=PendingVideos(); bool completed=false,success=true; int callbacks=0;
        auto session=OpenRecordingSession(region,[&](bool ok){completed=true;success=ok;++callbacks;}); HWND bar=session->Window();
        SendMessageW(bar,WM_COMMAND,109,0); // deterministic silent capture
        if(scenario==2) {
            for(int dpi:{96,144,192}) {
                RECT bounds{}; GetWindowRect(bar,&bounds); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&bounds));
                SendMessageW(bar,WM_MOUSEMOVE,0,MAKELPARAM(MulDiv(36,dpi,96),MulDiv(23,dpi,96)));
                WindowSnapshot(bar,prefix+L"-prepare-muted-dpi"+std::to_wstring(dpi)+L".png");
                SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,110,0);
                WindowSnapshot(bar,prefix+L"-prepare-on-dpi"+std::to_wstring(dpi)+L".png");
                SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,110,0);
            }
        }
        SendMessageW(bar,WM_COMMAND,113,0);
        PumpFor(scenario==0?0:scenario==1?1200:4200);
        if(scenario==2) {
            for(int dpi:{96,144,192}) {
                RECT bounds{}; GetWindowRect(bar,&bounds); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&bounds));
                SendMessageW(bar,WM_COMMAND,101,0); SendMessageW(bar,WM_MOUSEMOVE,0,MAKELPARAM(MulDiv(72,dpi,96),MulDiv(23,dpi,96)));
                WindowSnapshot(bar,prefix+L"-recording-dpi"+std::to_wstring(dpi)+L".png");
                SendMessageW(bar,WM_COMMAND,111,0); WindowSnapshot(bar,prefix+L"-paused-dpi"+std::to_wstring(dpi)+L".png"); SendMessageW(bar,WM_COMMAND,111,0);
                SendMessageW(bar,WM_COMMAND,101,0);
            }
        }
        if(scenario==3) SendMessageW(bar,WM_COMMAND,111,0);
        SendMessageW(bar,WM_COMMAND,112,0); SendMessageW(bar,WM_COMMAND,112,0);
        if(scenario==4) session.reset(); // closing the app must not undo discard
        else {
            const auto deadline=GetTickCount64()+5000; while(!completed && GetTickCount64()<deadline) PumpFor(20);
            if(!completed || success || callbacks!=1 || session->Active()) throw std::runtime_error("Discard did not restore screenshot callback exactly once");
            session.reset();
        }
        if(PendingVideos()!=previousFiles) throw std::runtime_error("Discard left a new final or partial recording");
        if(unexpectedDiscardDialog) throw std::runtime_error("Discard showed a confirmation/save/error dialog");
    }
    KillTimer(nullptr,timer);
    std::cout<<"DISCARD UI PASS: initialization, countdown, recording, paused, repeated X and shutdown; no dialogs or new files\n";
}
static void DiscardFilesTest(const std::wstring& prefix) {
    const auto path=prefix+L"-owned.partial.mp4",unrelated=prefix+L"-unrelated.mp4";
    HANDLE held=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(held==INVALID_HANDLE_VALUE) winrt::throw_last_error();
    HANDLE other=CreateFileW(unrelated.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(other==INVALID_HANDLE_VALUE) { CloseHandle(held); winrt::throw_last_error(); } CloseHandle(other);
    RecordingState state; state.ownedFiles={path}; state.finished=true;
    state.RequestStop(RecordingStopReason::Discard); state.RequestStop(RecordingStopReason::Shutdown);
    if(state.stopReason!=RecordingStopReason::Discard) throw std::runtime_error("Shutdown overrode discard reason");
    auto error=DiscardRecordingFiles(state);
    if(error.find(path)==std::wstring::npos || state.ownedFiles.size()!=1) throw std::runtime_error("Deletion failure lost retained file path");
    CloseHandle(held);
    if(!DiscardRecordingFiles(state).empty() || !state.ownedFiles.empty() || GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Partial file cleanup failed");
    if(GetFileAttributesW(unrelated.c_str())==INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Cleanup touched unrelated file"); DeleteFileW(unrelated.c_str());
    if(!DiscardRecordingFiles(state).empty()) throw std::runtime_error("Cleanup is not idempotent");
    std::cout<<"DISCARD FILES PASS: partial ownership, locked file errors, retry and sticky discard reason\n";
}
static void AnnotationAlphaTest(const std::wstring& prefix) {
    for(UINT dpi:{96u,144u,192u}) {
        AnnotationRenderer renderer(dpi); RecordingAnnotations snapshot; snapshot.dpi=dpi;
        for(Tool tool:{Tool::Pen,Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Text}) {
            Annotation a; a.tool=tool; a.color=RGB(255,70,70); a.size=tool==Tool::Text?24:3;
            const int index=int(snapshot.shapes.size()); a.start={20+index*115,25}; a.end={105+index*115,95};
            a.points={a.start,{a.start.x+25,50},{a.start.x+47,40},a.end}; a.text=L"文字 Aa";
            snapshot.shapes.push_back(a);
        }
        snapshot.laser={{570,145},true};
        auto alpha=RenderRecordingSurface(620,180,snapshot,renderer);
        int partial=0;
        for(auto pixel:alpha.pixels) {
            const auto coverage=pixel>>24; if(coverage>0 && coverage<255) ++partial;
            for(int shift:{0,8,16}) if(((pixel>>shift)&255)>coverage) throw std::runtime_error("Layer pixels are not premultiplied");
        }
        if(partial<400) throw std::runtime_error("Annotations lack antialiased coverage");
        for(uint32_t background:{0xfff7f7f7u,0xff303030u}) {
            Image direct(620,180); std::fill(direct.pixels.begin(),direct.pixels.end(),background);
            CompositeRecording(direct,snapshot,renderer,0); Image composed(620,180);
            int worst=0; size_t worstPixel=0;
            for(size_t i=0;i<alpha.pixels.size();++i) {
                const auto p=alpha.pixels[i],a=p>>24; uint32_t output=0xff000000;
                for(int shift:{0,8,16}) {
                    const unsigned c=((p>>shift)&255)+(((background>>shift)&255)*(255-a)+127)/255;
                    output|=std::min(255u,c)<<shift;
                    const int difference=std::abs(int(c)-int((direct.pixels[i]>>shift)&255)); if(difference>worst) { worst=difference; worstPixel=i; }
                }
                composed.pixels[i]=output;
            }
            if(worst>4) throw std::runtime_error("Transparent annotation mismatch at pixel "+std::to_string(worstPixel)+": "+std::to_string(worst));
            SavePng(composed,prefix+L"-dpi"+std::to_wstring(dpi)+(background==0xff303030?L"-dark.png":L"-light.png"));
        }
    }
    std::cout<<"ANNOTATION ALPHA PASS: pen, rectangle, ellipse, arrow, text and pointer match opaque rendering at 96/144/192 DPI\n";
}
static void ClickStyle(HWND window,RECT rect) {
    const LPARAM p=MAKELPARAM((rect.left+rect.right)/2,(rect.top+rect.bottom)/2);
    SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,p); SendMessageW(window,WM_LBUTTONUP,0,p);
}
void CheckCrossProcessClick(POINT point,HWND overrideOverlay=nullptr);
static void StylePanelTest(const std::wstring& prefix) {
    AnnotationRenderer lifetime;
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);
    AnnotationStyle chosen; int changes=0,escapes=0;
    AnnotationStylePanel panel(nullptr,[&](AnnotationStyle value){chosen=value;++changes;},[&]{++escapes;});
    const auto popup=[] { return FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏字号列表"); };
    for(UINT dpi:{96u,144u,192u}) {
        const auto layout=MakeStylePanelLayout({},dpi,false,true),text=MakeStylePanelLayout({},dpi,true,true);
        const int row=MulDiv(22,int(dpi),96);
        RECT anchor{mi.rcMonitor.left+20,mi.rcMonitor.top+30,mi.rcMonitor.left+800,mi.rcMonitor.top+80};
        panel.Update({},false,dpi,anchor,mi.rcMonitor);
        for(int i=0;i<3;++i) { ClickStyle(panel.Window(),layout.widths[i]); if(chosen.lineIndex!=i) throw std::runtime_error("Width click did not change style"); }
        for(int i=0;i<16;++i) { ClickStyle(panel.Window(),layout.colors[i]); if(chosen.color!=AnnotationColors[i]) throw std::runtime_error("Color click did not change style"); }
        SendMessageW(panel.Window(),WM_ACTIVATE,WA_INACTIVE,0);
        if(!IsWindowVisible(panel.Window())) throw std::runtime_error("Style palette closed while drawing");
        ClickStyle(panel.Window(),layout.colors[0]); WindowSnapshot(panel.Window(),prefix+L"-width-dpi"+std::to_wstring(dpi)+L".png");
        panel.Update(chosen,true,dpi,anchor,mi.rcMonitor);
        const auto selectRow=[&](int index) { RECT bounds{}; GetClientRect(popup(),&bounds); ClickStyle(popup(),{1,1+row*index,bounds.right-1,1+row*(index+1)}); };
        for(bool bottom:{false,true}) {
            if(bottom) anchor={mi.rcMonitor.right-120,mi.rcMonitor.bottom-60,mi.rcMonitor.right,mi.rcMonitor.bottom-10};
            panel.Update(chosen,true,dpi,anchor,mi.rcMonitor); UpdateWindow(panel.Window());
            RECT closed{}; GetWindowRect(panel.Window(),&closed);
            const auto before=WindowSnapshot(panel.Window(),prefix+L"-palette-dpi"+std::to_wstring(dpi)+L".png");
            int geometryChanges=0;
            const SUBCLASSPROC geometryObserver=[](HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data)->LRESULT {
                if(message==WM_NCCALCSIZE || message==WM_SIZE) ++*reinterpret_cast<int*>(data);
                return DefSubclassProc(window,message,w,l);
            };
            SetWindowSubclass(panel.Window(),geometryObserver,77,reinterpret_cast<DWORD_PTR>(&geometryChanges));
            struct ObserverCleanup { HWND window; SUBCLASSPROC proc; ~ObserverCleanup(){RemoveWindowSubclass(window,proc,77);} } observerCleanup{panel.Window(),geometryObserver};
            for(int repeat=0;repeat<8;++repeat) {
                ClickStyle(panel.Window(),text.fontCombo);
                RECT palette{},list{}; GetWindowRect(panel.Window(),&palette); GetWindowRect(popup(),&list);
                if(!EqualRect(&palette,&closed) || !IsWindowVisible(popup())) throw std::runtime_error("Font dropdown resized or moved the palette");
                if(list.left<mi.rcMonitor.left || list.right>mi.rcMonitor.right || list.top<mi.rcMonitor.top || list.bottom>mi.rcMonitor.bottom) throw std::runtime_error("Font dropdown escaped monitor");
                if(bottom && list.bottom>closed.top+text.fontCombo.top) throw std::runtime_error("Bottom dropdown failed to open upward");
                DWORD affinity{}; if(!GetWindowDisplayAffinity(popup(),&affinity) || affinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Font dropdown is not excluded from recording");
                const auto after=WindowSnapshot(panel.Window(),prefix+L"-palette-open-dpi"+std::to_wstring(dpi)+L".png");
                if(before.pixels!=after.pixels) throw std::runtime_error("Opening font dropdown changed palette pixels");
                WindowSnapshot(popup(),prefix+(bottom?L"-font-above-dpi":L"-font-open-dpi")+std::to_wstring(dpi)+L".png");
                ClickStyle(panel.Window(),text.fontCombo);
                if(IsWindowVisible(popup())) throw std::runtime_error("Font combo failed to close dropdown");
            }
            RemoveWindowSubclass(panel.Window(),geometryObserver,77);
            // A zero count proves no resize/region transition can copy stale palette pixels.
            if(geometryChanges) throw std::runtime_error("Dropdown toggles caused palette geometry changes");
            ClickStyle(panel.Window(),text.fontCombo); selectRow(bottom?4:8);
            if(chosen.fontPoints!=(bottom?12:16) || IsWindowVisible(popup())) throw std::runtime_error("Font list selected wrong row");
            ClickStyle(panel.Window(),text.fontCombo); SendMessageW(panel.Window(),WM_KEYDOWN,VK_ESCAPE,0);
            if(escapes || IsWindowVisible(popup())) throw std::runtime_error("Dropdown Esc exited drawing prematurely");
            ClickStyle(panel.Window(),text.fontCombo); CheckCrossProcessClick({mi.rcMonitor.right-40,mi.rcMonitor.top+20});
            if(IsWindowVisible(popup()) || !IsWindowVisible(panel.Window())) throw std::runtime_error("Outside click failed to close only the font list");
            ClickStyle(panel.Window(),text.fontCombo); ClickStyle(panel.Window(),text.colors[5]);
            if(IsWindowVisible(popup()) || chosen.color!=AnnotationColors[5]) throw std::runtime_error("Palette click failed to dismiss dropdown and select color");
        }
        panel.Hide();
        RECT limitedMonitor=mi.rcMonitor; limitedMonitor.bottom=limitedMonitor.top+MulDiv(300,int(dpi),96);
        anchor={limitedMonitor.left+20,limitedMonitor.top+MulDiv(70,int(dpi),96),limitedMonitor.left+800,limitedMonitor.top+MulDiv(106,int(dpi),96)};
        panel.Update(chosen,true,dpi,anchor,limitedMonitor); ClickStyle(panel.Window(),text.fontCombo);
        RECT list{}; GetClientRect(popup(),&list); const int rows=(list.bottom-2)/row;
        if(rows<1 || rows>=MaximumFontPoints-MinimumFontPoints+1) throw std::runtime_error("Constrained list did not limit visible rows");
        SendMessageW(popup(),WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-20*WHEEL_DELTA)),0); selectRow(rows-1);
        if(chosen.fontPoints!=MaximumFontPoints) throw std::runtime_error("Maximum font size unreachable");
        ClickStyle(panel.Window(),text.fontCombo); SendMessageW(popup(),WM_MOUSEWHEEL,MAKEWPARAM(0,20*WHEEL_DELTA),0); selectRow(0);
        if(chosen.fontPoints!=MinimumFontPoints) throw std::runtime_error("Minimum font size unreachable");
        panel.Hide(); if(IsWindowVisible(popup())) throw std::runtime_error("Font list survived palette hide");
    }
    if(changes<60) throw std::runtime_error("Style callbacks were lost");
    std::cout<<"STYLE PANEL PASS: DPI, independent excluded dropdown, no palette geometry/pixel changes, outside clicks, scrolling and native selection\n";
}
static int ClickFixtureProcess(int x,int y) {
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"PcTool.CrossProcessFixture";
    wc.lpfnWndProc=[](HWND window,UINT m,WPARAM w,LPARAM l)->LRESULT {
        if(m==WM_LBUTTONDOWN) { const auto clicks=reinterpret_cast<INT_PTR>(GetPropW(window,L"Clicks")); SetPropW(window,L"Clicks",reinterpret_cast<HANDLE>(clicks+1)); return 0; }
        if(m==WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window,m,w,l);
    };
    RegisterClassW(&wc);
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"PcTool test mouse target",WS_POPUP|WS_VISIBLE,x-10,y-10,24,24,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window) winrt::throw_last_error();
    SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)&~WS_EX_NOACTIVATE);
    MSG message{}; while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
void CheckCrossProcessClick(POINT point,HWND overrideOverlay) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    std::wstring command=L"\""+std::wstring(executable)+L"\" click-fixture "+std::to_wstring(point.x)+L" "+std::to_wstring(point.y);
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{}; winrt::check_bool(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process));
    struct Cleanup { PROCESS_INFORMATION process; ~Cleanup(){ if(WaitForSingleObject(process.hProcess,100)!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,1); CloseHandle(process.hProcess); CloseHandle(process.hThread); } } cleanup{process};
    HWND fixture=nullptr; const auto deadline=GetTickCount64()+3000;
    while(GetTickCount64()<deadline && !fixture) { fixture=FindWindowW(L"PcTool.CrossProcessFixture",L"PcTool test mouse target"); PumpFor(10); }
    if(!fixture) throw std::runtime_error("Cross-process fixture did not start");
    // Put the other application's target below the recording display, but
    // above the main test fixture. Testing above the overlay would miss bugs.
    HWND overlay=overrideOverlay?overrideOverlay:FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏边框");
    if(overlay) winrt::check_bool(SetWindowPos(overlay,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE));
    winrt::check_bool(SetWindowPos(fixture,overlay?overlay:HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW));
    SetCursorPos(point.x,point.y); PumpFor(30);
    wchar_t hitClass[100]{}; GetClassNameW(WindowFromPoint(point),hitClass,100);
    RECT targetBounds{}; GetWindowRect(fixture,&targetBounds);
    std::cout<<"Cross-process hit="<<winrt::to_string(hitClass)<<" visible="<<IsWindowVisible(fixture)<<" bounds="<<targetBounds.left<<","<<targetBounds.top<<" point="<<point.x<<","<<point.y<<std::endl;
    INPUT mouse[2]{}; mouse[0].type=mouse[1].type=INPUT_MOUSE; mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(SendInput(2,mouse,sizeof(INPUT))!=2) throw std::runtime_error("Cross-process input injection failed");
    PumpFor(100); const bool received=GetPropW(fixture,L"Clicks")!=nullptr;
    SendMessageW(fixture,WM_CLOSE,0,0); WaitForSingleObject(process.hProcess,1000);
    if(!received) throw std::runtime_error("Recording annotations intercepted another process's mouse click");
}
static void LivePreviewTest(RECT region) {
    HWND controller{}; const auto startupDeadline=GetTickCount64()+3000;
    while(!controller && GetTickCount64()<startupDeadline) { controller=FindWindowW(L"PcTool.BackgroundController",nullptr); PumpFor(20); }
    if(!controller) throw std::runtime_error("Start the real PcTool application before live-preview-test");
    if(FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 屏幕录制") || FindWindowW(L"PcTool.CaptureToolWindow",L"屏幕录制预览 · PcTool"))
        throw std::runtime_error("Close existing recording/preview windows before running the live test");
    const auto before=PendingVideos();
    SendMessageW(controller,WM_HOTKEY,1,0); PumpFor(100);
    HWND source=FindWindowW(L"PcTool.ScreenshotOverlay",nullptr);
    if(!source) throw std::runtime_error("Real application screenshot did not open");
    POINT a{region.left,region.top},b{region.right,region.bottom}; ScreenToClient(source,&a); ScreenToClient(source,&b);
    SendMessageW(source,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y)); SendMessageW(source,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y)); SendMessageW(source,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));
    const int dpi=int(GetDpiForWindow(source)),width=MulDiv(36,dpi,96)*14+MulDiv(68,dpi,96);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromRect(&region,MONITOR_DEFAULTTONEAREST),&monitor);
    POINT button{std::clamp<int>(region.right-width,monitor.rcMonitor.left,monitor.rcMonitor.right-width)+MulDiv(378,dpi,96),region.bottom+MulDiv(23,dpi,96)};
    ScreenToClient(source,&button); SendMessageW(source,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(button.x,button.y)); SendMessageW(source,WM_LBUTTONUP,0,MAKELPARAM(button.x,button.y));
    HWND bar=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 屏幕录制");
    if(!bar) throw std::runtime_error("Real recording toolbar did not open");
    SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,113,0); PumpFor(4500);
    SendMessageW(bar,WM_COMMAND,113,0);
    HWND preview{}; const auto deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline) {
        preview=FindWindowW(L"PcTool.CaptureToolWindow",L"屏幕录制预览 · PcTool");
        if(preview && IsWindowVisible(preview) && !FindWindowW(L"PcTool.ScreenshotOverlay",nullptr) && !FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 屏幕录制")) break;
        PumpFor(20);
    }
    if(!preview || !IsWindowVisible(preview) || FindWindowW(L"PcTool.ScreenshotOverlay",nullptr) || FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 屏幕录制"))
        throw std::runtime_error("Real application end did not show the preview");
    SendMessageW(preview,WM_CLOSE,0,0); PumpFor(200);
    for(const auto& file:PendingVideos()) if(!before.count(file)) throw std::runtime_error("Live test's discarded recording was not cleaned");
    std::cout<<"LIVE PREVIEW PASS: real background app screenshot -> recording -> visible preview, then discard\n";
}
static void RecordingExitTest(RECT region) {
    for(int mode=0;mode<4;++mode) {
        const auto before=PendingVideos();
        CaptureTools tools; ScreenshotOverlay source(GetModuleHandleW(nullptr)); source.SetCaptureTools(&tools);
        if(!source.Start()) throw std::runtime_error("Screenshot source did not start");
        HWND overlay=FindWindowW(L"PcTool.ScreenshotOverlay",nullptr);
        POINT a{region.left,region.top},b{region.right,region.bottom}; ScreenToClient(overlay,&a); ScreenToClient(overlay,&b);
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));
        SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y));
        SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));
        const int dpi=int(GetDpiForWindow(overlay));
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromRect(&region,MONITOR_DEFAULTTONEAREST),&monitor);
        const int toolbarWidth=MulDiv(36,dpi,96)*16+MulDiv(68,dpi,96);
        POINT record{std::clamp<int>(region.right-toolbarWidth,monitor.rcMonitor.left,monitor.rcMonitor.right-toolbarWidth)+MulDiv(36,dpi,96)*12+MulDiv(18,dpi,96),region.bottom+MulDiv(23,dpi,96)};
        ScreenToClient(overlay,&record);
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(record.x,record.y)); SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(record.x,record.y));
        HWND bar=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 屏幕录制");
        if(!bar || IsWindowVisible(overlay)) throw std::runtime_error("Screenshot did not hand off to recorder");
        if(mode) { SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,113,0); PumpFor(mode==1?1200:4200); }
        if(mode==2) SendMessageW(bar,WM_COMMAND,111,0);
        SendMessageW(bar,WM_COMMAND,mode==3?113:112,0); PumpFor(mode==0?20:900);
        if(source.IsActive() || IsWindow(overlay)) throw std::runtime_error("Recorder returned to screenshot editing");
        if(mode==3 && (!IsWindowVisible(FindWindowW(L"PcTool.CaptureToolWindow",L"屏幕录制预览 · PcTool")) || IsWindow(bar) || tools.FocusActive()))
            throw std::runtime_error("Recorder did not hand off to independent preview");
        tools.Shutdown();
        for(const auto& file:PendingVideos()) if(!before.count(file)) std::filesystem::remove(file);
    }
    std::cout<<"RECORD EXIT PASS: prepare/countdown/paused discard and normal end all close screenshot source\n";
}static Image DesktopUiSnapshot(RECT bounds,const std::wstring& path,bool processMessages=true) {
    std::vector<HWND> excluded;
    EnumWindows([](HWND window,LPARAM data)->BOOL {
        DWORD process{},affinity{}; GetWindowThreadProcessId(window,&process);
        if(process==GetCurrentProcessId() && GetWindowDisplayAffinity(window,&affinity) && affinity==WDA_EXCLUDEFROMCAPTURE)
            reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
        return TRUE;
    },reinterpret_cast<LPARAM>(&excluded));
    struct Restore { std::vector<HWND>& windows; ~Restore(){for(HWND w:windows) if(IsWindow(w)) SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE);} } restore{excluded};
    for(HWND window:excluded) winrt::check_bool(SetWindowDisplayAffinity(window,WDA_NONE));
    if(processMessages)PumpFor(60);else DwmFlush();
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    HDC desktop=GetDC(nullptr),dc=CreateCompatibleDC(desktop); HBITMAP bitmap=CreateCompatibleBitmap(desktop,width,height); auto old=SelectObject(dc,bitmap);
    BitBlt(dc,0,0,width,height,desktop,bounds.left,bounds.top,SRCCOPY|CAPTUREBLT);
    SelectObject(dc,old); DeleteDC(dc); ReleaseDC(nullptr,desktop);
    auto image=FromBitmap(bitmap); DeleteObject(bitmap); SavePng(image,path); return image;
}
static void BidirectionalScrollTest(HWND fixture,const std::wstring& prefix,bool wholeOnly=false){
    bidirectionalFixture=true;partialFixture=false;scrollFixture=false;
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=NestedScrollProc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"PcTool.NestedScrollFixture";RegisterClassW(&wc);
    SetWindowLongPtrW(fixture,GWL_STYLE,WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN);
    SetWindowPos(fixture,HWND_TOPMOST,140,110,900,740,SWP_FRAMECHANGED|SWP_SHOWWINDOW);SetForegroundWindow(fixture);
    POINT origin{};ClientToScreen(fixture,&origin);RECT region{origin.x,origin.y,origin.x+900,origin.y+740};
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(fixture,MONITOR_DEFAULTTONEAREST),&mi);
    std::unique_ptr<LongCaptureSession> session;std::shared_ptr<ImageDocument> document;bool completed=false;
    struct TraceDump {const std::wstring& prefix;~TraceDump(){std::ofstream out(std::filesystem::path(prefix+L"-wheel.csv"));out<<"time,delta,before,target,x,y,window,postedTime\n";for(const auto& d:scrollDeliveries)out<<d.time<<','<<d.delta<<','<<d.before<<','<<d.target<<','<<d.point.x<<','<<d.point.y<<','<<reinterpret_cast<uintptr_t>(d.window)<<','<<d.postedTime<<'\n';}} traceDump{prefix};
    auto explain=[&](const char* reason){const auto p=session->Progress();wchar_t foregroundClass[128]{};GetClassNameW(GetForegroundWindow(),foregroundClass,128);std::cerr<<reason<<" fixture="<<fixtureOffset<<" target="<<partialTarget<<" height="<<p.height<<" current="<<p.analysis.currentY<<" min="<<p.analysis.minY<<" max="<<p.analysis.maxY<<" mode="<<int(p.mode)<<" inFlight="<<p.inFlight<<" action="<<p.actionId<<" done="<<p.completedActionId<<" status="<<winrt::to_string(p.status)<<" foreground="<<winrt::to_string(foregroundClass)<<std::endl;DesktopUiSnapshot(mi.rcMonitor,prefix+L"-failure-desktop.png",false);};
    auto wait=[&](auto predicate,int timeout,const char* reason){const auto end=GetTickCount64()+timeout;while(!predicate()&&GetTickCount64()<end)PumpFor(15);if(!predicate()){explain(reason);throw std::runtime_error(reason);}};
    auto sendWheel=[&](int delta){INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=MOUSEEVENTF_WHEEL;input.mi.mouseData=DWORD(delta);winrt::check_bool(SendInput(1,&input,sizeof(input))==1);};
    auto anchor=[&]{const auto b=BidirectionalBody();SetCursorPos(origin.x+(b.left+b.right)/2,origin.y+(b.top+b.bottom)/2);};
    auto click=[&](bool outsideBody){if(outsideBody)SetCursorPos(origin.x+180,origin.y+310);else anchor();POINT p{};GetCursorPos(&p);RECT preview{};GetWindowRect(session->Window(),&preview);wchar_t cls[100]{};GetClassNameW(WindowFromPoint(p),cls,100);std::cout<<"click "<<GetTickCount64()<<" at="<<p.x<<","<<p.y<<" window="<<winrt::to_string(cls)<<" preview="<<preview.left<<","<<preview.top<<","<<preview.right<<","<<preview.bottom<<" mode="<<int(session->Progress().mode)<<std::endl;INPUT input[2]{};for(auto& i:input)i.type=INPUT_MOUSE;input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;winrt::check_bool(SendInput(2,input,sizeof(INPUT))==2);PumpFor(30);};
    auto close=[&]{if(session){SendMessageW(session->Window(),WM_CLOSE,0,0);PumpFor(350);session.reset();}KillTimer(fixture,901);KillTimer(fixture,902);KillTimer(fixture,903);startupAnimation=0;};
    auto open=[&](bool whole,int start,int limit,bool staleSnapshot=false,bool immediateWheel=false,bool animated=false){
        close();bidirectionalWhole=whole;partialSmooth=false;bidirectionalDelay=0;bidirectionalRemainder=0;partialLimit=limit;fixtureOffset=partialTarget=start;
        if(IsWindow(bidirectionalChild))DestroyWindow(bidirectionalChild);const auto body=BidirectionalBody();
        if(animated){startupAnimation=1;SetTimer(fixture,903,60,nullptr);}
        bidirectionalChild=CreateWindowExW(0,wc.lpszClassName,L"Synthetic nested scroll target",WS_CHILD|WS_VISIBLE,body.left,body.top,body.right-body.left,body.bottom-body.top,fixture,nullptr,wc.hInstance,nullptr);winrt::check_bool(bidirectionalChild!=nullptr);
        FocusNativeFixture(fixture);RepaintScrollFixture(fixture);PumpFor(300);document=std::make_shared<ImageDocument>();completed=false;
        if(staleSnapshot){
            // The screenshot selector retains a frozen image. A hover panel can
            // disappear before live long capture starts, exposing the list.
            document->image=BidirectionalImage(start);
            for(int y=body.top;y<body.bottom;++y)for(int x=body.left;x<body.right;++x)document->image.pixels[size_t(y)*900+x]=0xfffafafa;
        }
        session=OpenLongCaptureSession(region,document,[&](bool ok){completed=ok;},[](auto){});
        if(immediateWheel){const size_t delivered=scrollDeliveries.size();anchor();sendWheel(-120);PumpFor(100);if(scrollDeliveries.size()!=delivered)throw std::runtime_error("Wheel reached source before live baseline was captured");}
        wait([&]{return session->Progress().height==740;},4000,"Initial whole-window capture missing");PumpFor(400);anchor();
    };
    auto wheel=[&](int delta,int start){anchor();const auto id=session->Progress().actionId;sendWheel(delta);
        wait([&]{return session->Progress().actionId>id;},2000,"Controlled wheel was not issued");
        wait([&]{auto p=session->Progress();return p.completedActionId>id&&!p.inFlight;},4000,"Controlled wheel did not complete");
        PumpFor(100); // allow the UI timer to paint the published status
        const auto p=session->Progress();if(p.analysis.locked&&p.analysis.currentY!=fixtureOffset-start){explain("Observed viewport differs from actual source");throw std::runtime_error("Observed viewport differs from actual source");}
    };
    auto finishExact=[&](int start,int minimum,int maximum,const wchar_t* suffix){
        const auto p=session->Progress();if(p.analysis.minY!=minimum-start||p.analysis.maxY!=maximum-start+p.analysis.viewportHeight||p.height!=740+maximum-minimum){explain("Captured interval does not match known source interval");throw std::runtime_error("Captured interval does not match known source interval");}
        SendMessageW(session->Window(),WM_COMMAND,11,0);wait([&]{return completed;},5000,"Edit handoff did not finish");
        const auto body=BidirectionalBody();const int added=maximum-minimum;auto first=BidirectionalImage(start),last=BidirectionalImage(maximum);
        if(document->image.width!=900||document->image.height!=740+added)throw std::runtime_error("Bidirectional output extent incorrect");
        for(int y=0;y<document->image.height;++y)for(int x=0;x<900;++x){uint32_t expected;
            if(y<body.top)expected=first.pixels[size_t(y)*900+x];
            else if(y>=body.bottom+added)expected=last.pixels[size_t(y-added)*900+x];
            else if(x>=body.left&&x<body.right)expected=ScrollFixturePixel(x-body.left,y-body.top+minimum);
            else if(y<body.bottom)expected=first.pixels[size_t(y)*900+x];
            else expected=0xffeceef0;
            if((document->image.pixels[size_t(y)*900+x]&0xffffff)!=(expected&0xffffff)){SavePng(document->image,prefix+L"-failed.png");std::cerr<<"Incorrect pixel x="<<x<<" y="<<y<<" body="<<body.top<<","<<body.bottom<<" min="<<minimum<<" max="<<maximum<<std::endl;throw std::runtime_error("Bidirectional body has missing/repeated rows or duplicated fixed content");}}
        SavePng(document->image,prefix+suffix);session.reset();
    };
    try{
        if(!wholeOnly){
            std::cout<<"BIDIRECTIONAL: stale selector snapshot after hover panel closes"<<std::endl;
            open(false,240,480,true);wheel(-120,240);
            if(!session->Progress().analysis.locked||session->Progress().analysis.currentY!=fixtureOffset-240||session->Progress().height<=740){explain("First scroll compared against stale selector snapshot");throw std::runtime_error("First scroll compared against stale selector snapshot");}
            finishExact(240,240,fixtureOffset,L"-fresh-baseline.png");
            std::cout<<"BIDIRECTIONAL: immediate wheel waits for fresh baseline without losing input"<<std::endl;
            open(false,240,480,true,true);
            wait([&]{const auto p=session->Progress();return p.actionId&&p.completedActionId>=p.actionId&&!p.inFlight;},4000,"Initial wheel was lost while preparing baseline");
            if(!session->Progress().analysis.locked||fixtureOffset<=240)throw std::runtime_error("Immediate first wheel failed to append");
            finishExact(240,240,fixtureOffset,L"-immediate-baseline.png");
            std::cout<<"BIDIRECTIONAL: startup animation outside the requested list"<<std::endl;
            open(false,240,480,true,false,true);wheel(-120,240);
            if(!session->Progress().analysis.locked||fixtureOffset<=240){explain("Unrelated animation blocked the first scroll");throw std::runtime_error("Unrelated animation blocked the first scroll");}
            SendMessageW(session->Window(),WM_COMMAND,11,0);wait([&]{return completed;},5000,"Animated startup edit handoff failed");
            if(document->image.height!=740+fixtureOffset-240)throw std::runtime_error("Animated startup has incorrect output height");
            for(int y=330;y<530+fixtureOffset-240;++y)for(int x=320;x<500;++x)
                if(document->image.pixels[size_t(y)*900+x]!=ScrollFixturePixel(x-320,y-330+240))throw std::runtime_error("Animated startup lost source rows");
            close();
            std::cout<<"BIDIRECTIONAL: partial viewport, up-only auto restore"<<std::endl;
            open(false,240,480);
            const int movesBefore=scrollSourceMoves,buttonsBefore=scrollSourceOtherButtons;
            for(POINT p:{POINT{origin.x+60,origin.y+180},POINT{origin.x+390,origin.y+410},POINT{origin.x+190,origin.y+310}}){
                SetCursorPos(p.x,p.y);PumpFor(250);POINT actual{};GetCursorPos(&actual);if(actual.x!=p.x||actual.y!=p.y)throw std::runtime_error("Input shield freezes cursor movement");
                wchar_t title[100]{};GetWindowTextW(WindowFromPoint(p),title,100);if(std::wstring(title)!=L"PcTool · 长截图输入保护")throw std::runtime_error("Source remains exposed to hover input");
            }
            INPUT extraButtons[4]{};for(auto& i:extraButtons)i.type=INPUT_MOUSE;
            extraButtons[0].mi.dwFlags=MOUSEEVENTF_MIDDLEDOWN;extraButtons[1].mi.dwFlags=MOUSEEVENTF_MIDDLEUP;
            extraButtons[2].mi.dwFlags=MOUSEEVENTF_XDOWN;extraButtons[3].mi.dwFlags=MOUSEEVENTF_XUP;extraButtons[2].mi.mouseData=extraButtons[3].mi.mouseData=XBUTTON1;
            winrt::check_bool(SendInput(4,extraButtons,sizeof(INPUT))==4);PumpFor(150);
            if(scrollSourceMoves!=movesBefore||scrollSourceOtherButtons!=buttonsBefore||GetForegroundWindow()!=fixture)throw std::runtime_error("Hover/buttons reached source or shield stole focus");
            std::cout<<"INPUT SHIELD PASS: free cursor, blocked source hover/middle/side clicks, original focus retained"<<std::endl;
            wheel(120,240);wheel(120,240);const int minimum=fixtureOffset;const int upHeight=session->Progress().height;
            if(minimum>=240||upHeight<=740)throw std::runtime_error("Manual upward scrolling did not prepend");
            PumpFor(800);if(session->Progress().atTop||session->Progress().atEnd)throw std::runtime_error("Idle falsely reported a boundary");
            const size_t beforeAuto=scrollDeliveries.size();const int clicksBefore=fixtureClicks;click(true);
            wait([&]{return scrollDeliveries.size()>beforeAuto;},2500,"Blank-selection click did not control locked child");
            if(scrollDeliveries.back().window!=bidirectionalChild||scrollDeliveries.back().target<=minimum)throw std::runtime_error("Auto restore targeted wrong child/direction");
            const auto limit=GetTickCount64()+18000;while(!session->Progress().atEnd&&GetTickCount64()<limit){const auto p=session->Progress();if(p.analysis.currentY<=0&&p.height!=upHeight)throw std::runtime_error("Restore through already captured rows duplicated output");PumpFor(20);}
            if(!session->Progress().atEnd||fixtureOffset!=480||fixtureClicks!=clicksBefore){explain("Up-only automatic restore failed");throw std::runtime_error("Up-only automatic restore failed");}
            finishExact(240,minimum,480,L"-up-auto.png");
            const int restoredMoves=scrollSourceMoves;SetCursorPos(origin.x+400,origin.y+420);PumpFor(180);
            if(scrollSourceMoves<=restoredMoves||FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 长截图输入保护"))throw std::runtime_error("Finishing capture left source input blocked");
            std::cout<<"INPUT RESTORE PASS: source hover resumes after capture finishes"<<std::endl;

            std::cout<<"BIDIRECTIONAL: revisit, two-sided auto restore, click stop and manual interrupt"<<std::endl;
            open(false,240,480);wheel(-120,240);wheel(-120,240);wheel(-120,240);const int maximum=fixtureOffset;wheel(120,240);const int heightBefore=session->Progress().height;wheel(120,240);
            if(session->Progress().height!=heightBefore)throw std::runtime_error("Revisiting captured range grew output");
            for(int n=0;fixtureOffset>=240&&n<8;++n)wheel(120,240);if(fixtureOffset>=240)throw std::runtime_error("Manual scroll did not extend above initial viewport");const int bothMinimum=fixtureOffset;const int restoreHeight=session->Progress().height;const size_t startRestore=scrollDeliveries.size();click(true);
            wait([&]{return scrollDeliveries.size()>startRestore;},2500,"Both-sided restore did not issue a wheel");click(true);
            wait([&]{return !session->Progress().inFlight;},4000,"Stopped restore did not finish current action");const auto stoppedCount=scrollDeliveries.size();PumpFor(600);if(scrollDeliveries.size()!=stoppedCount)throw std::runtime_error("Second click did not stop restoration");
            click(true);wait([&]{return scrollDeliveries.size()>stoppedCount;},2500,"Restore restart failed");wheel(120,240);const auto interruptedCount=scrollDeliveries.size();PumpFor(600);
            if(scrollDeliveries.size()!=interruptedCount||session->Progress().mode==LongCaptureMode::Automatic||session->Progress().mode==LongCaptureMode::Restoring)throw std::runtime_error("Manual wheel did not interrupt automatic restore");
            click(true);const auto restoreEnd=GetTickCount64()+18000;while(!session->Progress().atEnd&&GetTickCount64()<restoreEnd){const auto p=session->Progress();if(p.analysis.currentY<=maximum-240&&p.height!=restoreHeight)throw std::runtime_error("Lower anchor restore duplicated captured interval");PumpFor(20);}
            if(!session->Progress().atEnd||fixtureOffset!=480)throw std::runtime_error("Both-sided restore did not continue below latest lower anchor");finishExact(240,bothMinimum,480,L"-both-auto.png");

            std::cout<<"BIDIRECTIONAL: bounded rapid input and reversal"<<std::endl;
            open(false,240,1200);const size_t burstStart=scrollDeliveries.size();anchor();std::vector<INPUT> burst(40);for(auto& i:burst){i.type=INPUT_MOUSE;i.mi.dwFlags=MOUSEEVENTF_WHEEL;i.mi.mouseData=DWORD(-1200);}
            // A low-level hook executes on the installing UI thread. Inject a
            // large batch from a separate input producer while pumping that
            // thread, just as physical input arrives independently of it.
            auto producer=std::async(std::launch::async,[&]{return SendInput(UINT(burst.size()),burst.data(),sizeof(INPUT));});while(producer.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)PumpFor(10);winrt::check_bool(producer.get()==burst.size());PumpFor(1600);
            const size_t delivered=scrollDeliveries.size()-burstStart;if(delivered<1||delivered>2)throw std::runtime_error("Rapid wheel input was forwarded directly or replayed as a long queue");
            for(size_t i=burstStart;i<scrollDeliveries.size();++i){const auto& d=scrollDeliveries[i];if(std::abs(d.target-d.before)>40||std::abs(d.delta)>60)throw std::runtime_error("Controlled step exceeds slow-scroll target");if(i>burstStart&&d.time-scrollDeliveries[i-1].time<345)throw std::runtime_error("Controlled steps violate minimum pacing");}
            const auto afterBurst=scrollDeliveries.size();PumpFor(800);if(scrollDeliveries.size()!=afterBurst)throw std::runtime_error("Source continued scrolling after burst had stopped");
            const int rapidMaximum=fixtureOffset;wheel(120,240);if(fixtureOffset>=rapidMaximum)throw std::runtime_error("Reverse wheel retained old pending direction");finishExact(240,std::min(240,fixtureOffset),rapidMaximum,L"-burst.png");

            std::cout<<"BIDIRECTIONAL: smooth/delayed response, one-event top and bottom, repeated hints, DPI"<<std::endl;
            open(false,60,180);partialSmooth=true;wheel(120,60);if(fixtureOffset>=60)throw std::runtime_error("Smooth upward scroll not observed");
            for(int n=0;fixtureOffset>0&&n<40;++n)wheel(120,60);if(fixtureOffset!=0)throw std::runtime_error("Controlled smooth scroll never reached top");wheel(120,60);
            if(!session->Progress().atTop||session->Progress().status!=L"已到顶部")throw std::runtime_error("Single ineffective upward wheel did not show top hint");
            HWND hint=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 长图提示");PumpFor(3150);if(IsWindowVisible(hint)||!session->Active())throw std::runtime_error("Top hint did not expire without closing");wheel(120,60);if(!IsWindowVisible(hint)||!session->Progress().atTop)throw std::runtime_error("Repeated top wheel did not redisplay hint");
            partialSmooth=false;bidirectionalDelay=250;wheel(-120,60);if(fixtureOffset<=0||session->Progress().atTop||session->Progress().atEnd)throw std::runtime_error("Delayed moving frame incorrectly treated as boundary");bidirectionalDelay=0;
            for(int n=0;fixtureOffset<partialLimit&&n<40;++n)wheel(-120,60);if(fixtureOffset!=partialLimit)throw std::runtime_error("Controlled scroll never reached bottom");wheel(-120,60);
            if(!session->Progress().atEnd||session->Progress().status!=L"已到底部，可编辑、下载或完成")throw std::runtime_error("Single ineffective downward wheel did not show bottom hint");
            for(int dpi:{96,144,192}){RECT r{};GetWindowRect(session->Window(),&r);SendMessageW(session->Window(),WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));wheel(-120,60);
                if(!IsWindowVisible(hint)||!session->Progress().atEnd||session->Progress().status!=L"已到底部，可编辑、下载或完成")throw std::runtime_error("DPI bottom hint is not showing the expected boundary text");const auto desktop=DesktopUiSnapshot(mi.rcMonitor,prefix+L"-boundary-"+std::to_wstring(dpi)+L".png",false);RECT hr{};GetWindowRect(hint,&hr);int dark=0,count=0;
                for(int y=std::max(hr.top+3,mi.rcMonitor.top);y<std::min(hr.bottom-3,mi.rcMonitor.bottom);++y)for(int x=std::max(hr.left+3,mi.rcMonitor.left);x<std::min(hr.right-3,mi.rcMonitor.right);++x){auto p=desktop.pixels[size_t(y-mi.rcMonitor.top)*desktop.width+x-mi.rcMonitor.left];++count;if((p&255)<110&&((p>>8)&255)<110&&((p>>16)&255)<110)++dark;}if(count==0||dark<count/2)throw std::runtime_error("Boundary label is not drawn on the actual desktop");
                // This evidence-only screenshot briefly disables capture
                // exclusion. Drain those artificial compositor frames before
                // the next real action; production never toggles it this way.
                PumpFor(800);
            }
            PumpFor(3150);if(IsWindowVisible(hint)||!session->Active())throw std::runtime_error("Bottom hint did not expire without closing");wheel(-120,60);if(!IsWindowVisible(hint))throw std::runtime_error("Repeated bottom wheel did not redisplay hint");
            partialLimit=240;fixtureOffset=partialTarget=240;RepaintScrollFixture(fixture);wait([&]{return session->Progress().analysis.currentY==180&&!session->Progress().atEnd;},3500,"Late content did not append and clear boundary");const auto lateCount=scrollDeliveries.size();PumpFor(500);if(scrollDeliveries.size()!=lateCount)throw std::runtime_error("Late content unexpectedly restarted automatic scrolling");
            finishExact(60,0,240,L"-smooth-late.png");

            open(false,180,180);wheel(-120,180);if(session->Progress().status!=L"未检测到可滚动内容"||session->Progress().analysis.locked)throw std::runtime_error("Initially stationary source incorrectly claimed a known bottom");close();
            std::cout<<"BIDIRECTIONAL: moved source and focus loss retain captured result"<<std::endl;
            open(false,120,480);wheel(-120,120);const int sourceHeight=session->Progress().height;
            SetWindowPos(fixture,nullptr,160,110,0,0,SWP_NOSIZE|SWP_NOZORDER);wait([&]{return session->Progress().status.find(L"改变")!=std::wstring::npos;},2500,"Moved source did not pause capture");
            if(session->Progress().height!=sourceHeight||session->Progress().atTop||session->Progress().atEnd)throw std::runtime_error("Moved source corrupted image or reported a boundary");
            SetWindowPos(fixture,nullptr,140,110,0,0,SWP_NOSIZE|SWP_NOZORDER);PumpFor(400);
            HWND focus=CreateWindowExW(WS_EX_TOPMOST,L"STATIC",L"PcTool independent focus fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,std::min(int(mi.rcWork.right)-260,int(region.right)+30),int(mi.rcWork.top)+40,250,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);winrt::check_bool(focus!=nullptr);SetForegroundWindow(focus);
            wait([&]{return session->Progress().status.find(L"失去焦点")!=std::wstring::npos;},2500,"Source focus loss did not pause capture");if(session->Progress().height!=sourceHeight||session->Progress().atTop||session->Progress().atEnd)throw std::runtime_error("Source focus loss corrupted image or reported a boundary");DestroyWindow(focus);SetForegroundWindow(fixture);close();
        }
        std::cout<<"BIDIRECTIONAL: whole-body exact up/down composition"<<std::endl;
        open(true,240,480);wheel(120,240);wheel(120,240);const int fullMinimum=fixtureOffset;wheel(-120,240);wheel(-120,240);wheel(-120,240);const int fullMaximum=fixtureOffset;wheel(120,240);
        finishExact(240,fullMinimum,fullMaximum,L"-whole.png");
        // Validate OS queue timestamps, not callback time after painting: queue
        // scheduling can compress consecutive receive timestamps by one tick.
        for(size_t i=1;i<scrollDeliveries.size();++i)if(DWORD(scrollDeliveries[i].postedTime-scrollDeliveries[i-1].postedTime)<345)throw std::runtime_error("Manual/automatic/supplement posting violates slow pacing");
        if(!wholeOnly){open(false,120,480);wheel(-120,120);const int closedHeight=session->Progress().height;DestroyWindow(fixture);wait([&]{return session->Progress().status.find(L"已关闭")!=std::wstring::npos;},2500,"Closed source did not pause capture");if(session->Progress().height!=closedHeight||session->Progress().atTop||session->Progress().atEnd)throw std::runtime_error("Closed source corrupted image or reported a boundary");}
        close();if(IsWindow(bidirectionalChild))DestroyWindow(bidirectionalChild);bidirectionalChild=nullptr;bidirectionalFixture=false;
    }catch(...){close();if(IsWindow(bidirectionalChild))DestroyWindow(bidirectionalChild);bidirectionalChild=nullptr;bidirectionalFixture=false;throw;}
    std::cout<<"BIDIRECTIONAL SCROLL PASS: real native child wheel delivery, bounded pacing, bidirectional exact composition, revisit, lower-anchor restore, interrupt/stop, smooth/delayed response, single/repeated boundaries and desktop DPI hints\n";
}
static void ScrollAnnotationEditorTest(const std::wstring& prefix){
    auto document=std::make_shared<ImageDocument>();document->image=Image(320,350);std::fill(document->image.pixels.begin(),document->image.pixels.end(),0xffffffff);
    Annotation pen;pen.tool=Tool::Pen;pen.start={40,40};pen.end={280,220};pen.points={{40,40},{280,220}};pen.size=5;pen.color=RGB(210,45,70);
    document->annotations=MapScrollAnnotations({pen},{90,75,235,190},320,260,45,90,96);const auto original=document->annotations;
    if(original.size()<3||!original.front().fragmentGroup)throw std::runtime_error("Cross-region mark was not divided into linked fragments");
    auto editor=OpenImageEditor(document,RECT{140,110,460,460});HWND w=editor->Window();SendMessageW(w,WM_COMMAND,125,0);SendMessageW(w,WM_COMMAND,100,0);PumpFor(150);
    // Original diagonal crosses (160,130). Its body fragment has been moved
    // down by45, so select that actual painted fragment at (160,175).
    SendMessageW(w,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(160,175));SendMessageW(w,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(175,185));SendMessageW(w,WM_LBUTTONUP,0,MAKELPARAM(175,185));
    if(document->annotations.size()!=original.size())throw std::runtime_error("Native selection changed linked fragment count");
    for(size_t i=0;i<original.size();++i){const auto& a=document->annotations[i];if(a.start.x!=original[i].start.x+15||a.start.y!=original[i].start.y+10||a.clip->left!=original[i].clip->left+15||a.clip->top!=original[i].clip->top+10)throw std::runtime_error("Dragging visible long-capture fragment did not move its complete logical annotation");}
    SavePng(RenderDocument(*document),prefix+L"-moved.png");SendMessageW(w,WM_KEYDOWN,VK_DELETE,0);if(!document->annotations.empty())throw std::runtime_error("Delete left orphaned long-capture fragments");
    SendMessageW(w,WM_COMMAND,110,0);if(document->annotations.size()!=original.size())throw std::runtime_error("Undo did not restore all linked fragments");SendMessageW(w,WM_COMMAND,110,0);
    for(size_t i=0;i<original.size();++i)if(document->annotations[i].start.x!=original[i].start.x||document->annotations[i].start.y!=original[i].start.y)throw std::runtime_error("Undo did not restore linked mark position");
    SendMessageW(w,WM_CLOSE,0,0);editor.reset();std::cout<<"SCROLL ANNOTATION EDITOR PASS: native hit on moved body fragment, whole-mark drag/delete and grouped undo\n";
}
static void LiveMenuTest(const std::wstring& prefix) {
    if(FindWindowW(L"PcTool.ScreenshotOverlay",nullptr) || FindWindowW(L"PcTool.CaptureToolWindow",nullptr))
        throw std::runtime_error("Close existing capture windows before live menu test");
    HWND monitor{};
    EnumChildWindows(FindWindowW(L"Shell_TrayWnd",nullptr),[](HWND w,LPARAM data)->BOOL {
        wchar_t cls[100]{}; GetClassNameW(w,cls,100);
        if(wcscmp(cls,L"PcTool.TaskbarMonitor")==0) *reinterpret_cast<HWND*>(data)=w;
        return TRUE;
    },reinterpret_cast<LPARAM>(&monitor));
    if(!monitor) throw std::runtime_error("Live taskbar monitor missing");
    RECT r{}; GetWindowRect(monitor,&r); SetCursorPos((r.left+r.right)/2,(r.top+r.bottom)/2);
    INPUT mouse[2]{}; mouse[0].type=mouse[1].type=INPUT_MOUSE;
    mouse[0].mi.dwFlags=MOUSEEVENTF_RIGHTDOWN; mouse[1].mi.dwFlags=MOUSEEVENTF_RIGHTUP;
    winrt::check_bool(SendInput(2,mouse,sizeof(INPUT))==2); PumpFor(300);
    HWND menu=FindWindowW(L"#32768",nullptr);
    if(!menu) throw std::runtime_error("Right click did not open menu");
    RECT bounds{}; GetWindowRect(menu,&bounds);
    auto before=DesktopUiSnapshot(bounds,prefix+L"-before.png");
    const auto key=[](WORD value,bool up=false) { INPUT input{}; input.type=INPUT_KEYBOARD; input.ki.wVk=value; input.ki.dwFlags=up?KEYEVENTF_KEYUP:0; SendInput(1,&input,sizeof(input)); PumpFor(80); };
    key(VK_CONTROL); key(VK_MENU);
    const bool preserved=IsWindowVisible(menu)!=FALSE;
    key('A'); key('A',true); key(VK_MENU,true); key(VK_CONTROL,true); PumpFor(200);
    HWND overlay=FindWindowW(L"PcTool.ScreenshotOverlay",nullptr);
    if(!overlay) throw std::runtime_error("Real screenshot hotkey did not open overlay");
    struct Cleanup { HWND w; ~Cleanup(){ if(IsWindow(w)) SendMessageW(w,WM_KEYDOWN,VK_ESCAPE,0); } } cleanup{overlay};
    if(!preserved) throw std::runtime_error("Modifiers dismissed live menu");
    POINT a{bounds.left,bounds.top},b{bounds.right,bounds.bottom}; ScreenToClient(overlay,&a); ScreenToClient(overlay,&b);
    SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));
    SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y));
    SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y)); PumpFor(150);
    auto after=DesktopUiSnapshot(bounds,prefix+L"-captured.png");
    int same=0,total=0;
    for(int y=8;y<before.height-8;++y) for(int x=8;x<before.width-8;++x) {
        const size_t i=size_t(y)*before.width+x; ++total;
        if((before.pixels[i]&0xffffff)==(after.pixels[i]&0xffffff)) ++same;
    }
    if(same<total*9/10) throw std::runtime_error("Screenshot did not preserve the menu pixels");
    std::cout<<"LIVE MENU PASS: native right-click and Ctrl+Alt+A preserved "<<same<<"/"<<total<<" menu pixels\n";
}
static void LiveClipboardHotkeyTest() {
    if(FindWindowW(L"PcTool.ClipboardHistory",nullptr)) throw std::runtime_error("Close history before hotkey test");
    const auto chord=[] {
        INPUT keys[4]{}; for(auto& key:keys) key.type=INPUT_KEYBOARD;
        keys[0].ki.wVk=keys[3].ki.wVk=VK_MENU; keys[1].ki.wVk=keys[2].ki.wVk='C';
        keys[2].ki.dwFlags=keys[3].ki.dwFlags=KEYEVENTF_KEYUP;
        if(SendInput(4,keys,sizeof(INPUT))!=4) throw std::runtime_error("Hotkey input failed");
        PumpFor(300);
    };
    chord(); HWND history=FindWindowW(L"PcTool.ClipboardHistory",nullptr);
    if(!history || !IsWindowVisible(history)) throw std::runtime_error("Alt+C did not open history");
    chord();
    if(FindWindowW(L"PcTool.ClipboardHistory",nullptr)!=history || !IsWindowVisible(history)) throw std::runtime_error("Repeated Alt+C did not preserve history window");
    std::vector<RECT> workAreas;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
        MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor,&info);
        reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcWork); return TRUE;
    },reinterpret_cast<LPARAM>(&workAreas));
    for(const auto& area:workAreas) for(POINT point:{POINT{area.left+2,area.top+2},POINT{area.right-2,area.bottom-2}}) {
        SetCursorPos(point.x,point.y); chord(); RECT bounds{}; GetWindowRect(history,&bounds);
        const LONG expectedX=std::clamp<LONG>(point.x-(bounds.right-bounds.left)/2,area.left,area.right-(bounds.right-bounds.left));
        const LONG expectedY=std::clamp<LONG>(point.y-(bounds.bottom-bounds.top),area.top,area.bottom-(bounds.bottom-bounds.top));
        if(bounds.left!=expectedX || bounds.top!=expectedY || bounds.right>area.right || bounds.bottom>area.bottom ||
           FindWindowW(L"PcTool.ClipboardHistory",nullptr)!=history) throw std::runtime_error("Alt+C did not reposition existing window at cursor within work area");
    }
    SendMessageW(history,WM_CLOSE,0,0); PumpFor(100);
    chord(); history=FindWindowW(L"PcTool.ClipboardHistory",nullptr);
    if(!history || !IsWindowVisible(history)) throw std::runtime_error("Alt+C did not reopen history");
    SendMessageW(history,WM_CLOSE,0,0);
    std::cout<<"LIVE CLIPBOARD HOTKEY PASS: Alt+C open, reuse, reposition at monitor edges and reopen\n";
}
static void LiveUtilitiesTest(const std::wstring& prefix) {
    if(FindWindowW(L"PcTool.ClipboardHistory",nullptr)) throw std::runtime_error("Close existing clipboard history before test");
    HWND monitor{};
    EnumChildWindows(FindWindowW(L"Shell_TrayWnd",nullptr),[](HWND w,LPARAM data)->BOOL {
        wchar_t name[100]{}; GetClassNameW(w,name,100); if(wcscmp(name,L"PcTool.TaskbarMonitor")==0) *reinterpret_cast<HWND*>(data)=w; return TRUE;
    },reinterpret_cast<LPARAM>(&monitor));
    if(!monitor) throw std::runtime_error("Start PcTool before utilities test");
    const auto click=[](POINT point,bool right) { SetCursorPos(point.x,point.y); INPUT mouse[2]{}; mouse[0].type=mouse[1].type=INPUT_MOUSE; mouse[0].mi.dwFlags=right?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_LEFTDOWN; mouse[1].mi.dwFlags=right?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_LEFTUP; SendInput(2,mouse,sizeof(INPUT)); PumpFor(250); };
    const auto command=[&](int index) {
        RECT r{}; GetWindowRect(monitor,&r); click({(r.left+r.right)/2,(r.top+r.bottom)/2},true);
        HWND menu=FindWindowW(L"#32768",nullptr); if(!menu) throw std::runtime_error("Menu missing");
        HMENU handle=reinterpret_cast<HMENU>(SendMessageW(menu,0x01e1,0,0));
        if(!GetMenuItemRect(nullptr,handle,UINT(index),&r)) throw std::runtime_error("Menu geometry missing");
        click({(r.left+r.right)/2,(r.top+r.bottom)/2},false);
    };
    command(5); HWND history=FindWindowW(L"PcTool.ClipboardHistory",nullptr);
    if(!history || !IsWindowVisible(history)) throw std::runtime_error("History menu command did not open window");
    RECT bounds{}; GetWindowRect(history,&bounds); DesktopUiSnapshot(bounds,prefix+L"-history.png");
    SendMessageW(history,WM_CLOSE,0,0); PumpFor(50);
    HWND previousPaint=FindWindowW(L"MSPaintApp",nullptr);
    if(previousPaint) throw std::runtime_error("Existing Paint window; skip launching test to preserve it");
    command(6); HWND paint{}; const auto deadline=GetTickCount64()+5000;
    while(!paint && GetTickCount64()<deadline) { paint=FindWindowW(L"MSPaintApp",nullptr); PumpFor(30); }
    if(!paint || !IsWindowVisible(paint)) throw std::runtime_error("Paint command did not launch system Paint");
    PostMessageW(paint,WM_CLOSE,0,0);
    std::cout<<"LIVE UTILITIES PASS: native menu opens clipboard history and system Paint\n";
}
static void RecordingVisualTest(HWND fixture,const std::wstring& prefix) {
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(fixture,MONITOR_DEFAULTTONEAREST),&mi);
    // Cover the monitor with our own neutral fixture before capturing UI states.
    SetWindowLongPtrW(fixture,GWL_STYLE,WS_POPUP|WS_VISIBLE);
    SetWindowPos(fixture,HWND_TOPMOST,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED|SWP_SHOWWINDOW);
    InvalidateRect(fixture,nullptr,TRUE); UpdateWindow(fixture);
    RECT region{mi.rcMonitor.left+80,mi.rcMonitor.top+80,mi.rcMonitor.left+1080,mi.rcMonitor.top+620};
    auto session=OpenRecordingSession(region,[](bool){}); HWND bar=session->Window();
    HWND display=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏边框");
    HWND layer=FindWindowExW(display,nullptr,L"PcTool.RecordingSurface",L"PcTool · 录屏平滑标注");
    const int nativeDpi=int(GetDpiForWindow(bar));
    auto dpi=[&](int value){ RECT r{}; GetWindowRect(bar,&r); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(value,value),reinterpret_cast<LPARAM>(&r)); };
    for(int value:{96,144,192}) { dpi(value); DesktopUiSnapshot(mi.rcMonitor,prefix+L"-prepare-"+std::to_wstring(value)+L".png"); }
    dpi(nativeDpi); SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,113,0);
    const auto deadline=GetTickCount64()+2500;
    while(!IsWindowVisible(layer) && GetTickCount64()<deadline) PumpFor(20);
    if(!IsWindowVisible(layer)) throw std::runtime_error("Countdown overlay did not appear");
    for(int value:{96,144,192}) { dpi(value); DesktopUiSnapshot(mi.rcMonitor,prefix+L"-countdown-"+std::to_wstring(value)+L".png"); }
    dpi(nativeDpi); PumpFor(3200); SendMessageW(bar,WM_COMMAND,111,0);
    HWND input=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏标注");
    for(int value:{96,144,192}) {
        dpi(value); SendMessageW(bar,WM_COMMAND,106,0); SendMessageW(bar,WM_COMMAND,101,0);
        SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(200,130));
        for(int x=205;x<=380;x+=5) SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,130+int(35*std::sin(x/25.0))));
        SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(385,145));
        HWND panel=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
        if(!IsWindowVisible(panel)) throw std::runtime_error("Drawing dismissed the style palette");
        DesktopUiSnapshot(mi.rcMonitor,prefix+L"-pen-paused-"+std::to_wstring(value)+L".png");
        SendMessageW(bar,WM_COMMAND,105,0); SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(200,230));
        HWND host=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏文字"),edit=GetDlgItem(host,1); RECT initial{},expanded{}; GetWindowRect(host,&initial);
        SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"录屏文字 Chinese ABC 123\r\n第二行：输入与提交保持一致")); PumpFor(30); GetWindowRect(host,&expanded);
        if(expanded.bottom-expanded.top<=initial.bottom-initial.top) throw std::runtime_error("Multiline editor did not grow");
        SendMessageW(edit,WM_IME_STARTCOMPOSITION,0,0); SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);
        if(!IsWindow(host)) throw std::runtime_error("IME Escape committed the annotation"); SendMessageW(edit,WM_IME_ENDCOMPOSITION,0,0);
        const auto beforeCommit=DesktopUiSnapshot(mi.rcMonitor,prefix+L"-text-edit-"+std::to_wstring(value)+L".png");
        SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0); if(IsWindowVisible(input) || IsWindowVisible(panel)) throw std::runtime_error("Text commit retained drawing mode");
        const auto afterCommit=DesktopUiSnapshot(mi.rcMonitor,prefix+L"-text-committed-"+std::to_wstring(value)+L".png");
        auto textRows=[&](const Image& image) {
            std::vector<int> rows;
            for(int y=expanded.top+1;y<expanded.bottom-1;++y) {
                bool ink=false;
                for(int x=expanded.left+1;x<expanded.right-1;++x) {
                    const auto pixel=image.pixels[size_t(y-mi.rcMonitor.top)*image.width+x-mi.rcMonitor.left];
                    if(((pixel>>16)&255)>200 && ((pixel>>8)&255)<150) { ink=true; break; }
                }
                if(ink) rows.push_back(y);
            }
            return rows;
        };
        const auto beforeRows=textRows(beforeCommit),afterRows=textRows(afterCommit);
        if(beforeRows.empty() || afterRows.empty() || std::abs(beforeRows.front()-afterRows.front())>1 || std::abs(beforeRows.back()-afterRows.back())>1)
            throw std::runtime_error("Text moved vertically or changed line spacing on commit");
        SendMessageW(bar,WM_COMMAND,108,0);
    }
    SendMessageW(bar,WM_COMMAND,112,0); PumpFor(600); session.reset();
    std::cout<<"RECORD VISUAL PASS: prepare/countdown/pen/style/text/IME/paused at 96/144/192 DPI\n";
}static void RecordingPrepareTest() {
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&info);
    RECT region{info.rcMonitor.left+40,info.rcMonitor.top+40,info.rcMonitor.left+681,info.rcMonitor.top+401};
    int completed=0;
    auto session=OpenRecordingSession(region,[&](bool success) { if(success) throw std::runtime_error("Cancel returned success"); ++completed; });
    HWND display=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏边框");
    HWND layer=FindWindowExW(display,nullptr,L"PcTool.RecordingSurface",L"PcTool · 录屏平滑标注");
    RECT bounds{}; DWORD affinity{};
    HWND shade=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏准备遮罩");
    HRGN mask=CreateRectRgn(0,0,0,0); GetWindowRgn(shade,mask);
    const bool validMask=IsWindowVisible(shade) && PtInRegion(mask,1,1) && !PtInRegion(mask,100,100); DeleteObject(mask);
    if(!validMask) throw std::runtime_error("Preparation mask does not cut out the selection");
    if(!layer || !GetWindowRect(layer,&bounds) || !EqualRect(&bounds,&region)) throw std::runtime_error("Alpha child is missing or positioned incorrectly");
    if(!GetWindowDisplayAffinity(GetAncestor(layer,GA_ROOT),&affinity) || affinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Alpha child does not inherit capture exclusion");
    if((GetWindowLongPtrW(layer,GWL_EXSTYLE)&(WS_EX_LAYERED|WS_EX_TRANSPARENT))!=(WS_EX_LAYERED|WS_EX_TRANSPARENT)) throw std::runtime_error("Annotation child cannot pass through desktop clicks");
    HWND dot=FindWindowExW(display,nullptr,L"PcTool.RecordingSurface",L"PcTool · 录屏激光光点");
    SendMessageW(session->Window(),WM_COMMAND,106,0);SetCursorPos(region.left+40,region.top+50);PumpFor(60);
    if(!IsWindowVisible(dot))throw std::runtime_error("MP4 prepare laser preview missing");
    SendMessageW(session->Window(),WM_COMMAND,114,0);SendMessageW(session->Window(),WM_COMMAND,106,0);PumpFor(40);
    if(IsWindowVisible(dot))throw std::runtime_error("MP4 prepare cursor off did not disable laser");
    SendMessageW(session->Window(),WM_COMMAND,114,0);PumpFor(40);
    if(IsWindowVisible(dot))throw std::runtime_error("MP4 prepare cursor on restored laser automatically");
    SendMessageW(session->Window(),WM_COMMAND,112,0);
    if(session->Active() || completed!=1) throw std::runtime_error("Preparation cancel did not complete exactly once");
    session.reset();
    if(IsWindow(layer) || IsWindow(display)) throw std::runtime_error("Recording surfaces survived cancellation");
    session=OpenRecordingSession(info.rcMonitor,[&](bool success){if(success)throw std::runtime_error("Full screen cancel succeeded");++completed;});
    HWND bar=session->Window();RECT before{},after{};GetWindowRect(bar,&before);
    auto mouse=[](DWORD flag){INPUT event{};event.type=INPUT_MOUSE;event.mi.dwFlags=flag;if(SendInput(1,&event,sizeof(event))!=1)throw std::runtime_error("Native mouse failed");PumpFor(80);};
    SetCursorPos(before.left+2,before.top+2);mouse(MOUSEEVENTF_LEFTDOWN);
    SetCursorPos(before.left-98,before.top-98);PumpFor(100);mouse(MOUSEEVENTF_LEFTUP);GetWindowRect(bar,&after);
    if(EqualRect(&before,&after)||GetCapture()==bar)throw std::runtime_error("Full screen toolbar drag intercepted");
    const int d=int(GetDpiForWindow(bar));
    SetCursorPos(after.left+MulDiv(ToolbarButtonDip*4+ToolbarButtonDip/2,d,96),after.top+MulDiv(ToolbarHeightDip/2,d,96));
    mouse(MOUSEEVENTF_LEFTDOWN);mouse(MOUSEEVENTF_LEFTUP);
    if(session->Active()||completed!=2)throw std::runtime_error("Full screen toolbar click intercepted");
    session.reset();
    std::cout<<"RECORD PREPARE PASS: alpha child, capture exclusion, screen coordinates and direct cancellation\n";
}

// Exercise the shared pointer state through native window messages in both encoders.
static void IndependentPointerControls(HWND bar,RECT region,const std::wstring& prefix) {
    auto command=[&](int id){SendMessageW(bar,WM_COMMAND,id,0);};
    HWND display=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏边框");
    HWND input=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏标注");
    HWND dot=FindWindowExW(display,nullptr,L"PcTool.RecordingSurface",L"PcTool · 录屏激光光点");
    auto visible=[&](bool expected){
        SetCursorPos(region.left+20,region.top+30);PumpFor(60);
        if(bool(IsWindowVisible(dot))!=expected)throw std::runtime_error("Independent laser visibility mismatch");
    };
    command(106);visible(true);
    for(int tool:{101,102,103,104,105}) {
        command(tool);visible(true);
        if(!IsWindowVisible(input))throw std::runtime_error("Laser disabled annotation input");
    }
    SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(70,80));
    HWND text=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏文字");
    HWND edit=GetDlgItem(text,1);SetWindowTextW(edit,L"独立 ABC");
    const auto color=SendMessageW(edit,WM_GETFONT,0,0);
    command(106);command(106); // show laser settings without committing text
    HWND panel=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
    const auto layout=MakeStylePanelLayout({},GetDpiForWindow(bar),false,true);
    ClickStyle(panel,layout.colors[5]);ClickStyle(panel,layout.widths[2]);
    if(!IsWindow(text)||WindowText(edit)!=L"独立 ABC"||SendMessageW(edit,WM_GETFONT,0,0)!=color)
        throw std::runtime_error("Laser settings replaced or restyled text draft");
    command(114);visible(false);command(106);visible(false);
    if(!IsWindow(text)||WindowText(edit)!=L"独立 ABC"||!IsWindowVisible(input))throw std::runtime_error("Cursor toggle lost annotation tool or draft");
    command(114);visible(false);command(106);visible(true);
    ClickStyle(panel,layout.colors[0]);ClickStyle(panel,layout.widths[0]);
    SendMessageW(bar,WM_KEYDOWN,VK_ESCAPE,0);visible(true);
    command(107);visible(true);
    command(101);
    SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(50,50));
    SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(90,70));
    SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(90,70));
    command(108);visible(true);
    for(int dpi:{96,144,192}) {
        RECT bounds{};GetWindowRect(bar,&bounds);SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&bounds));
        WindowSnapshot(bar,prefix+L"-independent-dpi"+std::to_wstring(dpi)+L".png");
        command(114);WindowSnapshot(bar,prefix+L"-cursor-hidden-dpi"+std::to_wstring(dpi)+L".png");
        command(114);command(106);
    }
    RECT bounds{};GetWindowRect(bar,&bounds);const auto dpi=GetDpiForWindow(bar);
    SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&bounds));
    command(111);command(114);visible(false);command(114);visible(false);command(106);visible(true);command(111);
    command(106);visible(false);
    SendMessageW(bar,WM_KEYDOWN,VK_ESCAPE,0);
    if(IsWindowVisible(input))throw std::runtime_error("Esc retained annotation input");
    std::cout<<"INDEPENDENT POINTER PASS: tools, draft, settings, undo/clear/Esc, cursor gating, pause and DPI"<<std::endl;
}

static void RecordingUiTest(RECT region,const std::wstring& prefix) {
    bool complete=false,success=false;
    auto session=OpenRecordingSession(region,[&](bool ok){complete=true;success=ok;});
    HWND bar=session->Window(),input=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏标注"),display=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏边框");
    for(HWND w:{bar,input,display}) { DWORD affinity=0; if(!GetWindowDisplayAffinity(w,&affinity) || affinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Recording UI not excluded"); }
    std::cout<<"UI prepare"<<std::endl;
    WindowSnapshot(bar,prefix+L"-prepare.png");
    const int dpi=int(GetDpiForWindow(bar));
    HWND tooltip=nullptr;
    for(HWND w=FindWindowExW(nullptr,nullptr,TOOLTIPS_CLASSW,nullptr);w;w=FindWindowExW(nullptr,w,TOOLTIPS_CLASSW,nullptr))
        if(GetWindow(w,GW_OWNER)==bar) { tooltip=w; break; }
    DWORD tipAffinity{};
    if(!tooltip || !GetWindowDisplayAffinity(tooltip,&tipAffinity) || tipAffinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Native recording tooltip missing or not excluded");
    if(SendMessageW(tooltip,TTM_GETDELAYTIME,TTDT_INITIAL,0)!=450 || SendMessageW(tooltip,TTM_GETDELAYTIME,TTDT_RESHOW,0)!=100) throw std::runtime_error("Recording tooltip timing differs from screenshot");
    RECT barBounds{}; GetWindowRect(bar,&barBounds);
    SetCursorPos(barBounds.left+MulDiv(18,dpi,96),barBounds.top+MulDiv(18,dpi,96));
    SendMessageW(bar,WM_MOUSEMOVE,0,MAKELPARAM(MulDiv(18,dpi,96),MulDiv(18,dpi,96))); PumpFor(500);
    if(!IsWindowVisible(tooltip)) throw std::runtime_error("Native tooltip did not appear on hover");
    if(!GetWindowDisplayAffinity(tooltip,&tipAffinity) || tipAffinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Shown tooltip lost capture exclusion");
    WindowSnapshot(tooltip,prefix+L"-tooltip.png"); SendMessageW(tooltip,TTM_POP,0,0);
    for(int testDpi:{96,144,192}) {
        RECT bounds{}; GetWindowRect(bar,&bounds); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(testDpi,testDpi),reinterpret_cast<LPARAM>(&bounds));
        const int cell=MulDiv(ToolbarButtonDip,testDpi,96);
        const LPARAM center=MAKELPARAM(cell/2,cell/2);
        for(bool pressed:{false,true}) {
            SendMessageW(bar,WM_MOUSEMOVE,0,center);
            if(pressed) SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,center);
            const auto actual=WindowSnapshot(bar,prefix+(pressed?L"-pressed-dpi":L"-hover-dpi")+std::to_wstring(testDpi)+L".png");
            if(actual.height!=cell) throw std::runtime_error("Recording toolbar height differs from screenshot");
            Image expected(cell,cell); std::fill(expected.pixels.begin(),expected.pixels.end(),0xff000000|((ToolbarBackground&255)<<16)|(ToolbarBackground&0xff00)|((ToolbarBackground>>16)&255));
            HBITMAP bitmap=ToBitmap(expected); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
            DrawToolbarButtonState(dc,{0,0,cell,cell},testDpi,true,false,true,pressed); DrawToolbarIcon(dc,ToolbarIcon::System,{0,0,cell,cell},testDpi,ToolbarInk);
            SelectObject(dc,old); DeleteDC(dc); expected=FromBitmap(bitmap); DeleteObject(bitmap);
            for(int y=1;y<cell-1;++y) for(int x=1;x<cell-1;++x) if(actual.pixels[size_t(y)*actual.width+x]!=expected.pixels[size_t(y)*cell+x]) throw std::runtime_error("Recording hover/pressed style differs from shared screenshot rendering");
        }
        SendMessageW(bar,WM_LBUTTONUP,0,center); // mute via the actual button
        wchar_t tipText[128]{}; TOOLINFOW info{sizeof(info)}; info.hwnd=bar; info.uId=109; info.lpszText=tipText;
        SendMessageW(tooltip,TTM_GETTEXTW,std::size(tipText),reinterpret_cast<LPARAM>(&info));
        if(std::wstring(tipText)!=L"开启系统声音") throw std::runtime_error("Native tooltip did not update after audio toggle");
        SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_MOUSELEAVE,0,0);
    }
    { RECT bounds{}; GetWindowRect(bar,&bounds); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&bounds)); }
    // The compact bar has no separate grip: its outer edge must remain draggable.
    {
        RECT original{}; GetWindowRect(bar,&original);
        POINT cursor{}; GetCursorPos(&cursor);
        SetCursorPos(original.right-20,original.top+1);
        SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(original.right-original.left-20,1));
        SetCursorPos(original.right-32,original.top-7);
        SendMessageW(bar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(original.right-original.left-32,-7));
        SendMessageW(bar,WM_LBUTTONUP,0,MAKELPARAM(original.right-original.left-20,1));
        RECT moved{}; GetWindowRect(bar,&moved);
        if(moved.left!=original.left-12 || moved.top!=original.top-8) throw std::runtime_error("Toolbar edge drag stopped working");
        SetWindowPos(bar,nullptr,original.left,original.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetCursorPos(cursor.x,cursor.y);
    }
    SendMessageW(bar,WM_RBUTTONUP,0,MAKELPARAM(MulDiv(36,dpi,96),MulDiv(23,dpi,96)));
    HWND settings=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
    if(settings) throw std::runtime_error("Audio right-click still offers device selection");
    RECT before{}; GetWindowRect(input,&before);
    SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(60,60)); SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(80,70)); SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(60,60));
    RECT moved{}; GetWindowRect(input,&moved); if(moved.left!=before.left+20 || moved.top!=before.top+10) throw std::runtime_error("Preparation selection move failed");
    // Restore the original selection before resize validation.
    SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(60,60)); SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(40,50)); SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(60,60));
    GetWindowRect(input,&before);
    const int pad=MulDiv(8,dpi,96),right=before.right-before.left-pad,bottom=before.bottom-before.top-pad;
    SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(right,bottom)); SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(right+16,bottom+10)); SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(right+16,bottom+10));
    GetWindowRect(input,&moved); if(moved.right!=before.right+16 || moved.bottom!=before.bottom+10) throw std::runtime_error("Selection resize failed");
    for(int testDpi:{96,144,192}) {
        RECT bounds{}; GetWindowRect(bar,&bounds); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(testDpi,testDpi),reinterpret_cast<LPARAM>(&bounds));
        WindowSnapshot(bar,prefix+L"-prepare-dpi"+std::to_wstring(testDpi)+L".png");
    }
    RECT bounds{}; GetWindowRect(bar,&bounds); SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&bounds));
    SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,113,0); PumpFor(1400);
    WindowSnapshot(display,prefix+L"-countdown.png");
    SendMessageW(bar,WM_COMMAND,112,0); PumpFor(600);
    if(!complete || success || session->Active()) throw std::runtime_error("Countdown cancellation failed"); session.reset();
    std::cout<<"UI cancelled countdown"<<std::endl;
    complete=false; const auto files=PendingVideos(); RecordingResult delivered;
    session=OpenRecordingSession(region,[&](bool ok){complete=true;success=ok;},[&](RecordingResult result){delivered=std::move(result);}); bar=session->Window();
    SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,113,0); PumpFor(4200);
    std::cout<<"UI recording started"<<std::endl;
    if(!session->Active()) throw std::runtime_error("Recorder did not start");
    input=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏标注");
    if(IsWindowVisible(input)) throw std::runtime_error("Desktop interaction mode intercepted input");
    IndependentPointerControls(bar,region,prefix);
    const auto checkHistoryButtons=[&](bool enabled) {
        SendMessageW(bar,WM_MOUSELEAVE,0,0);
        const auto actual=WindowSnapshot(bar,prefix+L"-history-buttons.png");
        const UINT buttonDpi=GetDpiForWindow(bar); const int cell=MulDiv(ToolbarButtonDip,int(buttonDpi),96);
        for(int id:{107,108}) {
            Image expected(cell,cell); std::fill(expected.pixels.begin(),expected.pixels.end(),0xfff8f9fa);
            HBITMAP bitmap=ToBitmap(expected); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
            DrawToolbarIcon(dc,id==107?ToolbarIcon::Undo:ToolbarIcon::Clear,{0,0,cell,cell},buttonDpi,enabled?ToolbarInk:ToolbarDisabled);
            SelectObject(dc,old); DeleteDC(dc); expected=FromBitmap(bitmap); DeleteObject(bitmap);
            for(int y=1;y<cell-1;++y) for(int x=1;x<cell-1;++x)
                if(actual.pixels[size_t(y)*actual.width+(id-102)*cell+x]!=expected.pixels[size_t(y)*cell+x])
                    throw std::runtime_error("Undo/clear appearance does not reflect annotation content");
        }
    };
    checkHistoryButtons(false);
    SendMessageW(bar,WM_COMMAND,106,0); checkHistoryButtons(false); SendMessageW(bar,WM_COMMAND,106,0);
    SendMessageW(bar,WM_COMMAND,105,0); SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,70));
    HWND emptyText=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏文字");
    for(int id:{107,108}) {
        const int cell=MulDiv(ToolbarButtonDip,int(GetDpiForWindow(bar)),96);
        const LPARAM point=MAKELPARAM((id-102)*cell+cell/2,cell/2);
        SendMessageW(bar,WM_LBUTTONDOWN,MK_LBUTTON,point); checkHistoryButtons(false);
        SendMessageW(bar,WM_LBUTTONUP,0,point); SendMessageW(bar,WM_COMMAND,id,0);
        if(!IsWindow(emptyText)) throw std::runtime_error("Disabled history command closed empty text input");
    }
    SendMessageW(GetDlgItem(emptyText,1),EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"测试")); checkHistoryButtons(true);
    SendMessageW(bar,WM_COMMAND,107,0); checkHistoryButtons(false);
    SendMessageW(bar,WM_COMMAND,102,0);
    for(int command:{107,108}) {
        SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,140));
        SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(300,140)); checkHistoryButtons(false);
        SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,140));
        SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(345,175));
        SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(345,175)); checkHistoryButtons(true);
        SendMessageW(bar,WM_COMMAND,command,0); checkHistoryButtons(false);
    }
    SendMessageW(bar,WM_COMMAND,102,0);
    SendMessageW(bar,WM_COMMAND,102,0);
    settings=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
    if(!settings || !IsWindowVisible(settings)) throw std::runtime_error("Inline annotation styles missing");
    auto styleLayout=MakeStylePanelLayout({},GetDpiForWindow(bar),false,true);
    const auto widthRect=styleLayout.widths[1];
    const LPARAM widthPoint=MAKELPARAM((widthRect.left+widthRect.right)/2,(widthRect.top+widthRect.bottom)/2);
    SendMessageW(settings,WM_LBUTTONDOWN,MK_LBUTTON,widthPoint); SendMessageW(settings,WM_LBUTTONUP,0,widthPoint);
    WindowSnapshot(settings,prefix+L"-styles.png");    // Move the controls into the capture region; DXGI duplication must still exclude them.
    SetWindowPos(bar,HWND_TOPMOST,region.left,region.top+180,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
    for(int id=101;id<=104;++id) {
        SendMessageW(bar,WM_COMMAND,id,0); if(!IsWindowVisible(input)) throw std::runtime_error("Annotation input was not enabled");
        int x=300+(id-101)*65;
        SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,140)); SendMessageW(input,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+45,175)); SendMessageW(input,WM_LBUTTONUP,0,MAKELPARAM(x+45,175));
    }
    SendMessageW(bar,WM_COMMAND,101,0); SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,300));
    SendMessageW(input,WM_KEYDOWN,VK_ESCAPE,0); if(GetCapture()==input || IsWindowVisible(input)) throw std::runtime_error("Esc during stroke retained mouse capture");
    SendMessageW(bar,WM_COMMAND,105,0); SendMessageW(input,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(300,70));
    HWND text=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏文字"); if(!text) throw std::runtime_error("Text editor not shown"); SendMessageW(GetDlgItem(text,1),EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"录屏 ABC")); PumpFor(250);
    SendMessageW(input,WM_KEYDOWN,VK_ESCAPE,0); if(IsWindowVisible(input)) throw std::runtime_error("Esc did not restore desktop input");
    WindowSnapshot(bar,prefix+L"-recording.png");
    display=FindWindowW(L"PcTool.RecordingSurface",L"PcTool · 录屏边框");
    HWND smooth=FindWindowExW(display,nullptr,L"PcTool.RecordingSurface",L"PcTool · 录屏平滑标注"); DWORD affinity{};
    if(!smooth || !GetWindowDisplayAffinity(GetAncestor(smooth,GA_ROOT),&affinity) || affinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Smooth annotation window not excluded");
    // Temporarily include only the test-owned annotation surface for desktop
    // compositor verification; restore exclusion before recording assertions.
    winrt::check_bool(SetWindowDisplayAffinity(display,WDA_NONE)); PumpFor(100);
    HDC desktop=GetDC(nullptr),memory=CreateCompatibleDC(desktop);
    HBITMAP liveBitmap=CreateCompatibleBitmap(desktop,641,361); auto previousBitmap=SelectObject(memory,liveBitmap);
    BitBlt(memory,0,0,641,361,desktop,region.left,region.top,SRCCOPY|CAPTUREBLT);
    SelectObject(memory,previousBitmap); DeleteDC(memory); ReleaseDC(nullptr,desktop);
    auto live=FromBitmap(liveBitmap); DeleteObject(liveBitmap); SavePng(live,prefix+L"-live-desktop.png");
    winrt::check_bool(SetWindowDisplayAffinity(display,WDA_EXCLUDEFROMCAPTURE));
    PumpFor(700);
    std::cout<<"UI annotations drawn"<<std::endl;
    POINT previousCursor{}; GetCursorPos(&previousCursor);
    SendMessageW(bar,WM_COMMAND,106,0);
    if(IsWindowVisible(input)) throw std::runtime_error("Laser intercepted desktop input");
    HWND pointerLayer=FindWindowExW(display,nullptr,L"PcTool.RecordingSurface",L"PcTool · 录屏激光光点");
    if(!pointerLayer) throw std::runtime_error("Dedicated laser sprite missing");
    HWND laserPanel=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
    if(!laserPanel || !IsWindowVisible(laserPanel)) throw std::runtime_error("Laser style panel missing");
    const auto laserLayout=MakeStylePanelLayout({},dpi,false,true);
    ClickStyle(laserPanel,laserLayout.widths[2]); ClickStyle(laserPanel,laserLayout.colors[5]);
    SetCursorPos(region.left+480,region.top+260); PumpFor(80);
    RECT largeDot{}; GetWindowRect(pointerLayer,&largeDot);
    if(largeDot.right-largeDot.left!=4*MulDiv(14,dpi,96)+5) throw std::runtime_error("Laser size selection did not update sprite");
    WindowSnapshot(laserPanel,prefix+L"-laser-style.png");
    ClickStyle(laserPanel,laserLayout.widths[0]); ClickStyle(laserPanel,laserLayout.colors[0]);
    // Keep the settings panel away from the synthetic pointer path.
    SetWindowPos(laserPanel,nullptr,region.left,region.bottom+100,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    const int laserExtent=2*MulDiv(4,dpi,96)+2;
    if(!GetWindowDisplayAffinity(GetAncestor(pointerLayer,GA_ROOT),&affinity) || affinity!=WDA_EXCLUDEFROMCAPTURE) throw std::runtime_error("Laser sprite is not capture-excluded");
    RECT staticInk{}; GetWindowRect(smooth,&staticInk);
    KillTimer(bar,2); // Prove mouse events, rather than the fallback timer, drive motion.
    for(int step=0;step<16;++step) {
        const POINT position{region.left+390+step*6,region.top+260};
        INPUT movement{}; movement.type=INPUT_MOUSE;
        movement.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
        movement.mi.dx=MulDiv(position.x-GetSystemMetrics(SM_XVIRTUALSCREEN),65535,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1);
        movement.mi.dy=MulDiv(position.y-GetSystemMetrics(SM_YVIRTUALSCREEN),65535,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1);
        winrt::check_bool(SendInput(1,&movement,sizeof(INPUT))==1); PumpFor(5);
        RECT dot{},ink{}; POINT actual{}; GetCursorPos(&actual); GetWindowRect(pointerLayer,&dot); GetWindowRect(smooth,&ink);
        if(!IsWindowVisible(pointerLayer) || dot.right-dot.left!=2*laserExtent+1 || dot.bottom-dot.top!=2*laserExtent+1 || std::abs(dot.left+laserExtent-actual.x)>1 || std::abs(dot.top+laserExtent-actual.y)>1)
        { std::cout<<"laser visible="<<IsWindowVisible(pointerLayer)<<" size="<<dot.right-dot.left<<" extent="<<laserExtent<<" position="<<dot.left<<","<<dot.top<<" cursor="<<actual.x<<","<<actual.y<<std::endl; throw std::runtime_error("Laser did not follow native mouse events without timer polling"); }
        if(!EqualRect(&staticInk,&ink)) throw std::runtime_error("Laser movement changed the static annotation surface");
    }
    SetTimer(bar,2,16,nullptr);
    RECT controlBounds{}; GetWindowRect(bar,&controlBounds);
    const auto expectHidden=[&](POINT target,const char* message) {
        const auto deadline=GetTickCount64()+150;
        do { SetCursorPos(target.x,target.y); PumpFor(10); } while(IsWindowVisible(pointerLayer) && GetTickCount64()<deadline);
        if(IsWindowVisible(pointerLayer)) {
            POINT actual{}; GetCursorPos(&actual);
            std::cout<<"Hide target="<<target.x<<","<<target.y<<" actual="<<actual.x<<","<<actual.y<<std::endl;
            throw std::runtime_error(message);
        }
    };
    expectHidden({controlBounds.left+10,controlBounds.top+10},"Laser stayed visible over recording controls");
    expectHidden({region.right+25,region.bottom+25},"Laser stayed visible outside recording region");
    const int clicks=fixtureClicks; SetCursorPos(region.left+600,region.top+320);
    INPUT mouse[2]{}; mouse[0].type=mouse[1].type=INPUT_MOUSE;
    mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(SendInput(2,mouse,sizeof(INPUT))!=2) throw std::runtime_error("Cannot test desktop mouse input");
    PumpFor(100); std::cout<<"UI clicks="<<fixtureClicks<<std::endl; if(fixtureClicks!=clicks+1) throw std::runtime_error("Annotation display blocked desktop mouse clicks");
    CheckCrossProcessClick({region.left+600,region.top+320});
    SetCursorPos(region.left+400,region.top+260); PumpFor(100);
    SetCursorPos(region.left+480,region.top+260); PumpFor(200);
    SendMessageW(bar,WM_COMMAND,111,0); PumpFor(400); SendMessageW(bar,WM_COMMAND,111,0); PumpFor(1500);
    SendMessageW(bar,WM_COMMAND,106,0);
    if(IsWindowVisible(pointerLayer)) throw std::runtime_error("Laser sprite survived tool deselection");
    SetCursorPos(previousCursor.x,previousCursor.y);
    SendMessageW(bar,WM_COMMAND,107,0); PumpFor(150); SendMessageW(bar,WM_COMMAND,108,0); PumpFor(500);
    checkHistoryButtons(false);
    std::cout<<"UI finish"<<std::endl;
    SendMessageW(bar,WM_COMMAND,113,0); PumpFor(1200);
    if(!complete || !success || session->Active() || session->Window() || delivered.path.empty()) throw std::runtime_error("End did not close controls and deliver a preview result");
    bool ownDialog=false;
    EnumWindows([](HWND w,LPARAM data)->BOOL{
        DWORD pid{};GetWindowThreadProcessId(w,&pid);wchar_t cls[64]{};GetClassNameW(w,cls,64);
        if(pid==GetCurrentProcessId()&&IsWindowVisible(w)&&wcscmp(cls,L"#32770")==0){*reinterpret_cast<bool*>(data)=true;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&ownDialog));
    if(ownDialog) throw std::runtime_error("End still opened a save dialog");
    std::filesystem::path video;
    for(const auto& file:PendingVideos()) if(!files.count(file) && file.wstring().find(L"partial")==std::wstring::npos) video=file;
    if(video.empty() || video!=delivered.path) throw std::runtime_error("End lost the preview video");
    winrt::check_hresult(MFStartup(MF_VERSION));
    const auto videoEnd=StreamEnd(video.wstring(),DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
    auto annotated=VideoPreview(video.wstring(),videoEnd-12000000); SavePng(annotated,prefix+L"-annotated.png");
    auto red=[](uint32_t pixel){return ((pixel>>16)&255)>170 && ((pixel>>8)&255)<140;};
    int redCount=0; for(int y=130;y<180;++y) for(int x=290;x<600;++x) if(red(annotated.pixels[size_t(y)*annotated.width+x])) ++redCount;
    if(redCount<100) throw std::runtime_error("Recorded annotation tools missing");
    // The bar overlaps the red fixture rectangle at this location.
    auto pixel=annotated.pixels[size_t(205)*annotated.width+100]; if(!red(pixel)) throw std::runtime_error("Toolbar leaked into recording");
    int textRed=0; for(int y=70;y<110;++y) for(int x=300;x<470;++x) if(red(annotated.pixels[size_t(y)*annotated.width+x])) ++textRed;
    if(textRed<50) throw std::runtime_error("Recording text preview/commit missing");
    // The final stationary laser is held for 1.5 s, followed by 650 ms of
    // deselection/clear checks. Sample relative to the end so earlier UI
    // assertions and machine speed do not move this frame before laser mode.
    auto laserFrame=VideoPreview(video.wstring(),videoEnd-12000000); SavePng(laserFrame,prefix+L"-laser.png");
    int laserRed=0; for(int y=254;y<268;++y) for(int x=474;x<486;++x) { auto px=laserFrame.pixels[size_t(y)*laserFrame.width+x]; if(int((px>>16)&255)-int((px>>8)&255)>20) ++laserRed; }
    if(laserRed<30) throw std::runtime_error("Recorded laser missing");
    int trail=0; for(int y=252;y<269;++y) for(int x=390;x<465;++x) if(red(laserFrame.pixels[size_t(y)*laserFrame.width+x])) ++trail;
    if(trail) throw std::runtime_error("Laser left a trail at previous cursor positions");
    auto clear=VideoPreview(video.wstring(),StreamEnd(video.wstring(),DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM))-2000000);
    if(std::abs(StreamEnd(video.wstring(),DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM))-StreamEnd(video.wstring(),DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM)))>2000000) throw std::runtime_error("UI recording A/V drift");
    int remaining=0; for(int y=130;y<180;++y) for(int x=290;x<600;++x) if(red(clear.pixels[size_t(y)*clear.width+x])) ++remaining;
    if(remaining>10) throw std::runtime_error("Clear left annotations in later frames");
    SavePng(clear,prefix+L"-clear.png"); MFShutdown();
    std::filesystem::copy_file(video,prefix+L".mp4",std::filesystem::copy_options::overwrite_existing);
    session.reset();
    if(!std::filesystem::exists(video)) throw std::runtime_error("Closing result deleted unsaved video");
    std::filesystem::remove(video); // only this test's newly created recording
    const auto beforeShutdown=PendingVideos();
    session=OpenRecordingSession(region,[](bool){}); bar=session->Window();
    SendMessageW(bar,WM_COMMAND,109,0); SendMessageW(bar,WM_COMMAND,113,0); PumpFor(4200);
    session.reset(); // application shutdown finalizes without presenting a save dialog
    std::filesystem::path shutdownVideo; for(const auto& file:PendingVideos()) if(!beforeShutdown.count(file) && file.wstring().find(L"partial")==std::wstring::npos) shutdownVideo=file;
    if(shutdownVideo.empty()) throw std::runtime_error("Shutdown did not retain recording");
    winrt::check_hresult(MFStartup(MF_VERSION)); if(StreamEnd(shutdownVideo.wstring(),DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM))<5000000) throw std::runtime_error("Shutdown recording unplayable"); MFShutdown(); std::filesystem::remove(shutdownVideo);
    std::cout<<"RECORDING UI PASS: move, cancel countdown, draw, text, event-driven cached laser, pass-through, pause, exclusion, preview result and recovery\n";
}
static int PasteVideoFixture(const std::wstring& destination) {
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"PcTool.VideoPasteFixture";
    wc.lpfnWndProc=[](HWND window,UINT m,WPARAM w,LPARAM l)->LRESULT {
        if(m==WM_KEYDOWN && w=='V' && GetKeyState(VK_CONTROL)<0) return SendMessageW(window,WM_PASTE,0,0);
        if(m==WM_PASTE) {
            bool ok=false;
            if(OpenClipboard(window)) {
                const auto drop=static_cast<HDROP>(GetClipboardData(CF_HDROP)); wchar_t path[32768]{};
                if(drop && DragQueryFileW(drop,0,path,32768)) {
                    const auto* target=reinterpret_cast<const std::wstring*>(GetWindowLongPtrW(window,GWLP_USERDATA));
                    ok=CopyFileW(path,target->c_str(),FALSE)!=FALSE;
                }
                CloseClipboard();
            }
            PostQuitMessage(ok?0:1); return 0;
        }
        return DefWindowProcW(window,m,w,l);
    };
    RegisterClassW(&wc);
    HWND window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"视频文件粘贴测试",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,360,180,nullptr,nullptr,wc.hInstance,nullptr);
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&destination)); SetForegroundWindow(window); SetFocus(window);
    MSG message{}; while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    DestroyWindow(window); return int(message.wParam);
}
static void PreviewChildProcess(const std::wstring& mode,const std::wstring& path,bool paste=false) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    std::wstring command=L"\""+std::wstring(executable)+L"\" "+mode+L" \""+path+L"\"";
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process{}; winrt::check_bool(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process));
    struct Cleanup { PROCESS_INFORMATION p; ~Cleanup(){if(WaitForSingleObject(p.hProcess,0)!=WAIT_OBJECT_0) TerminateProcess(p.hProcess,1);CloseHandle(p.hProcess);CloseHandle(p.hThread);} } cleanup{process};
    const auto deadline=GetTickCount64()+5000;
    if(paste) {
        HWND target{}; while(!target && GetTickCount64()<deadline) { target=FindWindowW(L"PcTool.VideoPasteFixture",nullptr); PumpFor(10); }
        if(!target) throw std::runtime_error("Paste fixture did not start");
        ShowWindow(target,SW_SHOW); SetForegroundWindow(target); PumpFor(80);
        INPUT keys[4]{}; for(auto& key:keys) key.type=INPUT_KEYBOARD;
        keys[0].ki.wVk=VK_CONTROL; keys[1].ki.wVk='V'; keys[2].ki.wVk='V'; keys[2].ki.dwFlags=KEYEVENTF_KEYUP;
        keys[3].ki.wVk=VK_CONTROL; keys[3].ki.dwFlags=KEYEVENTF_KEYUP;
        winrt::check_bool(SendInput(4,keys,sizeof(INPUT))==4);
    }
    while(WaitForSingleObject(process.hProcess,0)!=WAIT_OBJECT_0 && GetTickCount64()<deadline) PumpFor(20);
    DWORD code{}; GetExitCodeProcess(process.hProcess,&code); if(code!=0) throw std::runtime_error("Preview child process failed or timed out ("+winrt::to_string(mode)+"): "+std::to_string(code));
}
static void PreviewTest(RECT region,const std::wstring& prefix) {
    // Restore transferable clipboard data, including images, after exercising paste.
    struct ClipboardRestore {
        std::vector<std::pair<UINT,std::vector<BYTE>>> values;
        ClipboardRestore() {
            if(!OpenClipboard(nullptr)) return;
            for(UINT format=EnumClipboardFormats(0);format;format=EnumClipboardFormats(format)) {
                if(format==CF_BITMAP || format==CF_PALETTE || format==CF_ENHMETAFILE || format==CF_METAFILEPICT) continue;
                HANDLE memory=GetClipboardData(format); const SIZE_T size=memory?GlobalSize(memory):0;
                if(size) { auto* bytes=static_cast<const BYTE*>(GlobalLock(memory)); if(bytes) { values.push_back({format,{bytes,bytes+size}}); GlobalUnlock(memory); } }
            }
            CloseClipboard();
        }
        ~ClipboardRestore() {
            if(!OpenClipboard(nullptr)) return; EmptyClipboard();
            for(const auto& value:values) { HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,value.second.size()); if(!memory) continue;
                void* bytes=GlobalLock(memory); if(!bytes) {GlobalFree(memory);continue;} memcpy(bytes,value.second.data(),value.second.size()); GlobalUnlock(memory);
                if(!SetClipboardData(value.first,memory)) GlobalFree(memory);
            }
            CloseClipboard();
        }
    } clipboardRestore;
    const std::wstring source=prefix+L"-source.mp4",current=prefix+L"-current.mp4",saved=prefix+L"-saved.mp4";
    PumpFor(350); // Let the fixture's desktop fade-in finish before the first captured frame.
    RecordingOptions options; options.region=region; options.systemAudio=true; options.path=source;
    auto sourceTone=std::make_unique<TestTone>();
    RecordingState recording; std::thread recorder([&]{RecordScreen(options,recording);});
    struct RecordingCleanup { RecordingState& state; std::thread& worker; ~RecordingCleanup(){state.stop=true;if(worker.joinable()) worker.join();} } recordingCleanup{recording,recorder};
    PumpFor(1100);
    HWND changingFixture=FindWindowW(L"PcTool.CaptureFixture",nullptr);
    SetPropW(changingFixture,L"PreviewLateFrame",reinterpret_cast<HANDLE>(1)); InvalidateRect(changingFixture,nullptr,FALSE);
    PumpFor(1100); recording.stop=true; recorder.join(); sourceTone.reset();
    RemovePropW(changingFixture,L"PreviewLateFrame"); InvalidateRect(changingFixture,nullptr,FALSE);
    if(!recording.error.empty()) throw std::runtime_error(winrt::to_string(recording.error));
    HWND fixture=FindWindowW(L"PcTool.CaptureFixture",nullptr); SetWindowPos(fixture,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    const auto fresh=[&] { std::filesystem::copy_file(source,current,std::filesystem::copy_options::overwrite_existing); return OpenRecordingPreview({current,{}}); };
    PreviewChildProcess(L"preview-visible-fixture",source);
    const auto ready=[&](RecordingPreview& preview) {
        const auto deadline=GetTickCount64()+5000;
        while(!preview.State().ready && preview.State().error.empty() && GetTickCount64()<deadline) PumpFor(20);
        if(!preview.State().error.empty()) throw std::runtime_error(winrt::to_string(preview.State().error));
        if(!preview.State().ready || preview.State().duration<10000000) throw std::runtime_error("Preview did not load duration and first frame");
        PumpFor(150);
    };
    auto preview=fresh(); ready(*preview); HWND window=preview->Window();
    if(preview->State().playing || preview->State().muted) throw std::runtime_error("Preview must start paused with sound enabled");
    for(int dpi:{96,144,192}) {
        RECT rect{}; GetWindowRect(window,&rect); SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&rect));
        RECT videoBefore{}; GetWindowRect(GetDlgItem(window,2110),&videoBefore);
        SetCursorPos(rect.left+30,rect.top+60); PumpFor(150);
        if(!preview->State().controlsVisible) throw std::runtime_error("Preview hover failed to show controls");
        auto pixels=DesktopUiSnapshot(rect,prefix+L"-dpi"+std::to_wstring(dpi)+L".png");
        int red=0; for(auto pixel:pixels.pixels) if(((pixel>>16)&255)>170 && ((pixel>>8)&255)<130) ++red;
        if(red<1000) throw std::runtime_error("Preview first frame is blank or not rendered");
        RECT button{}; GetWindowRect(GetDlgItem(window,PreviewComplete),&button);
        if(button.top<videoBefore.bottom) throw std::runtime_error("Preview controls overlap video");
        // Exercise real hit testing and capture, and verify decoded frames while
        // the button is still held (a UI-only thumb update must not pass).
        HWND track=GetDlgItem(window,PreviewSeek); RECT trackRect{}; GetWindowRect(track,&trackRect);
        const auto move=[&](double fraction) {
            const int padding=MulDiv(8,dpi,96);
            SetCursorPos(trackRect.left+padding+int(fraction*(trackRect.right-trackRect.left-2*padding)),(trackRect.top+trackRect.bottom)/2);
            PumpFor(25);
        };
        const auto mouse=[&](DWORD flags) { INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=flags; winrt::check_bool(SendInput(1,&input,sizeof(input))==1); PumpFor(25); };
        move(0.15);
        POINT hit{}; GetCursorPos(&hit);
        if(WindowFromPoint(hit)!=track) throw std::runtime_error("Progress track does not receive native mouse input");
        mouse(MOUSEEVENTF_LEFTDOWN);
        if(GetCapture()!=track) throw std::runtime_error("Progress drag did not capture mouse");
        for(double fraction:{0.3,0.65,0.25,0.8}) move(fraction);
        PumpFor(300);
        auto dragged=DesktopUiSnapshot(videoBefore,prefix+L"-drag-dpi"+std::to_wstring(dpi)+L".png");
        int blue=0; for(auto pixel:dragged.pixels) if((pixel&255)>170 && ((pixel>>16)&255)<130) ++blue;
        if(blue<1000 || preview->State().playing || std::abs(preview->State().position-preview->State().duration*8/10)>1500000)
            throw std::runtime_error("Dragging did not locate and render the requested frame before release");
        // Release outside the track; capture must still commit the last target.
        SetCursorPos(trackRect.left+(trackRect.right-trackRect.left)*8/10,trackRect.top-20); PumpFor(25);
        mouse(MOUSEEVENTF_LEFTUP); PumpFor(200);
        if(GetCapture()==track || preview->State().playing) throw std::runtime_error("Progress drag release failed");
        SendMessageW(track,WM_KEYDOWN,VK_HOME,0); PumpFor(250);
        SetCursorPos(1,1); PumpFor(150);
        RECT videoAfter{}; GetWindowRect(GetDlgItem(window,2110),&videoAfter);
        if(preview->State().controlsVisible || !EqualRect(&videoBefore,&videoAfter)) throw std::runtime_error("Hiding preview controls changed video geometry");
    }
    SendMessageW(window,WM_COMMAND,PreviewPlay,0); PumpFor(450);
    if(!preview->State().playing || preview->State().position<1000000) throw std::runtime_error("Preview did not play");
    SendMessageW(window,WM_COMMAND,PreviewPlay,0); PumpFor(150);
    const auto paused=preview->State().position; PumpFor(200);
    if(preview->State().playing || std::abs(preview->State().position-paused)>300000) throw std::runtime_error("Paused preview clock advanced");
    SendMessageW(window,WM_COMMAND,PreviewMute,0); if(!preview->State().muted) throw std::runtime_error("Preview mute failed");
    SendMessageW(window,WM_COMMAND,PreviewMute,0); if(preview->State().muted) throw std::runtime_error("Preview unmute failed");
    HWND seek=GetDlgItem(window,PreviewSeek); RECT seekRect{}; GetClientRect(seek,&seekRect);
    SendMessageW(seek,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(seekRect.right/2,seekRect.bottom/2));
    SendMessageW(seek,WM_LBUTTONUP,0,MAKELPARAM(seekRect.right/2,seekRect.bottom/2)); PumpFor(250);
    if(std::abs(preview->State().position-preview->State().duration/2)>1500000 || preview->State().playing) throw std::runtime_error("Paused preview seek failed");
    SendMessageW(window,WM_COMMAND,PreviewPlay,0); PumpFor(150);
    SendMessageW(seek,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(seekRect.right/3,seekRect.bottom/2));
    SendMessageW(seek,WM_LBUTTONUP,0,MAKELPARAM(seekRect.right/3,seekRect.bottom/2)); PumpFor(250);
    if(!preview->State().playing || !preview->State().error.empty()) throw std::runtime_error("Playing seek did not resume cleanly");
    SendMessageW(seek,WM_KEYDOWN,VK_END,0); PumpFor(450);
    if(preview->State().playing) throw std::runtime_error("Preview did not stop at end");
    SendMessageW(window,WM_COMMAND,PreviewPlay,0); PumpFor(250);
    if(!preview->State().playing || preview->State().position>10000000) throw std::runtime_error("Preview did not replay from start");
    const auto endDeadline=GetTickCount64()+ULONGLONG(preview->State().duration/10000)+1000;
    while(preview->State().playing && GetTickCount64()<endDeadline) PumpFor(20);
    if(preview->State().playing || std::abs(preview->State().position-preview->State().duration)>100000) throw std::runtime_error("Natural playback completion did not hold at end");
    // Capture actual MFPlay output through WASAPI, not only its mute flag.
    SendMessageW(seek,WM_KEYDOWN,VK_HOME,0); PumpFor(150);
    RecordingOptions playbackOptions; playbackOptions.region=region; playbackOptions.systemAudio=true;
    playbackOptions.waitForStart=true; playbackOptions.path=prefix+L"-playback-audio.mp4";
    RecordingState playback; std::thread playbackRecorder([&]{RecordScreen(playbackOptions,playback);});
    RecordingCleanup playbackCleanup{playback,playbackRecorder};
    const auto audioDeadline=GetTickCount64()+4000; while(!playback.ready && !playback.finished && GetTickCount64()<audioDeadline) PumpFor(10);
    if(!playback.ready) throw std::runtime_error("Preview audio loopback failed to initialize");
    playback.start=true; SendMessageW(window,WM_COMMAND,PreviewPlay,0); PumpFor(600);
    SendMessageW(window,WM_COMMAND,PreviewMute,0); PumpFor(600);
    SendMessageW(window,WM_COMMAND,PreviewMute,0); PumpFor(600);
    playback.stop=true; playbackRecorder.join();
    if(preview->State().playing) SendMessageW(window,WM_COMMAND,PreviewPlay,0); PumpFor(100);
    winrt::check_hresult(MFStartup(MF_VERSION));
    const auto sound=AudioRms(playbackOptions.path,2500000,4500000),quiet=AudioRms(playbackOptions.path,8500000,10500000),restored=AudioRms(playbackOptions.path,14500000,16500000);
    MFShutdown(); std::cout<<"PREVIEW AUDIO rms="<<sound<<" muted="<<quiet<<" restored="<<restored<<std::endl;
    if(sound<0.00005 || restored<0.00005 || quiet>std::min(sound,restored)*0.1) throw std::runtime_error("Preview mute did not affect actual playback audio");
    const auto timer=SetTimer(nullptr,0,100,CancelSaveDialog); SendMessageW(window,WM_COMMAND,PreviewSave,0); KillTimer(nullptr,timer);
    if(!preview->Window() || !std::filesystem::exists(current)) throw std::runtime_error("Cancel save closed/deleted preview");
    CopyText(window,L"preview clipboard sentinel"); const auto clipboardBefore=GetClipboardSequenceNumber();
    preview->SaveTo(prefix+L"-missing/fail.mp4");
    while(preview->State().busy) PumpFor(20);
    if(!preview->Window() || preview->State().error.empty() || !std::filesystem::exists(current)) throw std::runtime_error("Failed save lost source or error");
    preview->SaveTo(saved); SendMessageW(window,WM_CLOSE,0,0); // busy close must be ignored
    const auto saveDeadline=GetTickCount64()+4000; while(preview->Window() && GetTickCount64()<saveDeadline) PumpFor(20);
    if(preview->Window() || !std::filesystem::exists(saved) || std::filesystem::exists(current) || GetClipboardSequenceNumber()!=clipboardBefore) throw std::runtime_error("Save did not close, clean source and preserve clipboard");
    preview.reset(); preview=fresh(); ready(*preview); window=preview->Window();
    {
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
        std::wstring command=L"\""+std::wstring(executable)+L"\" clipboard-lock-fixture";
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{}; winrt::check_bool(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process));
        struct Cleanup { PROCESS_INFORMATION p; ~Cleanup(){if(WaitForSingleObject(p.hProcess,0)!=WAIT_OBJECT_0) TerminateProcess(p.hProcess,1);CloseHandle(p.hProcess);CloseHandle(p.hThread);} } cleanup{process};
        HWND locker{}; const auto deadline=GetTickCount64()+3000;
        while(GetTickCount64()<deadline) { locker=FindWindowW(L"STATIC",L"PcTool clipboard lock fixture"); if(locker && GetPropW(locker,L"Ready")) break; PumpFor(10); }
        if(!locker || !GetPropW(locker,L"Ready")) throw std::runtime_error("Clipboard lock fixture unavailable");
        SendMessageW(window,WM_COMMAND,PreviewComplete,0);
        if(!preview->Window() || preview->State().error.empty() || !std::filesystem::exists(current)) throw std::runtime_error("Clipboard failure lost preview or video");
        PostMessageW(locker,WM_CLOSE,0,0);
        while(WaitForSingleObject(process.hProcess,0)!=WAIT_OBJECT_0 && GetTickCount64()<deadline) PumpFor(10);
    }
    SendMessageW(window,WM_COMMAND,PreviewComplete,0);
    if(preview->Window() || !std::filesystem::exists(current)) throw std::runtime_error("Complete lost clipboard video");
    preview.reset(); winrt::check_bool(OpenClipboard(nullptr));
    HDROP drop=static_cast<HDROP>(GetClipboardData(CF_HDROP)); wchar_t copied[32768]{};
    const bool fileCopied=drop && DragQueryFileW(drop,0,copied,32768)>0; CloseClipboard();
    if(!fileCopied || current!=copied) throw std::runtime_error("Complete did not publish real CF_HDROP file");
    // Publish in a separate process, let it exit, then paste using real Ctrl+V
    // in another native process. This catches delayed-rendering/lifetime bugs.
    PreviewChildProcess(L"preview-complete-fixture",current);
    PreviewChildProcess(L"video-paste-fixture",prefix+L"-pasted.mp4",true);
    if(std::filesystem::file_size(prefix+L"-pasted.mp4")!=std::filesystem::file_size(source)) throw std::runtime_error("Cross-process paste changed video contents");
    CopyText(nullptr,L"preview clipboard unchanged on cancel"); const auto beforeCancel=GetClipboardSequenceNumber();
    preview=fresh(); ready(*preview); window=preview->Window();
    HANDLE lock=CreateFileW(current.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    winrt::check_bool(lock!=INVALID_HANDLE_VALUE); SendMessageW(window,WM_CLOSE,0,0);
    if(!preview->Window() || preview->State().error.empty()) { CloseHandle(lock); throw std::runtime_error("Discard failure silently closed preview"); }
    CloseHandle(lock); SendMessageW(window,WM_CLOSE,0,0);
    if(preview->Window() || std::filesystem::exists(current) || beforeCancel!=GetClipboardSequenceNumber()) throw std::runtime_error("Discard did not delete only this video");
    preview.reset(); preview=fresh(); ready(*preview); preview.reset();
    if(!std::filesystem::exists(current)) throw std::runtime_error("App shutdown deleted unsaved preview");
    preview=OpenRecordingPreview({prefix+L"-missing.mp4",{}}); PumpFor(400);
    if(preview->State().error.empty()) throw std::runtime_error("Missing preview file has no error"); preview.reset();
    for(int i=0;i<5;++i) { preview=fresh(); preview.reset(); PumpFor(30); }
    std::filesystem::remove(current);
    std::cout<<"PREVIEW PASS: playback, seek, mute, DPI, hover, complete clipboard, save/discard/shutdown and late callbacks\n";
}
static void OcrUiTest(const std::wstring& prefix) {
    struct ClipboardRestore {
        std::vector<std::pair<UINT,std::vector<BYTE>>> values;
        ClipboardRestore() {
            if(!OpenClipboard(nullptr)) return;
            for(UINT format=EnumClipboardFormats(0);format;format=EnumClipboardFormats(format)) {
                if(format==CF_BITMAP || format==CF_PALETTE || format==CF_ENHMETAFILE || format==CF_METAFILEPICT) continue;
                HANDLE memory=GetClipboardData(format); const SIZE_T size=memory?GlobalSize(memory):0;
                if(size) { auto* bytes=static_cast<const BYTE*>(GlobalLock(memory)); if(bytes) { values.push_back({format,{bytes,bytes+size}}); GlobalUnlock(memory); } }
            }
            CloseClipboard();
        }
        ~ClipboardRestore() {
            if(!OpenClipboard(nullptr)) return; EmptyClipboard();
            for(const auto& value:values) { HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,value.second.size()); if(!memory) continue;
                void* bytes=GlobalLock(memory); if(!bytes) {GlobalFree(memory);continue;} memcpy(bytes,value.second.data(),value.second.size()); GlobalUnlock(memory);
                if(!SetClipboardData(value.first,memory)) GlobalFree(memory);
            }
            CloseClipboard();
        }
    } clipboardRestore;
    Image image(800,600); std::fill(image.pixels.begin(),image.pixels.end(),0xffffffff);
    HBITMAP bitmap=ToBitmap(image); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
    HFONT font=CreateFontW(-32,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei"); auto oldFont=SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(0,0,0));
    const wchar_t* texts[]={L"第一段 Hello 123",L"第二段 World 456",L"第三段 Text 789"};
    for(int i=0;i<3;++i) TextOutW(dc,30,40+i*180,texts[i],int(wcslen(texts[i])));
    SelectObject(dc,oldFont); SelectObject(dc,old); DeleteObject(font); DeleteDC(dc); image=FromBitmap(bitmap); DeleteObject(bitmap);
    HWND owner=CreateWindowExW(0,L"STATIC",L"OCR input owner",WS_OVERLAPPEDWINDOW,50,50,700,500,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    struct OwnerCleanup { HWND window; ~OwnerCleanup(){DestroyWindow(window);} } ownerCleanup{owner};
    ShowWindow(owner,SW_SHOW);
    OcrRequest cancelled; cancelled.image=image; cancelled.owner=owner; GetWindowRect(owner,&cancelled.busyRegion);
    auto busy=OpenOcr(std::move(cancelled));
    if(IsWindowEnabled(owner)) throw std::runtime_error("OCR did not lock source while recognizing");
    RECT rect{}; GetWindowRect(busy->Window(),&rect); DesktopUiSnapshot(rect,prefix+L"-busy.png");
    SendMessageW(busy->Window(),WM_KEYDOWN,VK_ESCAPE,0);
    if(busy->Window() || !IsWindowEnabled(owner)) throw std::runtime_error("OCR cancel did not restore source"); busy.reset();
    CopyText(owner,L"OCR clipboard sentinel"); const auto sequence=GetClipboardSequenceNumber();
    OcrRequest request; request.image=image; request.owner=owner; GetWindowRect(owner,&request.busyRegion);
    auto result=OpenOcr(std::move(request));
    const auto deadline=GetTickCount64()+8000;
    while(result->Window() && !GetDlgItem(result->Window(),402) && GetTickCount64()<deadline) PumpFor(20);
    HWND window=result->Window(),text=GetDlgItem(window,402),picture=GetDlgItem(window,401);
    if(!text || !picture || !IsWindowEnabled(owner) || GetClipboardSequenceNumber()!=sequence) throw std::runtime_error("OCR result did not open or modified clipboard");
    PumpFor(200);
    auto recognized=WindowText(text); recognized.erase(std::remove(recognized.begin(),recognized.end(),L' '),recognized.end()); if(recognized.find(L"123")==std::wstring::npos || recognized.find(L"789")==std::wstring::npos) throw std::runtime_error("OCR result text missing");
    SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    for(UINT dpi:{96u,144u,192u}) {
        GetWindowRect(window,&rect); SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&rect)); PumpFor(100);
        DesktopUiSnapshot(rect,prefix+L"-dpi"+std::to_wstring(dpi)+L".png");
        RECT imageClient{}; GetClientRect(picture,&imageClient);
        const double fit=std::min(double(imageClient.right)/800,double(imageClient.bottom)/600);
        const int hitX=int(std::max(0.0,(imageClient.right-800*fit)/2)+100*fit),hitY=int(std::max(0.0,(imageClient.bottom-600*fit)/2)+235*fit);
        SendMessageW(picture,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(hitX,hitY)); SendMessageW(picture,WM_LBUTTONUP,0,MAKELPARAM(hitX,hitY));
        if(GetPropW(window,L"OcrSelected")!=reinterpret_cast<HANDLE>(2)) throw std::runtime_error("Original image click did not select corresponding paragraph");
        SendMessageW(text,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,10)); SendMessageW(text,WM_LBUTTONUP,0,MAKELPARAM(10,10)); PumpFor(60);
        if(GetPropW(window,L"OcrSelected")!=reinterpret_cast<HANDLE>(1)) throw std::runtime_error("OCR result click did not select first paragraph");
        // RichEdit exposes CR-based character positions; locate the second paragraph there.
        GETTEXTEX get{0,GT_DEFAULT,1200,nullptr,nullptr}; std::vector<wchar_t> raw(size_t(GetWindowTextLengthW(text))+8); get.cb=DWORD(raw.size()*sizeof(wchar_t)); SendMessageW(text,EM_GETTEXTEX,reinterpret_cast<WPARAM>(&get),reinterpret_cast<LPARAM>(raw.data()));
        std::wstring rich(raw.data()); const auto separator=rich.find(L"\r\r");
        POINTL secondPoint{}; SendMessageW(text,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&secondPoint),separator+2);
        POINTL gapPoint{}; SendMessageW(text,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&gapPoint),separator+1);
        if(std::abs(int(secondPoint.y-gapPoint.y)-MulDiv(6,dpi,96))>1) throw std::runtime_error("OCR paragraph gap is not 6 DIP");
        SendMessageW(text,WM_MOUSEMOVE,0,MAKELPARAM(10,gapPoint.y+1));
        if(GetPropW(window,L"OcrHovered")) throw std::runtime_error("OCR paragraph gap hits adjacent text");
        POINT hoverPoint{10,secondPoint.y+2}; ClientToScreen(text,&hoverPoint); SetCursorPos(hoverPoint.x,hoverPoint.y); PumpFor(60);
        if(GetPropW(window,L"OcrHovered")!=reinterpret_cast<HANDLE>(2)) throw std::runtime_error("OCR hover did not map second paragraph");
        RECT textBounds{}; GetClientRect(text,&textBounds); SendMessageW(text,WM_MOUSEMOVE,0,MAKELPARAM(10,textBounds.bottom-5));
        if(GetPropW(window,L"OcrHovered")) throw std::runtime_error("OCR blank space incorrectly highlights a paragraph");
        if(GetPropW(window,L"OcrSelected")!=reinterpret_cast<HANDLE>(1)) throw std::runtime_error("OCR hover cleared persistent selection");
        GetWindowRect(text,&rect); SetCursorPos(rect.left+15,rect.top+15);
        INPUT mouse[2]{}; for(auto& m:mouse)m.type=INPUT_MOUSE; mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,mouse,sizeof(INPUT));PumpFor(50);
        const auto chord=[](WORD key) { INPUT keys[4]{};for(auto& k:keys)k.type=INPUT_KEYBOARD; keys[0].ki.wVk=keys[3].ki.wVk=VK_CONTROL;keys[1].ki.wVk=keys[2].ki.wVk=key;keys[2].ki.dwFlags=keys[3].ki.dwFlags=KEYEVENTF_KEYUP;SendInput(4,keys,sizeof(INPUT));PumpFor(100); };
        SetCursorPos(rect.left+2,rect.top+5); SendInput(1,&mouse[0],sizeof(INPUT)); PumpFor(25);
        SetCursorPos(rect.left+70,rect.top+secondPoint.y+5); PumpFor(50); SendInput(1,&mouse[1],sizeof(INPUT)); PumpFor(25);
        CHARRANGE dragged{}; SendMessageW(text,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&dragged));
        if(dragged.cpMin>=LONG(separator) || dragged.cpMax<=LONG(separator+2)) throw std::runtime_error("OCR mouse selection did not span paragraphs");
        chord('A'); CHARRANGE range{};SendMessageW(text,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
        if(range.cpMin!=0 || range.cpMax<30) throw std::runtime_error("OCR Ctrl+A did not span paragraphs");
        chord('C'); if(!OpenClipboard(window)) throw std::runtime_error("OCR copied text inaccessible");
        HANDLE h=GetClipboardData(CF_UNICODETEXT); const wchar_t* chars=h?static_cast<const wchar_t*>(GlobalLock(h)):nullptr; std::wstring copied=chars?chars:L"";if(chars)GlobalUnlock(h);CloseClipboard();
        copied.erase(std::remove(copied.begin(),copied.end(),L' '),copied.end());
        if(copied.find(L"123")==std::wstring::npos || copied.find(L"789")==std::wstring::npos || copied.find(L"1\r\n")!=std::wstring::npos) throw std::runtime_error("OCR copy lost text or included number gutter");
        // Mouse-wheel zoom and background drag must leave linked controls responsive.
        GetWindowRect(picture,&rect); POINT point{rect.left+100,rect.top+100};
        SendMessageW(picture,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),MAKELPARAM(point.x,point.y));
        SendMessageW(picture,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,10));SendMessageW(picture,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(20,20));SendMessageW(picture,WM_LBUTTONUP,0,MAKELPARAM(20,20));
        SendMessageW(picture,WM_LBUTTONDBLCLK,0,MAKELPARAM(5,5)); PumpFor(30);
    }
    SendMessageW(window,WM_CLOSE,0,0); result.reset();
    Image tall(800,20400);
    for(int n=0;n<34;++n) std::copy(image.pixels.begin(),image.pixels.end(),tall.pixels.begin()+size_t(n)*800*600);
    auto longResult=OpenOcr(std::move(tall)); const auto longDeadline=GetTickCount64()+45000;
    while(longResult->Window() && !GetDlgItem(longResult->Window(),402) && GetTickCount64()<longDeadline) PumpFor(20);
    HWND longText=GetDlgItem(longResult->Window(),402); if(!longText) throw std::runtime_error("Long OCR result missing");
    SendMessageW(longText,WM_VSCROLL,SB_BOTTOM,0); PumpFor(50); POINT scroll{};SendMessageW(longText,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    if(scroll.y<=0) throw std::runtime_error("Long OCR result did not scroll");
    const auto longContent=WindowText(longText);
    if(std::count(longContent.begin(),longContent.end(),L'\n')<198) throw std::runtime_error("OCR triple-digit paragraph fixture incomplete");
    RECT longBounds{};GetWindowRect(longResult->Window(),&longBounds);DesktopUiSnapshot(longBounds,prefix+L"-long-numbers.png");
    SendMessageW(longResult->Window(),WM_CLOSE,0,0); longResult.reset();
    // The no-text path must report the state and restore its source, not leave
    // an empty result window. Dismiss only this test thread's native notice.
    const DWORD uiThread=GetCurrentThreadId();
    auto dismissNotice=std::async(std::launch::async,[uiThread] {
        const auto deadline=GetTickCount64()+8000;
        bool dismissed=false;
        while(GetTickCount64()<deadline) {
            bool present=false;
            EnumThreadWindows(uiThread,[](HWND w,LPARAM data)->BOOL {
                wchar_t cls[32]{};GetClassNameW(w,cls,32);
                if(wcscmp(cls,L"#32770")==0) {
                    PostMessageW(w,WM_CLOSE,0,0); *reinterpret_cast<bool*>(data)=true;
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(&present));
            if(dismissed && !present) break;
            dismissed=dismissed || present;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return dismissed;
    });
    OcrRequest blank; blank.image=Image(400,200);std::fill(blank.image.pixels.begin(),blank.image.pixels.end(),0xffffffff);blank.owner=owner;
    auto empty=OpenOcr(std::move(blank));const auto emptyDeadline=GetTickCount64()+4000;
    while(empty->Window() && GetTickCount64()<emptyDeadline) PumpFor(20);
    if(!dismissNotice.get()) throw std::runtime_error("Empty OCR notice not displayed");
    if(empty->Window() || !IsWindowEnabled(owner)) throw std::runtime_error("Empty OCR did not restore source");empty.reset();
    // Closing an input window invalidates the asynchronous result rather than
    // delivering it into a recycled HWND.
    OcrRequest stale; stale.image=std::move(image); stale.owner=owner; auto late=OpenOcr(std::move(stale));
    DestroyWindow(owner);ownerCleanup.window=nullptr;PumpFor(150);
    if(late->Window()) throw std::runtime_error("OCR retained a result after its source was destroyed");
    std::cout<<"OCR UI PASS: progress, cancel, source lifetime, numbered result, DPI, selection/copy and zoom\n";
}
static void OcrSourceTest(RECT region,const std::wstring& prefix) {
    CaptureTools tools; ScreenshotOverlay source(GetModuleHandleW(nullptr)); source.SetCaptureTools(&tools);
    PumpFor(300); if(!source.Start()) throw std::runtime_error("Screenshot source did not start");
    HWND overlay=FindWindowW(L"PcTool.ScreenshotOverlay",nullptr);
    SetWindowPos(FindWindowW(L"PcTool.CaptureFixture",nullptr),HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    POINT a{region.left,region.top},b{region.right,region.bottom}; ScreenToClient(overlay,&a);ScreenToClient(overlay,&b);
    SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));
    const int dpi=int(GetDpiForWindow(overlay)),width=MulDiv(36,dpi,96)*14+MulDiv(68,dpi,96);
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&region,MONITOR_DEFAULTTONEAREST),&mi);
    POINT button{std::clamp<int>(region.right-width,mi.rcMonitor.left,mi.rcMonitor.right-width)+MulDiv(342,dpi,96),region.bottom+MulDiv(23,dpi,96)};ScreenToClient(overlay,&button);
    const auto start=[&]{SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(button.x,button.y));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(button.x,button.y));};
    start(); HWND busy=FindWindowW(L"PcTool.CaptureToolWindow",L"正在识别 · PcTool");
    if(!busy || IsWindowEnabled(overlay)) throw std::runtime_error("Screenshot OCR did not show locked progress");
    // Freeze result delivery while capturing this state: a warmed OCR engine
    // can finish before the screenshot helper's DWM settling delay elapses.
    KillTimer(busy,1);
    RECT screenshot{};GetWindowRect(overlay,&screenshot);DesktopUiSnapshot(screenshot,prefix+L"-source-busy.png");
    SendMessageW(busy,WM_KEYDOWN,VK_ESCAPE,0);
    if(!IsWindow(overlay) || !IsWindowEnabled(overlay)) throw std::runtime_error("Screenshot OCR cancellation lost editing state");
    start();
    const auto deadline=GetTickCount64()+8000; HWND result{};
    while(GetTickCount64()<deadline) {result=FindWindowW(L"PcTool.CaptureToolWindow",L"文字识别 · PcTool");if(result)break;PumpFor(20);}
    if(!result || FindWindowW(L"PcTool.ScreenshotOverlay",nullptr)) throw std::runtime_error("Completed OCR did not close screenshot source");
    GetWindowRect(result,&screenshot);DesktopUiSnapshot(screenshot,prefix+L"-source-result.png");
    SendMessageW(result,WM_CLOSE,0,0);tools.Shutdown();
    std::cout<<"OCR SOURCE PASS: selected desktop progress, cancellation restores tools, success closes screenshot\n";
}
static void RecordingCursorTest(RECT region,const std::wstring& prefix) {
    POINT previous{}; GetCursorPos(&previous);
    struct RestoreCursor { POINT p; ~RestoreCursor(){SetCursorPos(p.x,p.y);} } restore{previous};
    SetCursorPos(region.left+450,region.top+310); SetCursor(LoadCursorW(nullptr,IDC_ARROW)); PumpFor(60);
    RecordingOptions options; options.region=region; options.path=prefix+L".mp4"; options.systemAudio=false;
    RecordingState state; std::thread worker([&]{RecordScreen(options,state);});
    struct StopWorker { RecordingState& s; std::thread& t; ~StopWorker(){s.RequestStop(RecordingStopReason::Save); if(t.joinable()) t.join();} } cleanup{state,worker};
    const auto deadline=GetTickCount64()+5000;
    while(!state.ready && !state.finished && GetTickCount64()<deadline) PumpFor(10);
    if(!state.ready || state.finished) throw std::runtime_error("Cursor recorder did not initialize");
    const auto holdPointer=[&] {
        const auto until=GetTickCount64()+650;
        while(GetTickCount64()<until) { SetCursorPos(region.left+450,region.top+310); PumpFor(10); }
        CURSORINFO cursor{sizeof(cursor)}; GetCursorInfo(&cursor);
        std::cout<<"Cursor visible="<<cursor.flags<<" position="<<cursor.ptScreenPos.x-region.left<<","<<cursor.ptScreenPos.y-region.top<<std::endl;
    };
    const int64_t nativeTime=state.elapsed+3000000; holdPointer();
    { std::lock_guard<std::mutex> lock(state.mutex); state.laserMode=true; state.liveLaser={{450,310},true}; }
    const int64_t laserTime=state.elapsed+3000000; holdPointer();
    state.cursorEnabled=false;const int64_t hiddenLaserTime=state.elapsed+3000000;holdPointer();
    { std::lock_guard<std::mutex> lock(state.mutex); state.laserMode=false; state.liveLaser={}; }
    const int64_t hiddenNativeTime=state.elapsed+3000000;holdPointer();
    state.cursorEnabled=true;const int64_t restoredTime=state.elapsed+3000000; holdPointer();
    state.RequestStop(RecordingStopReason::Save); worker.join();
    if(!state.error.empty() || state.savedPath.empty()) throw std::runtime_error("Cursor recording failed: "+winrt::to_string(state.error));
    winrt::check_hresult(MFStartup(MF_VERSION));
    auto native=VideoPreview(state.savedPath,nativeTime),laser=VideoPreview(state.savedPath,laserTime),restored=VideoPreview(state.savedPath,restoredTime),hiddenLaser=VideoPreview(state.savedPath,hiddenLaserTime),hiddenNative=VideoPreview(state.savedPath,hiddenNativeTime); MFShutdown();
    SavePng(native,prefix+L"-native.png"); SavePng(laser,prefix+L"-laser.png"); SavePng(restored,prefix+L"-restored.png");
    const auto dark=[](const Image& image) {
        int count=0; for(int y=302;y<348;++y) for(int x=442;x<492;++x) {
            const auto p=image.pixels[size_t(y)*image.width+x]; if(((p>>16)&255)<100 && ((p>>8)&255)<100 && (p&255)<100) ++count;
        } return count;
    };
    if(dark(native)<15 || dark(restored)<15) throw std::runtime_error("Native cursor missing before/after laser mode");
    if(dark(laser)>5) throw std::runtime_error("Native cursor was encoded together with the laser");
    int red=0; for(int y=305;y<316;++y) for(int x=445;x<456;++x) { auto p=laser.pixels[size_t(y)*laser.width+x]; if(int((p>>16)&255)-int((p>>8)&255)>40) ++red; }
    if(red<30) throw std::runtime_error("Laser replacement missing from video");
    if(dark(hiddenLaser)>5||dark(hiddenNative)>5)throw std::runtime_error("Hidden cursor was encoded");
    for(int y=305;y<316;++y)for(int x=445;x<456;++x) {auto p=hiddenLaser.pixels[size_t(y)*hiddenLaser.width+x];if(int((p>>16)&255)-int((p>>8)&255)>40)throw std::runtime_error("Hidden laser was encoded");}
    SavePng(hiddenLaser,prefix+L"-hidden-laser.png");SavePng(hiddenNative,prefix+L"-hidden-native.png");
    std::cout<<"RECORD CURSOR PASS: native cursor, laser replacement without duplicate arrow, native cursor restored\n";
}
static void ElectronScrollTest(HWND source,const std::wstring& prefix){
    if(WindowText(source)!=L"PcTool Electron scroll fixture")throw std::runtime_error("Not the dedicated Electron fixture");
    struct Stats{int offset{},wheels{},moves{},clicks{};};
    auto stats=[&]{Stats s;const auto path=prefix+L"-stats.txt";HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Missing Electron fixture telemetry");char text[160]{};DWORD size{};const bool read=ReadFile(file,text,sizeof(text)-1,&size,nullptr)!=FALSE;CloseHandle(file);
        if(!read||sscanf_s(text,"%d %d %d %d",&s.offset,&s.wheels,&s.moves,&s.clicks)!=4)throw std::runtime_error("Invalid Electron fixture telemetry");return s;};
    SetWindowPos(source,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetForegroundWindow(source);PumpFor(300);RECT region{};GetClientRect(source,&region);POINT origin{};ClientToScreen(source,&origin);OffsetRect(&region,origin.x,origin.y);
    auto doc=std::make_shared<ImageDocument>();bool completed=false;
    auto session=OpenLongCaptureSession(region,doc,[&](bool ok){completed=ok;},[](auto){});PumpFor(650);
    const auto before=stats();SetCursorPos(origin.x+500,origin.y+400);PumpFor(100);
    INPUT wheel{};wheel.type=INPUT_MOUSE;wheel.mi.dwFlags=MOUSEEVENTF_WHEEL;wheel.mi.mouseData=120;SendInput(1,&wheel,sizeof(wheel));
    const auto end=GetTickCount64()+5000;while(GetTickCount64()<end){const auto p=session->Progress();if(p.actionId&&p.completedActionId>=p.actionId&&!p.inFlight)break;PumpFor(20);}
    PumpFor(200);const auto after=stats();const auto progress=session->Progress();
    std::cout<<"Electron before="<<before.offset<<" after="<<after.offset<<" wheels="<<after.wheels-before.wheels<<" moves="<<after.moves-before.moves<<" clicks="<<after.clicks-before.clicks<<" height="<<progress.height<<" action="<<progress.actionId<<" done="<<progress.completedActionId<<" status="<<winrt::to_string(progress.status)<<std::endl;
    if(after.offset>=before.offset||after.wheels==before.wheels){SendMessageW(session->Window(),WM_CLOSE,0,0);PumpFor(300);throw std::runtime_error("Electron did not receive the controlled upward wheel");}
    if(after.moves!=before.moves||after.clicks!=before.clicks)throw std::runtime_error("Source hover/click protection was broken by wheel delivery");
    if(!progress.analysis.locked||progress.analysis.currentY!=after.offset-before.offset||progress.height!=region.bottom-region.top+before.offset-after.offset)throw std::runtime_error("Electron scroll did not produce correct prepend geometry");
    auto gesture=[&](int delta){
        const auto id=session->Progress().actionId;wheel.mi.mouseData=DWORD(delta);winrt::check_bool(SendInput(1,&wheel,sizeof(wheel))==1);
        const auto deadline=GetTickCount64()+5000;while(GetTickCount64()<deadline){const auto p=session->Progress();if(p.actionId>id&&p.completedActionId>=p.actionId&&!p.inFlight)break;PumpFor(20);}
        PumpFor(200);const auto p=session->Progress();const auto s=stats();
        std::cout<<"wheel delta="<<delta<<" id="<<p.actionId<<" done="<<p.completedActionId<<" offset="<<s.offset<<" current="<<p.analysis.currentY<<" height="<<p.height<<" result="<<int(p.analysis.result)<<" status="<<winrt::to_string(p.status)<<std::endl;
        if(p.actionId<=id||p.inFlight||p.completedActionId<p.actionId||p.analysis.currentY!=s.offset-before.offset)throw std::runtime_error("Electron repeated wheel lost actual source position");
        if(s.moves!=before.moves||s.clicks!=before.clicks)throw std::runtime_error("Electron hover/click escaped during repeated wheel delivery");
    };
    for(int i=0;i<12;++i)gesture(i%2?-120:120);
    const int revisitHeight=session->Progress().height;
    for(int i=0;i<24&&!session->Progress().atTop;++i)gesture(120);
    if(!session->Progress().atTop||stats().offset!=0)throw std::runtime_error("Electron upward scrolling did not reach and identify the top");
    gesture(120);if(!session->Progress().atTop)throw std::runtime_error("Electron repeated top hint missing");
    // Once locked, clicks elsewhere in the selection control the same DOM list.
    SetCursorPos(origin.x+180,origin.y+310);PumpFor(100);INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,click,sizeof(INPUT));
    const auto autoEnd=GetTickCount64()+35000;while(!session->Progress().atEnd&&GetTickCount64()<autoEnd)PumpFor(20);
    const auto bottom=stats();POINT pointer{};GetCursorPos(&pointer);
    if(!session->Progress().atEnd||bottom.offset!=1640||session->Progress().height!=region.bottom-region.top+1640||session->Progress().height<=revisitHeight)throw std::runtime_error("Electron automatic lower-anchor restore or downward append failed");
    if(pointer.x!=origin.x+180||pointer.y!=origin.y+310||bottom.moves!=before.moves||bottom.clicks!=before.clicks)throw std::runtime_error("Electron auto capture moved the cursor or exposed hover/clicks");
    gesture(-120);if(!session->Progress().atEnd)throw std::runtime_error("Electron repeated bottom hint missing");
    SendMessageW(session->Window(),WM_COMMAND,11,0);const auto finish=GetTickCount64()+5000;while(!completed&&GetTickCount64()<finish)PumpFor(20);
    if(!completed)throw std::runtime_error("Electron long capture edit handoff failed");SavePng(doc->image,prefix+L"-stitched.png");
    Image expected(450,2000);std::ifstream pixels(std::filesystem::path(prefix+L"-expected.bgra"),std::ios::binary);pixels.read(reinterpret_cast<char*>(expected.pixels.data()),std::streamsize(expected.pixels.size()*4));
    if(pixels.gcount()!=std::streamsize(expected.pixels.size()*4))throw std::runtime_error("Expected Electron canvas pixels missing");
    for(int y=0;y<2000;++y)for(int x=0;x<450;++x)if((doc->image.pixels[size_t(y+240)*doc->image.width+x+300]&0xffffff)!=(expected.pixels[size_t(y)*450+x]&0xffffff)){
        std::cerr<<"Electron pixel mismatch "<<x<<","<<y<<std::endl;throw std::runtime_error("Electron stitched text has missing/repeated rows or columns");}
    std::cout<<"ELECTRON SCROLL PASS: actual DOM scroll, 12 alternating wheels, upward prepend, repeated top/bottom, locked-list automatic restore/append, no cursor motion or source hover/click, edit handoff"<<std::endl;
}
int wmain(int argc,wchar_t** argv) {
#ifdef PCTOOL_OCR_EVALUATION
    wchar_t evalModel[32768]{},evalTier[64]{};
    if(GetEnvironmentVariableW(L"PCTOOL_OCR_EVAL_MODEL",evalModel,32768)) {
        GetEnvironmentVariableW(L"PCTOOL_OCR_EVAL_TIER",evalTier,64);
        OcrEvaluationOptions options;options.modelDirectory=evalModel;options.v6=std::wstring(evalTier)!=L"v5";options.modelName=options.v6?L"PP-OCRv6 "+std::wstring(evalTier):L"PP-OCRv5 mobile";options.boxThreshold=std::wstring(evalTier)==L"tiny"?.4F:.45F;ConfigureOcrEvaluation(options);
    }
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool previewApartment=argc>1 && (std::wstring(argv[1])==L"preview-test" || std::wstring(argv[1])==L"preview-complete-fixture" || std::wstring(argv[1])==L"preview-visible-fixture");
    winrt::init_apartment(previewApartment?winrt::apartment_type::single_threaded:winrt::apartment_type::multi_threaded);
    if(argc>3&&std::wstring(argv[1])==L"electron-scroll-test"){
        try{ElectronScrollTest(reinterpret_cast<HWND>(_wcstoui64(argv[2],nullptr,10)),argv[3]);return 0;}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
    }
    if(argc>1 && std::wstring(argv[1])==L"clipboard-lock-fixture") {
        HWND window=CreateWindowExW(0,L"STATIC",L"PcTool clipboard lock fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!OpenClipboard(window)) { DestroyWindow(window); return 1; } SetPropW(window,L"Ready",reinterpret_cast<HANDLE>(1));
        const auto deadline=GetTickCount64()+5000; while(IsWindow(window) && GetTickCount64()<deadline) PumpFor(10);
        CloseClipboard(); if(IsWindow(window)) DestroyWindow(window); return 0;
    }
    if(argc>2 && std::wstring(argv[1])==L"video-paste-fixture") return PasteVideoFixture(argv[2]);
    if(argc>2 && std::wstring(argv[1])==L"preview-visible-fixture") {
        auto preview=OpenRecordingPreview({argv[2],{}}); PumpFor(250);
        return IsWindowVisible(preview->Window())?0:2;
    }
    if(argc>2 && std::wstring(argv[1])==L"gif-preview-visible-fixture") {
        auto preview=OpenRecordingPreview({argv[2],{},RecordingFormat::Gif}); PumpFor(250);
        if(!IsWindowVisible(preview->Window()))return 2;
        for(int id:{PreviewPlay,PreviewSave,PreviewComplete})if(!IsWindowVisible(GetDlgItem(preview->Window(),id)))return 3;
        return 0;
    }
    if(argc>2 && std::wstring(argv[1])==L"gif-preview-hidden-start-test") {
        try { PreviewChildProcess(L"gif-preview-visible-fixture",argv[2]); return 0; }
        catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    }
    if(argc>2 && std::wstring(argv[1])==L"preview-hidden-start-test") {
        try { PreviewChildProcess(L"preview-visible-fixture",argv[2]); return 0; }
        catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    }
    if(argc>2 && std::wstring(argv[1])==L"preview-complete-fixture") {
        auto preview=OpenRecordingPreview({argv[2],{}});
        SendMessageW(preview->Window(),WM_COMMAND,PreviewComplete,0);
        if(preview->Window())std::cerr<<"PREVIEW COMPLETE CHILD: "<<winrt::to_string(preview->State().error)<<std::endl;
        return preview->Window()?1:0;
    }
    try {
        if(argc>2 && std::wstring(argv[1])==L"ocr-compare") {CompareOcr(argv[2]);return 0;}
        if(argc>3 && std::wstring(argv[1])==L"click-fixture") return ClickFixtureProcess(_wtoi(argv[2]),_wtoi(argv[3]));
        if(argc>1 && std::wstring(argv[1])==L"window-snap-test") { RunWindowSnapTests(); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"pin-interaction-test") { RunScreenshotInteractionTests(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"ocr-ui-test") { OcrUiTest(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"style-test") { StylePanelTest(argv[2]); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"prepare-test") { RecordingPrepareTest(); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"annotation-test") { AnnotationAlphaTest(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"scroll-annotation-editor-test") { ScrollAnnotationEditorTest(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"icon-test") { IconTest(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"discard-files-test") { DiscardFilesTest(argv[2]); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"live-clipboard-hotkey-test") { LiveClipboardHotkeyTest(); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"live-utilities-test") { LiveUtilitiesTest(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"live-menu-test") { LiveMenuTest(argv[2]); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"taskbar-snapshot") {
            HWND monitor=nullptr; HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
            EnumChildWindows(taskbar,[](HWND w,LPARAM data)->BOOL { wchar_t cls[100]{}; GetClassNameW(w,cls,100); if(wcscmp(cls,L"PcTool.TaskbarMonitor")==0) { *reinterpret_cast<HWND*>(data)=w; return FALSE; } return TRUE; },reinterpret_cast<LPARAM>(&monitor));
            if(!monitor) throw std::runtime_error("Live taskbar monitor not found");
            RECT bounds{},tray{}; GetWindowRect(monitor,&bounds); GetWindowRect(FindWindowExW(taskbar,nullptr,L"TrayNotifyWnd",nullptr),&tray);
            if(bounds.right>tray.left) throw std::runtime_error("Live monitor overlaps notification area");
            DesktopCapture capture(bounds,false); Image latest; const auto deadline=GetTickCount64()+1800;
            while(GetTickCount64()<deadline) { Image fresh; if(capture.Next(fresh)) latest=std::move(fresh); PumpFor(20); }
            if(latest.Empty()) throw std::runtime_error("Live taskbar capture returned no frame"); SavePng(latest,argv[2]);
            std::cout<<"LIVE TASKBAR PASS bounds="<<bounds.left<<","<<bounds.right<<" tray="<<tray.left<<std::endl; return 0;
        }
        if(argc>2 && std::wstring(argv[1])==L"preview") {
            winrt::check_hresult(MFStartup(MF_VERSION)); auto image=VideoPreview(argv[2]);
            SavePng(image,std::wstring(argv[2])+L".preview.png"); MFShutdown(); return 0;
        }

        if(argc>2&&(std::wstring(argv[1])==L"browser-scroll-frames"||std::wstring(argv[1])==L"browser-bidirectional-frames")){
            winrt::com_ptr<IWICImagingFactory> factory;winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.put())));
            auto load=[&](int n){auto path=std::filesystem::path(argv[2])/(L"frame-"+std::to_wstring(n)+L".png");winrt::com_ptr<IWICBitmapDecoder> decoder;winrt::check_hresult(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,decoder.put()));winrt::com_ptr<IWICBitmapFrameDecode> frame;winrt::check_hresult(decoder->GetFrame(0,frame.put()));winrt::com_ptr<IWICFormatConverter> converter;winrt::check_hresult(factory->CreateFormatConverter(converter.put()));winrt::check_hresult(converter->Initialize(frame.get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));UINT w{},h{};frame->GetSize(&w,&h);Image image(w,h);winrt::check_hresult(converter->CopyPixels(nullptr,w*4,UINT(image.pixels.size()*4),reinterpret_cast<BYTE*>(image.pixels.data())));return image;};
            ScrollStitcher stitch;auto first=load(0);stitch.Push(first);Image last;
            if(std::wstring(argv[1])==L"browser-bidirectional-frames"){
                // Real browser renders, captured with the fixture buttons in
                // order 144,72,0,72,144,216; frame6 is a separately expanded
                // 454px body. This catches discontinuities across both seams.
                const int offsets[]{144,72,0,72,144,216};int minimum=144,maximum=144;
                for(int i=1;i<6;++i){last=load(i);const auto result=stitch.Push(last,POINT{500,450});const auto a=stitch.Analysis();
                    const auto expected=offsets[i]<minimum?StitchResult::Prepended:offsets[i]>maximum?StitchResult::Appended:StitchResult::Revisited;
                    minimum=std::min(minimum,offsets[i]);maximum=std::max(maximum,offsets[i]);
                    std::cout<<"browser bidirectional frame="<<i<<" result="<<int(result)<<" shift="<<a.displacement<<" current="<<a.currentY<<" range="<<a.minY<<","<<a.maxY<<" height="<<stitch.Height()<<std::endl;
                    if(result!=expected||a.currentY!=offsets[i]-144||a.minY!=minimum-144||a.maxY!=maximum-144+a.viewportHeight||stitch.Height()!=first.height+maximum-minimum)throw std::runtime_error("Browser bidirectional interval/revisit failed");
                }
                const auto a=stitch.Analysis();if(a.region.top!=341||a.region.bottom!=579||a.region.left<340||a.region.right>705)throw std::runtime_error("Browser bidirectional viewport contains fixed border");
                const auto output=stitch.Flatten(),reference=load(6);if(output.width!=first.width||output.height!=first.height+216)throw std::runtime_error("Browser bidirectional output extent failed");
                for(int y=0;y<454;y+=8){double error=0;int samples=0;for(int yy=y;yy<std::min(y+8,454);++yy)for(int x=347;x<690;++x){const auto p=output.pixels[size_t(341+yy)*output.width+x],q=reference.pixels[size_t(91+yy)*reference.width+x];for(int channel:{0,8,16}){error+=std::abs(int((p>>channel)&255)-int((q>>channel)&255));++samples;}}if(error/samples>8)throw std::runtime_error("Browser bidirectional rows differ from independently expanded reference");}
                for(int y=0;y<a.region.top;++y)for(int x=0;x<first.width;++x)if(output.pixels[size_t(y)*output.width+x]!=first.pixels[size_t(y)*first.width+x])throw std::runtime_error("Browser bidirectional top fixed content changed");
                SavePng(output,(std::filesystem::path(argv[2])/L"stitched.png").wstring());std::cout<<"BROWSER BIDIRECTIONAL PASS: real text, prepend/append/revisit, independently expanded content reference, unchanged full width\n";return 0;
            }
            for(int i=1;i<=2;++i){last=load(i);auto result=stitch.Push(last,POINT{500,450});auto a=stitch.Analysis();std::cout<<"browser frame="<<i<<" result="<<int(result)<<" shift="<<a.displacement<<" region="<<a.region.left<<","<<a.region.top<<","<<a.region.right<<","<<a.region.bottom<<" confidence="<<a.confidence<<std::endl;if(result!=StitchResult::Appended||a.displacement!=72)throw std::runtime_error("Browser text scroll matching failed");}
            auto out=stitch.Flatten();if(out.width!=first.width||out.height!=first.height+144)throw std::runtime_error("Browser output extent failed");
            const auto r=stitch.Analysis().region;
            // Independently known viewport boundaries in the HTML fixture. The
            // browser tool returns a resampled JPEG, so do not accept its 8px
            // compression fringes as fixed header/footer content to append.
            if(r.top!=341||r.bottom!=579||r.left<340||r.right>705)throw std::runtime_error("Browser viewport includes fixed borders");
            for(int y=0;y<r.bottom;++y)for(int x=0;x<out.width;++x)if(out.pixels[size_t(y)*out.width+x]!=first.pixels[size_t(y)*first.width+x])throw std::runtime_error("Browser static original pixels changed");
            auto one=load(1);for(int i=0;i<2;++i){const auto& frame=i?last:one;for(int y=0;y<72;++y)for(int x=r.left;x<r.right;++x)if(out.pixels[size_t(r.bottom+i*72+y)*out.width+x]!=frame.pixels[size_t(r.bottom-72+y)*frame.width+x])throw std::runtime_error("Browser new rows corrupted");}
            // A separate UI action expands the original list to show all eight
            // records. Compare against that render, not our own crop geometry.
            const auto reference=load(3);
            for(int y=0;y<382;y+=8){double error=0;int samples=0;for(int yy=y;yy<std::min(y+8,382);++yy)for(int x=347;x<690;++x){auto p=out.pixels[size_t(341+yy)*out.width+x],q=reference.pixels[size_t(91+yy)*reference.width+x];for(int channel:{0,8,16}){error+=std::abs(int((p>>channel)&255)-int((q>>channel)&255));++samples;}}if(error/samples>8)throw std::runtime_error("Browser row discontinuity against expanded reference");}
            SavePng(out,(std::filesystem::path(argv[2])/L"stitched.png").wstring());std::cout<<"BROWSER FRAMES PASS: real Chromium text render, exact viewport, contiguous rows against independent expanded reference, full width\n";return 0;
        }
        if(argc>1 && std::wstring(argv[1])==L"ocr") {
            Image image(800,500); std::fill(image.pixels.begin(),image.pixels.end(),0xffffffff);
            auto bitmap=ToBitmap(image); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
            HFONT font=CreateFontW(-40,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei");
            auto oldFont=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(0,0,0));
            const std::wstring text=L"PcTool 12345 中文识别测试"; TextOutW(dc,30,30,text.c_str(),int(text.size()));
            SelectObject(dc,oldFont); SelectObject(dc,old); DeleteDC(dc); DeleteObject(font);
            image=FromBitmap(bitmap); DeleteObject(bitmap);
            const auto languages=OcrLanguages(); std::wstring language;
            for(const auto& l:languages) if(l.tag.rfind(L"zh-Hans",0)==0) language=l.tag;
            if(language.empty()) throw std::runtime_error("Chinese OCR language unavailable");
            Cancellation cancel; auto result=RecognizeImage(image,language,cancel);
            std::cout<<"OCR output: "<<winrt::to_string(result)<<"\n";
            std::wstring compact=result; compact.erase(std::remove(compact.begin(),compact.end(),L' '),compact.end());
            if(compact.find(L"12345")==std::wstring::npos || compact.find(L"中文")==std::wstring::npos) throw std::runtime_error("OCR did not recognize fixture");
            Image blank(320,200); std::fill(blank.pixels.begin(),blank.pixels.end(),0xffffffff);
            if(!RecognizeImage(blank,language,cancel).empty()) throw std::runtime_error("Blank OCR image returned text");
            Image tall(800,6000); std::fill(tall.pixels.begin(),tall.pixels.end(),0xffffffff);
            for(int offset:{0,3800,5500}) for(int y=0;y<image.height;++y)
                std::copy_n(image.pixels.data()+size_t(y)*800,800,tall.pixels.data()+size_t(offset+y)*800);
            const auto longText=RecognizeImage(tall,language,cancel);
            size_t occurrences=0,position=0;
            while((position=longText.find(L"12345",position))!=std::wstring::npos) { ++occurrences; position+=5; }
            if(occurrences!=3) throw std::runtime_error("Long OCR lost or duplicated a tile line");
            std::cout<<"OCR PASS: "<<winrt::to_string(result)<<"\n";
            if(argc>2) { auto window=OpenOcr(std::move(image)); MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0 && window->Window()) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
            winrt::uninit_apartment(); return 0;
        }
        if(argc==1 || std::wstring(argv[1])==L"editor" || std::wstring(argv[1])==L"editor-test") {
            auto doc=std::make_shared<ImageDocument>(); doc->image=Image(800,6000);
            for(int y=0;y<6000;++y) for(int x=0;x<800;++x) doc->image.pixels[size_t(y)*800+x]=(y/80)%2?0xffeeeeee:0xffffffff;
            for(int y=20;y<6000;y+=160) { Annotation a; a.tool=Tool::Text; a.start={20,y}; a.end={780,y+60}; a.size=32; a.color=RGB(0,0,0); a.text=L"长图测试 12345 · 行 "+std::to_wstring(y/160+1); doc->annotations.push_back(a); }
            auto window=OpenImageEditor(doc); MSG msg{};
            if(argc>1 && std::wstring(argv[1])==L"editor-test") {
                HWND handle=window->Window();
                SendMessageW(handle,WM_COMMAND,125,0); // actual original-size command
                SendMessageW(handle,WM_VSCROLL,SB_BOTTOM,0);
                SCROLLINFO scroll{sizeof(scroll),SIF_POS}; GetScrollInfo(handle,SB_VERT,&scroll);
                const int dpi=int(GetDpiForWindow(handle)),top=MulDiv(88,dpi,96);
                const auto count=doc->annotations.size();
                SendMessageW(handle,WM_COMMAND,101,0);
                SendMessageW(handle,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(400,top+70));
                SendMessageW(handle,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(700,top+130));
                SendMessageW(handle,WM_LBUTTONUP,0,MAKELPARAM(700,top+130));
                if(doc->annotations.size()!=count+1 || doc->annotations.back().start.y!=scroll.nPos+70)
                    throw std::runtime_error("Scrolled annotation did not retain original coordinates");
                auto output=RenderDocument(*doc);
                if(output.height!=6000 || output.width!=800) throw std::runtime_error("Export cropped the long document");
                const std::wstring prefix=argc>2?argv[2]:L"editor-test";
                if(!SavePng(output,prefix+L"-full.png")) throw std::runtime_error("Long PNG export failed");
                SendMessageW(handle,WM_COMMAND,110,0);
                if(doc->annotations.size()!=count) throw std::runtime_error("Annotation undo failed");
                SendMessageW(handle,WM_COMMAND,123,0);
                if(!(GetWindowLongPtrW(handle,GWL_EXSTYLE)&WS_EX_TOPMOST)) throw std::runtime_error("Pin not topmost");
                SendMessageW(handle,WM_LBUTTONDBLCLK,0,MAKELPARAM(100,100));
                if(GetWindowLongPtrW(handle,GWL_EXSTYLE)&WS_EX_TOPMOST) throw std::runtime_error("Pin re-edit did not restore editor");
                for(int testDpi:{96,144,192}) {
                    RECT bounds{20,20,20+MulDiv(1000,testDpi,96),20+MulDiv(760,testDpi,96)};
                    SendMessageW(handle,WM_DPICHANGED,MAKEWPARAM(testDpi,testDpi),reinterpret_cast<LPARAM>(&bounds));
                    UpdateWindow(handle); RECT r{}; GetWindowRect(handle,&r);
                    Image snapshot(r.right-r.left,r.bottom-r.top); HBITMAP bitmap=ToBitmap(snapshot);
                    HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
                    if(!PrintWindow(handle,dc,2)) throw std::runtime_error("Editor print verification failed");
                    SelectObject(dc,old); DeleteDC(dc); snapshot=FromBitmap(bitmap); DeleteObject(bitmap);
                    SavePng(snapshot,prefix+L"-dpi"+std::to_wstring(testDpi)+L".png");
                    // Resize/move repeatedly, zoom then return to original scale and
                    // edit the bottom of the document using viewport coordinates.
                    for(int cycle=0;cycle<4;++cycle) SetWindowPos(handle,nullptr,30+cycle*10,30+cycle*8,900+cycle*30,680+cycle*20,SWP_NOZORDER|SWP_NOACTIVATE);
                    POINT zoomPoint{300,300};ClientToScreen(handle,&zoomPoint);SendMessageW(handle,WM_MOUSEWHEEL,MAKEWPARAM(MK_CONTROL,WHEEL_DELTA),MAKELPARAM(zoomPoint.x,zoomPoint.y));
                    SendMessageW(handle,WM_COMMAND,125,0);SendMessageW(handle,WM_VSCROLL,SB_BOTTOM,0);
                    SendMessageW(handle,WM_HSCROLL,SB_TOP,0);
                    GetScrollInfo(handle,SB_VERT,&scroll);int blankY=(120-scroll.nPos%160+160)%160;if(blankY<20)blankY+=160;
                    const int barTop=MulDiv(88,testDpi,96),inputY=barTop+blankY;size_t previousCount=doc->annotations.size();
                    SendMessageW(handle,WM_COMMAND,101,0);
                    SendMessageW(handle,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(400,inputY));SendMessageW(handle,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(520,inputY+20));SendMessageW(handle,WM_LBUTTONUP,0,MAKELPARAM(520,inputY+20));
                    if(doc->annotations.size()!=previousCount+1) throw std::runtime_error("Resized long editor lost new annotation");
                    const auto initial=doc->annotations.back().start;SendMessageW(handle,WM_COMMAND,100,0);
                    SendMessageW(handle,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(430,inputY));SendMessageW(handle,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(450,inputY+20));SendMessageW(handle,WM_LBUTTONUP,0,MAKELPARAM(450,inputY+20));
                    if(doc->annotations.back().start.x!=initial.x+20 || doc->annotations.back().start.y!=initial.y+20) throw std::runtime_error("Resized long editor annotation drag lost image coordinates");
                    SendMessageW(handle,WM_COMMAND,110,0);SendMessageW(handle,WM_COMMAND,110,0);
                    if(doc->annotations.size()!=previousCount) throw std::runtime_error("Long editor movement/create undo failed");
                }
                SendMessageW(handle,WM_CLOSE,0,0); window.reset();
                std::cout<<"EDITOR PASS: bottom annotation, full export, undo, pin, re-edit, DPI rendering\n";
                winrt::uninit_apartment(); return 0;
            }
            while(window->Window() && GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            window.reset(); winrt::uninit_apartment(); return 0;
        }
        if(argc>1 && std::wstring(argv[1])==L"overlay") {
            ScreenshotOverlay overlay(GetModuleHandleW(nullptr)); CaptureTools tools; overlay.SetCaptureTools(&tools); overlay.Start();
            MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            tools.Shutdown(); return 0;
        }
        const bool system=argc>1 && (std::wstring(argv[1])==L"system" || std::wstring(argv[1])==L"both");
        const bool mic=argc>1 && (std::wstring(argv[1])==L"mic" || std::wstring(argv[1])==L"both");
        const int seconds=argc>2?_wtoi(argv[2]):4;
        const std::wstring path=argc>3?argv[3]:L"capture-integration.mp4";
        WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=FixtureProc; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.lpszClassName=L"PcTool.CaptureFixture"; RegisterClassW(&wc);
        HWND fixture=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"PcTool Capture Test Fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,40,40,700,460,nullptr,nullptr,wc.hInstance,nullptr);
        if(!fixture) winrt::throw_last_error();
        // Console launchers may pass STARTF_USESHOWWINDOW/SW_HIDE. Explicitly
        // show the native fixture rather than accidentally capturing the desktop.
        SetWindowPos(fixture,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetForegroundWindow(fixture);
        if(!IsWindowVisible(fixture))throw std::runtime_error("Native capture fixture hidden");
        UpdateWindow(fixture);
        POINT origin{0,0}; ClientToScreen(fixture,&origin);
        if(argc>1 && std::wstring(argv[1])==L"live-preview-test") { LivePreviewTest({origin.x,origin.y,origin.x+641,origin.y+361}); DestroyWindow(fixture); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"preview-test") { PreviewTest({origin.x,origin.y,origin.x+641,origin.y+361},argv[2]); DestroyWindow(fixture); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"record-cursor-test") { RecordingCursorTest({origin.x,origin.y,origin.x+641,origin.y+361},argv[2]); DestroyWindow(fixture); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"record-visual-test") { RecordingVisualTest(fixture,argv[2]); DestroyWindow(fixture); return 0; }
        if(argc>2 && std::wstring(argv[1])==L"ocr-source-test") { OcrSourceTest({origin.x,origin.y,origin.x+641,origin.y+361},argv[2]); DestroyWindow(fixture); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"record-exit-test") { RecordingExitTest({origin.x,origin.y,origin.x+641,origin.y+361}); DestroyWindow(fixture); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"discard-ui-test") { DiscardUiTest({origin.x,origin.y,origin.x+641,origin.y+361},argc>2?argv[2]:L"discard-ui"); DestroyWindow(fixture); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"live-audio-test") { LiveAudioTest({origin.x,origin.y,origin.x+641,origin.y+361},argc>2?argv[2]:L"live-audio.mp4"); DestroyWindow(fixture); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"gif-test") { GifTest({origin.x,origin.y,origin.x+321,origin.y+181},argc>2?argv[2]:L"gif-test"); DestroyWindow(fixture); return 0; }
        if(argc>1 && std::wstring(argv[1])==L"record-ui-test") { RecordingUiTest({origin.x,origin.y,origin.x+641,origin.y+361},argc>2?argv[2]:L"record-ui"); DestroyWindow(fixture); return 0; }


        if(argc>1&&(std::wstring(argv[1])==L"partial-scroll-test"||std::wstring(argv[1])==L"bidirectional-scroll-test"||std::wstring(argv[1])==L"bidirectional-whole-test")){
            BidirectionalScrollTest(fixture,argc>2?argv[2]:L"bidirectional-scroll",std::wstring(argv[1])==L"bidirectional-whole-test");DestroyWindow(fixture);return 0;
        }
        if(argc>1 && std::wstring(argv[1])==L"qq-long-test") {
            const std::wstring prefix=argc>2?argv[2]:L"qq-long";
            scrollFixture=true;fixtureOffset=0;scrollFixtureWheelUnits=0;
            SetWindowPos(fixture,HWND_TOPMOST,400,320,700,460,SWP_SHOWWINDOW);SetForegroundWindow(fixture);
            FocusNativeFixture(fixture);
            ClientToScreen(fixture,&(origin=POINT{}));
            RECT region{origin.x,origin.y,origin.x+641,origin.y+361};
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(fixture,MONITOR_DEFAULTTONEAREST),&mi);
            InvalidateRect(fixture,nullptr,FALSE);UpdateWindow(fixture);PumpFor(350);
            const auto mouse=[&](DWORD flag,DWORD data=0){INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=flag;input.mi.mouseData=data;winrt::check_bool(SendInput(1,&input,sizeof(input))==1);PumpFor(90);};
            const auto point=[&]{SetCursorPos(region.left+300,region.top+180);PumpFor(80);};
            bool completed=false,success=false,opened=false;auto document=std::make_shared<ImageDocument>();
            std::unique_ptr<ToolWindow> editor;
            auto session=OpenLongCaptureSession(region,document,[&](bool ok){completed=true;success=ok;},[&](auto doc){opened=true;editor=OpenImageEditor(doc,region);});
            PumpFor(1000);HWND control=session->Window();
            HWND bar=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 长截图工具栏");
            if(!IsWindowVisible(control)||!IsWindowVisible(bar)||(GetWindowLongPtrW(control,GWL_STYLE)&WS_CAPTION))throw std::runtime_error("QQ capture layout not visible/borderless");
            for(int dpi:{96,144,192}){RECT r{};GetWindowRect(control,&r);SendMessageW(control,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));PumpFor(150);DesktopUiSnapshot(mi.rcMonitor,prefix+L"-capture-"+std::to_wstring(dpi)+L".png");}
            RECT previewBounds{};GetWindowRect(control,&previewBounds);
            const auto onScreen=DesktopUiSnapshot(previewBounds,prefix+L"-thumbnail-desktop.png");const auto unshaded=WindowSnapshot(control,prefix+L"-thumbnail-render.png");
            if(FrameDifference(onScreen,unshaded)>0.6)throw std::runtime_error("Thumbnail is darkened/covered by the surround");
            RECT r{};GetWindowRect(control,&r);const UINT dpi=GetDpiForWindow(fixture);SendMessageW(control,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));
            point();mouse(MOUSEEVENTF_WHEEL,DWORD(-WHEEL_DELTA));PumpFor(650);
            if(fixtureOffset!=60)throw std::runtime_error("Manual wheel did not reach original viewport");
            int clicks=fixtureClicks;mouse(MOUSEEVENTF_LEFTDOWN);mouse(MOUSEEVENTF_LEFTUP);PumpFor(1450);
            std::cout<<"auto offset="<<fixtureOffset<<" clicks="<<fixtureClicks-clicks<<std::endl;
            if(fixtureOffset<=60||fixtureClicks!=clicks)throw std::runtime_error("Click auto scrolling failed or leaked click to source");
            mouse(MOUSEEVENTF_LEFTDOWN);mouse(MOUSEEVENTF_LEFTUP);int pausedOffset=fixtureOffset;PumpFor(900);
            if(fixtureOffset!=pausedOffset)throw std::runtime_error("Second click did not pause scrolling");
            mouse(MOUSEEVENTF_WHEEL,DWORD(-WHEEL_DELTA));PumpFor(750);
            int finalOffset=fixtureOffset;
            SendMessageW(control,WM_COMMAND,11,0);PumpFor(1000);
            if(!completed||!success||!opened||!editor||document->image.width!=641||document->image.height!=361+finalOffset)throw std::runtime_error("Long image extent/editor handoff incorrect");
            const auto& image=document->image;
            for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){int row=y>=image.height-16?y-finalOffset:y;uint32_t seed=uint32_t(row)*1664525u+uint32_t(x)*1013904223u;seed^=seed>>13;seed*=1274126177u;if((image.pixels[size_t(y)*641+x]&0xffffff)!=(seed&0xffffff))throw std::runtime_error("Stitched pixels include overlay, gap or duplicate rows");}
            SavePng(image,prefix+L"-stitched.png");
            // Extend only the test document, then create a tall editor to exercise viewport scrolling.
            SendMessageW(editor->Window(),WM_CLOSE,0,0);editor.reset();
            auto tall=std::make_shared<ImageDocument>();tall->image=Image(641,2200);tall->dpi=dpi;
            for(int y=0;y<2200;++y)for(int x=0;x<641;++x)tall->image.pixels[size_t(y)*641+x]=0xff000000|((y*19+x*13)&0xffffff);
            editor=OpenImageEditor(tall,region);HWND ew=editor->Window();PumpFor(200);HWND scroll=GetDlgItem(ew,150);
            if(!IsWindowVisible(scroll)||(GetWindowLongPtrW(ew,GWL_STYLE)&(WS_CAPTION|WS_VSCROLL)))throw std::runtime_error("Editor must have only custom scrollbar");
            SendMessageW(scroll,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),0);int offset=int(SendMessageW(scroll,SBM_GETPOS,0,0));if(offset<=0)throw std::runtime_error("Editor scrollbar wheel did not scroll");
            SendMessageW(ew,WM_COMMAND,101,0);SendMessageW(ew,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(30,40));SendMessageW(ew,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(160,150));SendMessageW(ew,WM_LBUTTONUP,0,MAKELPARAM(160,150));
            if(tall->annotations.size()!=1||tall->annotations[0].start.y<=40)throw std::runtime_error("Scrolled annotation coordinate mismatch");
            SendMessageW(ew,WM_COMMAND,127,0);if(!tall->annotations.empty())throw std::runtime_error("Clear failed");SendMessageW(ew,WM_COMMAND,110,0);if(tall->annotations.size()!=1)throw std::runtime_error("Undo clear failed");
            SendMessageW(ew,WM_COMMAND,127,0);
            for(int testDpi:{96,144,192}){GetWindowRect(ew,&r);SendMessageW(ew,WM_DPICHANGED,MAKEWPARAM(testDpi,testDpi),reinterpret_cast<LPARAM>(&r));PumpFor(150);DesktopUiSnapshot(mi.rcMonitor,prefix+L"-editor-"+std::to_wstring(testDpi)+L".png");}
            // Save cancellation keeps the same editor and document available.
            std::cout<<"editor save cancel"<<std::endl;UINT_PTR timer=SetTimer(nullptr,0,100,CancelSaveDialog);SendMessageW(ew,WM_COMMAND,121,0);KillTimer(nullptr,timer);if(!IsWindowVisible(ew))throw std::runtime_error("Cancelled save closed editor");
            std::cout<<"editor finish"<<std::endl;SendMessageW(ew,WM_COMMAND,129,0);PumpFor(150);
            if(IsWindow(ew)||!IsClipboardFormatAvailable(CF_HDROP)||!IsClipboardFormatAvailable(CF_BITMAP))throw std::runtime_error("Finish did not close and copy PNG plus bitmap");
            winrt::check_bool(OpenClipboard(fixture));HDROP drop=static_cast<HDROP>(GetClipboardData(CF_HDROP));wchar_t copied[32768]{};DragQueryFileW(drop,0,copied,32768);CloseClipboard();
            if(!std::filesystem::exists(copied)||std::filesystem::path(copied).extension()!=L".png")throw std::runtime_error("Copied cache PNG absent");
            std::cout<<"editor finished"<<std::endl;editor.reset();session.reset();
            // Finalized session can retry after cancelled Download, then Finish without losing its image.
            completed=false;fixtureOffset=0;InvalidateRect(fixture,nullptr,FALSE);UpdateWindow(fixture);FocusNativeFixture(fixture);PumpFor(200);
            auto retryDoc=std::make_shared<ImageDocument>();session=OpenLongCaptureSession(region,retryDoc,[&](bool ok){completed=ok;},[](auto){});PumpFor(900);
            std::cout<<"session save cancel"<<std::endl;timer=SetTimer(nullptr,0,100,CancelSaveDialog);SendMessageW(session->Window(),WM_COMMAND,13,0);PumpFor(1000);KillTimer(nullptr,timer);
            if(!session->Active()||retryDoc->image.Empty())throw std::runtime_error("Cancelled download lost finalized capture");
            SendMessageW(session->Window(),WM_COMMAND,14,0);PumpFor(300);if(!completed)throw std::runtime_error("Retry finish after save cancellation failed");session.reset();
            for(bool escape:{false,true}){completed=false;success=true;session=OpenLongCaptureSession(region,std::make_shared<ImageDocument>(),[&](bool ok){completed=true;success=ok;},[](auto){});PumpFor(500);point();if(escape){INPUT keys[2]{};for(auto& k:keys){k.type=INPUT_KEYBOARD;k.ki.wVk=VK_ESCAPE;}keys[1].ki.dwFlags=KEYEVENTF_KEYUP;SendInput(2,keys,sizeof(INPUT));}else{mouse(MOUSEEVENTF_RIGHTDOWN);mouse(MOUSEEVENTF_RIGHTUP);}PumpFor(400);if(!completed||success||session->Active())throw std::runtime_error("Capture cancellation failed");session.reset();}
            // Full-screen fallback keeps every action inside the work area and clickable.
            completed=false;session=OpenLongCaptureSession(mi.rcMonitor,std::make_shared<ImageDocument>(),[&](bool){completed=true;},[](auto){});PumpFor(700);
            bar=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 长截图工具栏");GetWindowRect(bar,&r);
            RECT thumb{},intersection{};GetWindowRect(session->Window(),&thumb);if(IntersectRect(&intersection,&thumb,&r))throw std::runtime_error("Full-screen thumbnail overlaps action toolbar");
            if(r.left<mi.rcWork.left||r.right>mi.rcWork.right||r.top<mi.rcWork.top||r.bottom>mi.rcWork.bottom)throw std::runtime_error("Full-screen controls escaped work area");
            SetCursorPos(r.left+MulDiv(48+36+18,int(GetDpiForWindow(bar)),96),r.top+(r.bottom-r.top)/2);mouse(MOUSEEVENTF_LEFTDOWN);mouse(MOUSEEVENTF_LEFTUP);PumpFor(400);
            if(!completed||session->Active())throw std::runtime_error("Full-screen cancel button not clickable");session.reset();
            DestroyWindow(fixture);std::cout<<"QQ LONG PASS: actual wheel/click auto, pause, exact pixels/sticky edges, compact editor, DPI layouts, scroll, annotation/undo, save cancel/retry, PNG+bitmap, right/Esc cancel\n";return 0;
        }
        if(argc>1 && std::wstring(argv[1])==L"session-test") {
            CaptureTools tools; bool completed=false,success=false;
            auto document=std::make_shared<ImageDocument>();
            Annotation a; a.tool=Tool::Rectangle; a.start={20,20}; a.end={100,100}; a.size=3; document->annotations.push_back(a);
            const RECT region{origin.x,origin.y,origin.x+641,origin.y+361};
            std::cout<<"Session: long start"<<std::endl;
            tools.LongCapture(region,document,[&](bool ok){completed=true;success=ok;});
            const auto deadline=GetTickCount64()+8000; bool finishSent=false;
            while(!completed && GetTickCount64()<deadline) {
                MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                if(!finishSent && GetTickCount64()+6500>deadline) {
                    HWND control=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 长截图");
                    if(!control) throw std::runtime_error("Long capture controls missing");
                    SendMessageW(control,WM_COMMAND,11,0); finishSent=true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(!completed || !success || document->image.Empty() || document->annotations.size()!=1) throw std::runtime_error("Long capture completion did not preserve document");
            completed=false; success=true;
            std::cout<<"Session: record prepare"<<std::endl;
            tools.Record(region,[&](bool ok){completed=true;success=ok;});
            HWND recording=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 屏幕录制");
            if(!recording || !tools.FocusActive()) throw std::runtime_error("Recording preparation not active");
            SendMessageW(recording,WM_CLOSE,0,0);
            if(!completed || success || tools.FocusActive()) throw std::runtime_error("Recording preparation cancel failed");
            tools.Ocr(RenderDocument(*document));
            tools.Shutdown(); DestroyWindow(fixture);
            std::cout<<"SESSION PASS: long capture completion, annotation handoff, preparation cancel, OCR shutdown\n";
            return 0;
        }
        if(argc>1 && std::wstring(argv[1])==L"long-test") {
            scrollFixture=true; fixtureOffset=0; InvalidateRect(fixture,nullptr,FALSE); UpdateWindow(fixture);
            DesktopCapture capture({origin.x,origin.y,origin.x+641,origin.y+361},false);
            ScrollStitcher stitcher; Image previous;
            for(int offset:{0,60,120,180}) {
                fixtureOffset=offset; InvalidateRect(fixture,nullptr,FALSE); UpdateWindow(fixture);
                const auto deadline=GetTickCount64()+5000;
                bool accepted=false; Image candidate;StitchResult lastResult=StitchResult::Unchanged;
                while(GetTickCount64()<deadline) {
                    MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                    Image frame;
                    if(capture.Next(frame) && FrameDifference(previous,frame)>0.6) {
                        const bool stable=FrameDifference(candidate,frame)<0.8; candidate=frame;
                        if(!stable) continue;
                        const auto result=stitcher.Push(frame);
                        lastResult=result;
                        if(result==StitchResult::First || result==StitchResult::Appended) { previous=std::move(frame); accepted=true; break; }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if(!accepted){const auto a=stitcher.Analysis();std::cerr<<"Live failure offset="<<offset<<" result="<<int(lastResult)<<" displacement="<<a.displacement<<" roi="<<a.region.left<<","<<a.region.top<<","<<a.region.right<<","<<a.region.bottom<<std::endl;
                    const std::wstring prefix=argc>2?argv[2]:L"live-failure";SavePng(previous,prefix+L"-previous.png");SavePng(candidate,prefix+L"-candidate.png");throw std::runtime_error("Live scrolling frame failed to stitch");}
            }
            const auto result=stitcher.Flatten();
            if(result.width!=641 || result.height!=541) throw std::runtime_error("Live scrolling stitched wrong extent");
            SavePng(result,argc>2?argv[2]:L"capture-live-long.png"); DestroyWindow(fixture);
            std::cout<<"LONG CAPTURE PASS: live WGC frames, sticky borders, 180 px append\n"; return 0;
        }
        RecordingOptions options; options.region={origin.x,origin.y,origin.x+641,origin.y+361}; options.systemAudio=system; options.microphone=mic; options.path=path;
        RecordingState state; const auto start=GetTickCount64();
        std::thread worker([&]{RecordScreen(options,state);});
        while(!state.finished) {
            MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            const auto elapsed=GetTickCount64()-start;
            state.paused=elapsed>=1000 && elapsed<2000;
            if(elapsed>=ULONGLONG(seconds)*1000 || !IsWindow(fixture)) state.stop=true;
            InvalidateRect(fixture,nullptr,FALSE);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        worker.join(); if(IsWindow(fixture)) DestroyWindow(fixture);
        if(!state.error.empty()) { std::cerr<<winrt::to_string(state.error)<<'\n'; return 1; }
        winrt::check_hresult(MFStartup(MF_VERSION));
        const auto videoEnd=StreamEnd(path,DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        const auto preview=VideoPreview(path);
        SavePng(preview,path+L".preview.png");
        if(preview.width!=642 || preview.height!=362) throw std::runtime_error("Odd recording dimensions were not padded");
        const auto red=preview.pixels[size_t(270)*preview.width+100];
        if(((red>>16)&255)<150 || (red&255)>120) throw std::runtime_error("Video orientation or color mismatch");
        SavePng(preview,path+L".preview.png");
        if(videoEnd<int64_t(seconds-2)*10000000 || videoEnd>int64_t(seconds)*10000000) throw std::runtime_error("Pause not reflected in MP4 duration");
        int64_t audioEnd=0;
        { audioEnd=StreamEnd(path,DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM)); if(std::abs(audioEnd-videoEnd)>2000000) throw std::runtime_error("Audio/video end drift exceeds 200 ms"); }
        std::cout<<"RECORD PASS video="<<videoEnd/10000000.0<<" audio="<<audioEnd/10000000.0<<" seconds\n";
        MFShutdown(); winrt::uninit_apartment(); return 0;
    } catch(const winrt::hresult_error& e) { std::cerr<<winrt::to_string(ErrorMessage(L"Integration failure",e.code()))<<'\n'; return 1; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}














static void GifTest(RECT region,const std::wstring& prefix) {
    auto factory=winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    for(int rate:{1,15,30}) {
        RecordingOptions options;options.region=region;options.format=RecordingFormat::Gif;options.frameRate=rate;options.gifDpi=rate==1?96:rate==15?144:192;
        options.path=app_storage::Unique(L"Gif",L".gif").wstring();options.waitForStart=true;options.systemAudio=false;
        RecordingState state;state.cursorEnabled=false;std::thread worker([&]{RecordScreen(options,state);});
        auto deadline=GetTickCount64()+10000;
        while(!state.ready&&!state.finished&&GetTickCount64()<deadline)PumpFor(10);
        if(!state.ready){state.stop=true;worker.join();throw std::runtime_error(winrt::to_string(state.error));}
                if(rate==15) { std::lock_guard<std::mutex> lock(state.mutex);state.laserMode=true;state.liveLaser={{280,140},true,RGB(0,220,0),9};state.cursorEnabled=true; }
        state.start=true;PumpFor(1100);state.cursorEnabled=false;PumpFor(1100);state.RequestStop(RecordingStopReason::Save);worker.join();
        if(!state.error.empty()||state.savedPath.empty())throw std::runtime_error(winrt::to_string(state.error));
        winrt::com_ptr<IWICBitmapDecoder> decoder;winrt::check_hresult(factory->CreateDecoderFromFilename(state.savedPath.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,decoder.put()));
        UINT count{};winrt::check_hresult(decoder->GetFrameCount(&count));if(count<2)throw std::runtime_error("GIF has too few frames");
        winrt::com_ptr<IWICMetadataQueryReader> global;decoder->GetMetadataQueryReader(global.put());PROPVARIANT loop{};
        winrt::check_hresult(global->GetMetadataByName(L"/appext/application",&loop));
        if(loop.vt!=(VT_VECTOR|VT_UI1)||loop.caub.cElems!=11||memcmp(loop.caub.pElems,"NETSCAPE2.0",11))throw std::runtime_error("GIF looping extension missing");PropVariantClear(&loop);
        if(rate==15) {
            auto greenAt=[&](UINT index) {
                winrt::com_ptr<IWICBitmapFrameDecode> frame;decoder->GetFrame(index,frame.put());
                winrt::com_ptr<IWICFormatConverter> converter;factory->CreateFormatConverter(converter.put());
                winrt::check_hresult(converter->Initialize(frame.get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
                WICRect pixel{MulDiv(280,96,options.gifDpi),MulDiv(140,96,options.gifDpi),1,1};uint32_t value{};winrt::check_hresult(converter->CopyPixels(&pixel,4,4,reinterpret_cast<BYTE*>(&value)));
                return int((value>>8)&255)>int((value>>16)&255)+80;
            };
            if(!greenAt(2)||greenAt(count-2))throw std::runtime_error("GIF cursor switch or selected laser color failed");
        }
        int duration=0;
        for(UINT i=0;i<count;++i) {
            winrt::com_ptr<IWICBitmapFrameDecode> frame;winrt::check_hresult(decoder->GetFrame(i,frame.put()));
            UINT w{},h{};frame->GetSize(&w,&h);if(w!=UINT(MulDiv(321,96,options.gifDpi))||h!=UINT(MulDiv(181,96,options.gifDpi)))throw std::runtime_error("GIF DPI dimensions incorrect");
            winrt::com_ptr<IWICMetadataQueryReader> meta;frame->GetMetadataQueryReader(meta.put());PROPVARIANT value{};
            winrt::check_hresult(meta->GetMetadataByName(L"/grctlext/Delay",&value));duration+=value.uiVal;PropVariantClear(&value);
        }
        if(duration<190||duration>350)throw std::runtime_error("GIF duration differs from recording");
        std::cout<<"GIF "<<rate<<" FPS: "<<count<<" frames, "<<duration<<" cs"<<std::endl;
        CopyFileW(state.savedPath.c_str(),(prefix+L"-"+std::to_wstring(rate)+L".gif").c_str(),FALSE);
        if(rate==15) {
            auto preview=OpenRecordingPreview({state.savedPath,L"",RecordingFormat::Gif});PumpFor(150);
            if(!preview->State().ready||!preview->State().playing)throw std::runtime_error("GIF preview not playing");
            SendMessageW(preview->Window(),WM_COMMAND,PreviewPlay,0);if(preview->State().playing)throw std::runtime_error("GIF pause failed");
            WindowSnapshot(preview->Window(),prefix+L"-preview.png");
            preview->SaveTo(prefix+L"-download.gif");
            SendMessageW(preview->Window(),WM_COMMAND,PreviewComplete,0);
            if(IsWindow(preview->Window())||!IsClipboardFormatAvailable(CF_HDROP))throw std::runtime_error("GIF finish did not copy file");
        }
    }
    bool completed=false;RecordingResult result;
    auto session=OpenRecordingSession(region,[&](bool){completed=true;},[&](RecordingResult r){result=std::move(r);},RecordingFormat::Gif);
    HWND bar=session->Window();
    for(int dpi:{96,144,192}) {
        RECT r{};GetWindowRect(bar,&r);SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));
        WindowSnapshot(bar,prefix+L"-prepare-dpi"+std::to_wstring(dpi)+L".png");
    }
    const int nativeDpi=GetDpiForWindow(bar); RECT nativeBounds{};GetWindowRect(bar,&nativeBounds);
    SendMessageW(bar,WM_DPICHANGED,MAKEWPARAM(nativeDpi,nativeDpi),reinterpret_cast<LPARAM>(&nativeBounds));
    auto nativeMouse=[](DWORD flag){INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=flag;winrt::check_bool(SendInput(1,&input,sizeof(input))==1);PumpFor(60);};
    auto nativeClick=[&](int x,int y){RECT r{};GetWindowRect(bar,&r);SetCursorPos(r.left+MulDiv(x,nativeDpi,96),r.top+MulDiv(y,nativeDpi,96));nativeMouse(MOUSEEVENTF_LEFTDOWN);nativeMouse(MOUSEEVENTF_LEFTUP);};
    GetWindowRect(bar,&nativeBounds);SetCursorPos(nativeBounds.left+MulDiv(55,nativeDpi,96),nativeBounds.top+MulDiv(18,nativeDpi,96));nativeMouse(MOUSEEVENTF_LEFTDOWN);
    SetCursorPos(nativeBounds.left+MulDiv(10,nativeDpi,96),nativeBounds.top+MulDiv(18,nativeDpi,96));PumpFor(80);
    WindowSnapshot(bar,prefix+L"-slider1.png");
    SetCursorPos(nativeBounds.left+MulDiv(100,nativeDpi,96),nativeBounds.top+MulDiv(18,nativeDpi,96));PumpFor(80);nativeMouse(MOUSEEVENTF_LEFTUP);
    WindowSnapshot(bar,prefix+L"-slider30.png");
    if(GetCapture())throw std::runtime_error("GIF slider retained capture");
    nativeClick(178,18);SendMessageW(bar,WM_COMMAND,106,0);
    HWND panel=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
    if(panel&&IsWindowVisible(panel))throw std::runtime_error("Disabled GIF laser opened settings");
    nativeClick(178,18);nativeClick(214,18);
    panel=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · 录屏样式");
    if(!panel||!IsWindowVisible(panel))throw std::runtime_error("Prepare GIF laser settings missing");
    SendMessageW(bar,WM_COMMAND,113,0);PumpFor(1800);
    if(!result.path.empty())throw std::runtime_error("GIF completed during countdown");
    PumpFor(2500);
    WindowSnapshot(bar,prefix+L"-recording.png");
    SendMessageW(bar,WM_COMMAND,114,0);
    if(IsWindowVisible(panel))throw std::runtime_error("Cursor off left laser settings open");
    SendMessageW(bar,WM_COMMAND,114,0);IndependentPointerControls(bar,region,prefix);
    SendMessageW(bar,WM_COMMAND,111,0);PumpFor(300);SendMessageW(bar,WM_COMMAND,111,0);PumpFor(400);
    SendMessageW(bar,WM_COMMAND,113,0);
    auto deadline=GetTickCount64()+10000;while(result.path.empty()&&!completed&&GetTickCount64()<deadline)PumpFor(20);
    if(result.path.empty()||result.format!=RecordingFormat::Gif)throw std::runtime_error("GIF session did not return GIF");
    {
        winrt::com_ptr<IWICBitmapDecoder> decoder;winrt::check_hresult(factory->CreateDecoderFromFilename(result.path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,decoder.put()));
        winrt::com_ptr<IWICBitmapFrameDecode> frame;winrt::check_hresult(decoder->GetFrame(0,frame.put()));UINT width{},height{};frame->GetSize(&width,&height);
        if(width!=UINT(MulDiv(321,96,std::max(96,nativeDpi)))||height!=UINT(MulDiv(181,96,std::max(96,nativeDpi))))throw std::runtime_error("GIF automatic monitor DPI scaling failed");
        std::cout<<"GIF automatic DPI: "<<nativeDpi<<", output "<<width<<"x"<<height<<std::endl;
    }
    std::cout<<"GIF UI PASS: countdown, cursor/laser gating, pause, finish, preview, copy and download"<<std::endl;
    session.reset();
    for(bool started:{false,true}) {
        RecordingOptions options; options.region=region;options.format=RecordingFormat::Gif;options.waitForStart=true;options.path=app_storage::Unique(L"Gif",L".gif").wstring();
        RecordingState state;std::thread worker([&]{RecordScreen(options,state);});
        auto limit=GetTickCount64()+10000;while(!state.ready&&!state.finished&&GetTickCount64()<limit)PumpFor(10);
        state.start=started;if(started)PumpFor(250);state.RequestStop(RecordingStopReason::Discard);worker.join();
        if(!state.error.empty()||!DiscardRecordingFiles(state).empty()||std::filesystem::exists(options.path+L".partial.gif"))throw std::runtime_error("GIF discard left output");
    }
    RecordingOptions invalid;invalid.format=RecordingFormat::Gif;invalid.region=region;invalid.path=prefix+L"-missing-folder/file.gif";
    RecordingState failed;std::thread failure([&]{RecordScreen(invalid,failed);});failure.join();
    if(failed.error.empty()||!failed.savedPath.empty())throw std::runtime_error("GIF write failure claimed success");
    std::cout<<"GIF CANCEL PASS: before start, during capture and unwritable output"<<std::endl;
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromRect(&region,MONITOR_DEFAULTTONEAREST),&monitor);
    bool fullClosed=false;auto full=OpenRecordingSession(monitor.rcMonitor,[&](bool){fullClosed=true;},{},RecordingFormat::Gif);
    bar=full->Window();RECT before{},after{};GetWindowRect(bar,&before);
    SetCursorPos(before.left+2,before.top+2);nativeMouse(MOUSEEVENTF_LEFTDOWN);
    SetCursorPos(before.left-38,before.top-38);PumpFor(80);nativeMouse(MOUSEEVENTF_LEFTUP);GetWindowRect(bar,&after);
    if(EqualRect(&before,&after)||GetCapture())throw std::runtime_error("Fullscreen GIF toolbar did not drag");
    const int fullDpi=GetDpiForWindow(bar);SetCursorPos(after.right-MulDiv(114,fullDpi,96),after.top+MulDiv(18,fullDpi,96));nativeMouse(MOUSEEVENTF_LEFTDOWN);nativeMouse(MOUSEEVENTF_LEFTUP);
    if(!fullClosed||full->Active())throw std::runtime_error("Fullscreen GIF cancel was not clickable");
    std::cout<<"GIF NATIVE INPUT PASS: slider drag, cursor icons, laser and fullscreen prepare drag/cancel"<<std::endl;
    CaptureTools tools;tools.Record(region,[](bool){},true);
    bar=FindWindowW(L"PcTool.CaptureToolWindow",L"PcTool · GIF 录制");
    if(!bar)throw std::runtime_error("Application GIF session missing");
    SendMessageW(bar,WM_COMMAND,113,0);PumpFor(4100);
    RECT bounds{};GetWindowRect(bar,&bounds);const int dpi=GetDpiForWindow(bar);
    SetCursorPos(bounds.right-MulDiv(60,dpi,96),bounds.top+MulDiv(18,dpi,96));nativeMouse(MOUSEEVENTF_LEFTDOWN);nativeMouse(MOUSEEVENTF_LEFTUP);
    HWND preview{};auto limit=GetTickCount64()+6000;
    while(!preview&&GetTickCount64()<limit) {
        EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM p)->BOOL{if(WindowText(w)==L"PcTool · GIF 预览"){*reinterpret_cast<HWND*>(p)=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&preview));PumpFor(20);
    }
    if(!preview||!IsWindowVisible(preview))throw std::runtime_error("Application GIF end failed to show preview");
    GetWindowRect(preview,&bounds);POINT center{(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2};
    if(GetAncestor(WindowFromPoint(center),GA_ROOT)!=preview)throw std::runtime_error("Application GIF preview is obscured");
    for(int id:{PreviewSave,PreviewComplete})if(!IsWindowVisible(GetDlgItem(preview,id)))throw std::runtime_error("GIF preview actions missing");
    SendMessageW(preview,WM_CLOSE,0,0);
    PreviewChildProcess(L"gif-preview-visible-fixture",prefix+L"-15.gif");
    std::cout<<"GIF END PREVIEW PASS: application callback, real finish click, visible preview/actions and hidden startup"<<std::endl;
}
