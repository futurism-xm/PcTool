#include "ocr/ocr_service.h"
#include "ocr/ocr_window.h"
#include "capture/screenshot_overlay.h"
#include "translation/translation_capture.h"
#include "shared/ui/capture_ui.h"
#include <psapi.h>
#include <iostream>
#include <thread>
#include <future>

static void PumpMemoryTest(int milliseconds) {
    const auto end=GetTickCount64()+milliseconds; MSG msg{};
    do { while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);} Sleep(10); }while(GetTickCount64()<end);
}
static SIZE_T MemorySample(const char* phase) {
    PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
    K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
    std::cout<<phase<<", private_MiB="<<memory.PrivateUsage/1048576.0<<", working_MiB="<<memory.WorkingSetSize/1048576.0
        <<", GDI="<<GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<<std::endl;
    return memory.PrivateUsage;
}
static capture::Image MemoryTextImage(int width,int height) {
    HDC dc=CreateCompatibleDC(nullptr); BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    auto old=SelectObject(dc,bitmap);RECT rect{0,0,width,height};FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font=CreateFontW(-32,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei");
    auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
    const wchar_t text[]=L"Memory regression test 12345 中英文字识别测试";
    for(int y=40;y<height-40;y+=140)TextOutW(dc,40,y,text,lstrlenW(text));
    GdiFlush();capture::Image image(width,height);memcpy(image.pixels.data(),bits,image.pixels.size()*4);
    SelectObject(dc,oldFont);SelectObject(dc,old);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);return image;
}
void CaptureMemoryTests(bool enforce) {
    MemorySample("baseline");
    ScreenshotOverlay overlay(GetModuleHandleW(nullptr));
    SIZE_T screenshotWarm=0;DWORD screenshotGdi=0;
    for(int i=0;i<10;++i){
        if(!overlay.Start())throw std::runtime_error("Screenshot start failed");PumpMemoryTest(40);overlay.Cancel();PumpMemoryTest(40);
        if(i==4){screenshotWarm=MemorySample("screenshot_cancel_5");screenshotGdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);}
    }
    if(enforce && (MemorySample("screenshot_cancel_10")>screenshotWarm+8*1048576ull || GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)>screenshotGdi+2))throw std::runtime_error("Screenshot capture resources accumulate");
    translation::TranslationCapture selection;
    for(int i=0;i<10;++i){if(!selection.Start([](capture::Image){},[]{}))throw std::runtime_error("Translation capture failed");PumpMemoryTest(40);selection.Cancel();PumpMemoryTest(40);}
    MemorySample("translation_capture_cancel_10");
    SIZE_T firstIdle=0;
    for(int i=0;i<4;++i){
        const auto start=GetTickCount64();
        {auto image=MemoryTextImage(i==1?2560:1280,i==1?1440:800);capture::Cancellation cancel;
         auto result=capture::RecognizeLocalDocument(image,cancel);
         if(result.text.find(L"12345")==std::wstring::npos)throw std::runtime_error("OCR text regression");}
        PumpMemoryTest(100);
        const auto current=MemorySample(("ocr_idle_"+std::to_string(i)).c_str());
        std::cout<<"ocr_total_ms="<<GetTickCount64()-start<<std::endl;
        if(!i)firstIdle=current;
        if(enforce && current>firstIdle+32*1048576ull)throw std::runtime_error("OCR retained large-image inference allocation");
    }
    auto ocr=capture::OpenOcr(MemoryTextImage(1280,800));
    auto deadline=GetTickCount64()+30000;
    while(ocr->Window()&&!GetDlgItem(ocr->Window(),402)&&GetTickCount64()<deadline)PumpMemoryTest(20);
    if(!ocr->Window()||!GetDlgItem(ocr->Window(),402))throw std::runtime_error("OCR result not ready");
    MemorySample("ocr_window_ready");
    SendMessageW(ocr->Window(),WM_CLOSE,0,0);PumpMemoryTest(100);
    const auto closedMemory=MemorySample("ocr_window_closed_object_retained");
    ocr.reset();
    const auto destroyedMemory=MemorySample("ocr_object_destroyed");
    if(enforce && closedMemory>destroyedMemory+2*1048576ull)throw std::runtime_error("Closed OCR window retained source/preview pixels");
    auto cancelled=capture::OpenOcr(MemoryTextImage(2560,1440));
    SendMessageW(cancelled->Window(),WM_CLOSE,0,0);PumpMemoryTest(2000);
    const auto cancelledMemory=MemorySample("cancelled_window_object_retained");cancelled.reset();
    if(enforce && cancelledMemory>MemorySample("cancelled_window_destroyed")+4*1048576ull)throw std::runtime_error("Cancelled OCR future retained pixels");
    // A request cancelled while waiting for another OCR must not load a second engine.
    auto image=MemoryTextImage(1280,800);capture::Cancellation active,queued;
    auto one=std::async(std::launch::async,[&]{return capture::RecognizeLocalDocument(image,active);});
    auto two=std::async(std::launch::async,[&]{return capture::RecognizeLocalDocument(image,queued);});
    queued.requested=true;
    if(two.wait_for(std::chrono::seconds(3))!=std::future_status::ready)throw std::runtime_error("Queued cancellation stalled");
    two.get();one.get();
    std::cout<<"PASS repeated OCR, cancellation and screenshot lifecycle"<<std::endl;
}
