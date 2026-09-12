#include "hotkeys/hotkey_settings.h"
#include <commctrl.h>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
namespace {
int step=0,calls=0;bool failed=false;
void Check(bool value){if(!value)failed=true;}
HWND Field(HWND w,int id){return GetDlgItem(GetDlgItem(w,250),id);}
std::filesystem::path visualOutput;int visualStep=0;
int dragStep=0,dragSamples=0,dragMismatch=0,dragEntered=0;POINT dragOrigin{};
LRESULT CALLBACK DragObserve(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){if(m==WM_ENTERSIZEMOVE)++dragEntered;return DefSubclassProc(w,m,wp,lp);}
std::vector<DWORD> Pixels(HWND w){RECT r{};GetClientRect(w,&r);POINT p{};ClientToScreen(w,&p);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=r.right;info.bmiHeader.biHeight=-r.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* bits{};auto dc=CreateCompatibleDC(nullptr);auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);auto old=SelectObject(dc,bitmap);auto screen=GetDC(nullptr);BitBlt(dc,0,0,r.right,r.bottom,screen,p.x,p.y,SRCCOPY);GdiFlush();std::vector<DWORD> values(static_cast<DWORD*>(bits),static_cast<DWORD*>(bits)+r.right*r.bottom);ReleaseDC(nullptr,screen);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return values;}
void PaintPending(){MSG msg{};while(PeekMessageW(&msg,nullptr,WM_PAINT,WM_PAINT,PM_REMOVE))DispatchMessageW(&msg);DwmFlush();}
int boundaryPaints=0,boundaryMoves=0;
LRESULT CALLBACK BoundaryObserve(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(m==WM_PAINT)++boundaryPaints;
    if(m==WM_WINDOWPOSCHANGING)++boundaryMoves;
    return DefSubclassProc(w,m,wp,lp);
}
void BoundaryWheel(HWND w){
    for(int direction:{1,-1}){
        SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(direction*120*100)),0);PaintPending();
        std::vector<HWND> watched{w,GetDlgItem(w,250),GetDlgItem(w,501),GetDlgItem(w,IDOK)};
        for(int i=0;i<9;++i)watched.push_back(Field(w,300+i));
        for(auto child:watched)SetWindowSubclass(child,BoundaryObserve,991,0);
        boundaryPaints=boundaryMoves=0;
        for(int i=0;i<30;++i)SendMessageW(i%2?w:Field(w,308),WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(direction*120)),0);
        PaintPending();
        for(auto child:watched)RemoveWindowSubclass(child,BoundaryObserve,991);
        Check(boundaryPaints==0&&boundaryMoves==0);
        std::cout<<"Boundary "<<direction<<": paints="<<boundaryPaints<<", moves="<<boundaryMoves<<"\n";
    }
}
void RepaintStress(HWND w){
    auto viewport=GetDlgItem(w,250);RECT original{};GetWindowRect(w,&original);size_t changed=0;
    for(int i=0;i<30;++i){
        SetWindowPos(w,nullptr,0,0,original.right-original.left-(i%3)*17,original.bottom-original.top-(i%4)*13,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD((i%2?-1:1)*WHEEL_DELTA)),0);DwmFlush();auto before=Pixels(viewport);
        RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);DwmFlush();auto after=Pixels(viewport);
        if(before!=after)++changed;
    }
    std::cout<<"Repaint stress DPI "<<visualStep/4<<": "<<changed<<" inconsistent frames / 30\n";Check(changed==0);
    SetWindowPos(w,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,120*20),0);PaintPending();
}
void Snapshot(HWND w,const std::filesystem::path& path){RECT r{};GetWindowRect(w,&r);InflateRect(&r,28,28);auto screen=GetDC(nullptr);auto dc=CreateCompatibleDC(screen);auto bitmap=CreateCompatibleBitmap(screen,r.right-r.left,r.bottom-r.top);auto old=SelectObject(dc,bitmap);BitBlt(dc,0,0,r.right-r.left,r.bottom-r.top,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);Gdiplus::Bitmap image(bitmap,nullptr);UINT count=0,size=0;Gdiplus::GetImageEncodersSize(&count,&size);std::vector<BYTE> buffer(size);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());Gdiplus::GetImageEncoders(count,size,codecs);for(UINT i=0;i<count;++i)if(wcscmp(codecs[i].MimeType,L"image/png")==0)Check(image.Save(path.c_str(),&codecs[i].Clsid)==Gdiplus::Ok);DeleteObject(bitmap);}
void CALLBACK DragTick(HWND,UINT,UINT_PTR timer,DWORD){
    HWND w=FindWindowW(L"PcTool.HotkeySettings",nullptr);if(!w){KillTimer(nullptr,timer);failed=true;return;}
    const int trial=dragStep/8,phase=dragStep++%8,edge=trial%4;const UINT dpi[]={96,144,192};
    auto mouse=[](DWORD flag){INPUT input{};input.type=INPUT_MOUSE;input.mi.dwFlags=flag;Check(SendInput(1,&input,sizeof(input))==1);};
    if(phase==0){if(trial==0)SetWindowSubclass(w,DragObserve,990,0);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi);int width=std::min(MulDiv(760,dpi[trial/4],96),int(mi.rcWork.right-mi.rcWork.left)-180),height=std::min(MulDiv(620,dpi[trial/4],96),int(mi.rcWork.bottom-mi.rcWork.top)-180);RECT r{mi.rcWork.left+90,mi.rcWork.top+90,mi.rcWork.left+90+width,mi.rcWork.top+90+height};SendMessageW(w,WM_DPICHANGED,MAKEWPARAM(dpi[trial/4],dpi[trial/4]),reinterpret_cast<LPARAM>(&r));SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-240)),0);return;}
    if(phase==1){RECT r{};GetWindowRect(w,&r);dragOrigin=edge<2?POINT{edge==1?r.left+3:r.right-3,(r.top+r.bottom)/2}:POINT{(r.left+r.right)/2,edge==3?r.top+3:r.bottom-3};SetCursorPos(dragOrigin.x,dragOrigin.y);mouse(MOUSEEVENTF_LEFTDOWN);return;}
    if(phase==2||phase==4){int amount=phase==2?65:-25;SetCursorPos(dragOrigin.x+(edge<2?(edge==1?amount:-amount):0),dragOrigin.y+(edge>=2?(edge==3?amount:-amount):0));return;}
    if(phase==6){mouse(MOUSEEVENTF_LEFTUP);return;}
    DwmFlush();auto before=Pixels(w);Snapshot(w,visualOutput/(L"drag-"+std::to_wstring(trial)+L"-"+std::to_wstring(phase)+L".png"));RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);DwmFlush();auto after=Pixels(w);++dragSamples;if(before!=after)++dragMismatch;
    if(phase==7&&trial==11){KillTimer(nullptr,timer);std::cout<<"Native resize loops="<<dragEntered<<", mismatched frames="<<dragMismatch<<"/"<<dragSamples<<"\n";Check(dragEntered==12&&dragMismatch==0);SendMessageW(w,WM_CLOSE,0,0);}
}
void CALLBACK VisualTick(HWND,UINT,UINT_PTR timer,DWORD){HWND w=FindWindowW(L"PcTool.HotkeySettings",nullptr);if(!w){KillTimer(nullptr,timer);failed=true;return;}const int scale=visualStep/4;const UINT dpi[]={96,144,192};
    switch(visualStep%4){case 0:{MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi);RECT r{mi.rcWork.left+40,mi.rcWork.top+40,mi.rcWork.left+40+MulDiv(760,dpi[scale],96),mi.rcWork.top+40+std::min(MulDiv(720,dpi[scale],96),int(mi.rcWork.bottom-mi.rcWork.top)-80)};SendMessageW(w,WM_DPICHANGED,MAKEWPARAM(dpi[scale],dpi[scale]),reinterpret_cast<LPARAM>(&r));SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,120*20),0);break;}
    case 1:RepaintStress(w);Snapshot(w,visualOutput/(L"hotkeys-"+std::to_wstring(dpi[scale])+L"-top.png"));SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-120*20),0);break;
    case 2:Snapshot(w,visualOutput/(L"hotkeys-"+std::to_wstring(dpi[scale])+L"-bottom.png"));break;
    case 3:if(scale==2){KillTimer(nullptr,timer);SendMessageW(w,WM_CLOSE,0,0);}break;}
    ++visualStep;
}
void CALLBACK Tick(HWND,UINT,UINT_PTR timer,DWORD){
    HWND w=FindWindowW(L"PcTool.HotkeySettings",nullptr);if(!w){failed=true;KillTimer(nullptr,timer);PostQuitMessage(1);return;}
    const auto extended=GetWindowLongPtrW(w,GWL_EXSTYLE);
    Check((extended&WS_EX_APPWINDOW)!=0&&(extended&WS_EX_TOOLWINDOW)==0);
    Check((GetWindowLongPtrW(w,GWL_STYLE)&WS_CHILD)==0);
    auto set=[&](int index,WORD value){SendMessageW(Field(w,300+index),HKM_SETHOTKEY,value,0);};
    auto submit=[&]{SendMessageW(w,WM_COMMAND,IDOK,0);};
    switch(step++){
    case 0: BoundaryWheel(w);Check(SendMessageW(Field(w,300),HKM_GETHOTKEY,0,0)==MAKEWORD('A',HOTKEYF_CONTROL|HOTKEYF_ALT));set(1,MAKEWORD('A',HOTKEYF_CONTROL|HOTKEYF_ALT));submit();Check(calls==0&&IsWindow(w));break;
    case 1: set(1,MAKEWORD(0,HOTKEYF_CONTROL));submit();Check(calls==0&&IsWindow(w));break;
    case 2: SendMessageW(w,WM_COMMAND,501,0);Check(SendMessageW(Field(w,301),HKM_GETHOTKEY,0,0)==MAKEWORD('C',HOTKEYF_ALT));for(int i=5;i<9;++i)Check(SendMessageW(Field(w,300+i),HKM_GETHOTKEY,0,0)==0);set(4,MAKEWORD('T',HOTKEYF_CONTROL|HOTKEYF_SHIFT));submit();Check(calls==1&&IsWindow(w));break;
    case 3: submit();Check(calls==2&&IsWindowVisible(w));SendMessageW(w,WM_CLOSE,0,0);Check(!IsWindow(w));KillTimer(nullptr,timer);break;
    default: failed=true;DestroyWindow(w);KillTimer(nullptr,timer);break;
    }
}
}
int main(int argc,char** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
    if(argc>1){
        visualOutput=argv[1];std::filesystem::create_directories(visualOutput);
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PcTool.HotkeyTestBackdrop";wc.hbrBackground=CreateSolidBrush(RGB(224,231,235));RegisterClassW(&wc);
        HWND backdrop=CreateWindowExW(0,wc.lpszClassName,L"Synthetic test background",WS_POPUP|WS_VISIBLE,GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),nullptr,nullptr,wc.hInstance,nullptr);
        SetForegroundWindow(backdrop);UpdateWindow(backdrop);
        SetTimer(nullptr,0,250,argc>2?DragTick:VisualTick);hotkeys::ShowSettings(backdrop,hotkeys::Defaults,[](const hotkeys::Bindings&,std::wstring&){return true;});
        DestroyWindow(backdrop);UnregisterClassW(wc.lpszClassName,wc.hInstance);DeleteObject(wc.hbrBackground);return failed?1:0;
    }
    Check(hotkeys::Valid(hotkeys::Defaults));auto duplicate=hotkeys::Defaults;duplicate[1]=duplicate[0];Check(!hotkeys::Valid(duplicate));Check(!hotkeys::Valid(hotkeys::Binding{0,'A'}));Check(hotkeys::Valid(hotkeys::Binding{0,VK_F8}));
    SetTimer(nullptr,0,100,Tick);
    hotkeys::ShowSettings(nullptr,hotkeys::Defaults,[](const hotkeys::Bindings& values,std::wstring& error){++calls;Check(values[4]==hotkeys::Binding{MOD_CONTROL|MOD_SHIFT,'T'});error=L"模拟占用：请修改后重试。";return calls==2;});
    SetTimer(nullptr,0,100,[](HWND,UINT,UINT_PTR timer,DWORD){KillTimer(nullptr,timer);auto w=FindWindowW(L"PcTool.HotkeySettings",nullptr);SendMessageW(w,WM_COMMAND,IDCANCEL,0);});
    hotkeys::ShowSettings(nullptr,hotkeys::Defaults,[](const hotkeys::Bindings&,std::wstring&){failed=true;return false;});
    SetTimer(nullptr,0,100,[](HWND,UINT,UINT_PTR timer,DWORD){KillTimer(nullptr,timer);auto w=FindWindowW(L"PcTool.HotkeySettings",nullptr);Check(GetDlgItem(w,IDCANCEL)==nullptr);for(int i=0;i<9;++i)SendMessageW(Field(w,300+i),HKM_SETHOTKEY,0,0);SendMessageW(w,WM_COMMAND,IDOK,0);Check(IsWindowVisible(w));SendMessageW(w,WM_CLOSE,0,0);Check(!IsWindow(w));});
    hotkeys::ShowSettings(nullptr,hotkeys::Defaults,[](const hotkeys::Bindings& values,std::wstring&){for(auto value:values)Check(hotkeys::Empty(value));return true;});
    int closeStage=0;
    static int* phase;phase=&closeStage;
    SetTimer(nullptr,0,100,[](HWND,UINT,UINT_PTR timer,DWORD){
        auto w=FindWindowW(L"PcTool.HotkeySettings",nullptr);
        switch((*phase)++){
        case 0:SendMessageW(Field(w,300),HKM_SETHOTKEY,0,0);PostMessageW(w,WM_CLOSE,0,0);break;
        case 1:{auto confirm=FindWindowW(L"PcTool.TranslationConfirmation",nullptr);Check(confirm&&!IsWindowEnabled(w));SendMessageW(confirm,WM_COMMAND,IDNO,0);break;}
        case 2:Check(IsWindowEnabled(w)&&SendMessageW(Field(w,300),HKM_GETHOTKEY,0,0)==0);PostMessageW(w,WM_CLOSE,0,0);break;
        case 3:{auto confirm=FindWindowW(L"PcTool.TranslationConfirmation",nullptr);Check(confirm!=nullptr);KillTimer(nullptr,timer);SendMessageW(confirm,WM_COMMAND,IDYES,0);break;}
        }
    });
    hotkeys::ShowSettings(nullptr,hotkeys::Defaults,[](const hotkeys::Bindings&,std::wstring&){failed=true;return false;});
    Check(closeStage==4);
    static int captureStage=0;
    SetTimer(nullptr,0,120,[](HWND,UINT,UINT_PTR timer,DWORD){
        auto w=FindWindowW(L"PcTool.HotkeySettings",nullptr);auto field=Field(w,308);
        switch(captureStage++){
        case 0:{SetForegroundWindow(w);SetFocus(field);INPUT input[8]{};WORD keys[]{VK_CONTROL,VK_MENU,VK_SHIFT,VK_F24,VK_F24,VK_SHIFT,VK_MENU,VK_CONTROL};for(int i=0;i<8;++i){input[i].type=INPUT_KEYBOARD;input[i].ki.wVk=keys[i];input[i].ki.dwFlags=i>=4?KEYEVENTF_KEYUP:0;}Check(SendInput(8,input,sizeof(INPUT))==8);break;}
        case 1:{Check(SendMessageW(field,HKM_GETHOTKEY,0,0)==MAKEWORD(VK_F24,HOTKEYF_CONTROL|HOTKEYF_ALT|HOTKEYF_SHIFT));INPUT input[2]{};input[0].type=input[1].type=INPUT_KEYBOARD;input[0].ki.wVk=input[1].ki.wVk=VK_DELETE;input[1].ki.dwFlags=KEYEVENTF_KEYUP;Check(SendInput(2,input,sizeof(INPUT))==2);break;}
        case 2:Check(SendMessageW(field,HKM_GETHOTKEY,0,0)==0);KillTimer(nullptr,timer);SendMessageW(w,WM_CLOSE,0,0);break;
        }
    });
    hotkeys::ShowSettings(nullptr,hotkeys::Defaults,[](const hotkeys::Bindings&,std::wstring&){failed=true;return false;});
    if(failed){std::cerr<<"Hotkey settings failed\n";return 1;}
    std::cout<<"PASS: nine bindings, duplicate, invalid, all unbound, defaults, save failure/retry, dirty close cancel/discard\n";
}
