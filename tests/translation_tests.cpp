#include "translation/translation_controller.h"
#include "translation/translation_capture.h"
#include "translation/translation_view.h"
#include "shared/ui/capture_ui.h"
#include "shared/platform/capture_platform.h"
#include "clipboard/clipboard_history.h"
#include "monitor/taskbar_monitor_window.h"
#include "app/app_messages.h"
#include <filesystem>
#include <objbase.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <functional>
#include <future>
#include <richedit.h>
#include <thread>
#include <atomic>
int RunTranslationSourceTests(const std::filesystem::path&,bool);
namespace {
translation::TranslationController* service{};
shared_platform::MenuSourceTracker* menuTracker{};
shared_platform::MenuSource menuSource{};
int menuDispatched{};
HWND OwnWindow(const wchar_t* name){struct Search{const wchar_t* name;HWND found{};} search{name};EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM data)->BOOL{auto& search=*reinterpret_cast<Search*>(data);wchar_t name[128]{};GetClassNameW(w,name,128);if(wcscmp(name,search.name)==0){search.found=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&search));return search.found;}
HWND Child(HWND parent,int id){HWND child=GetDlgItem(parent,id);if(!child)child=GetDlgItem(GetDlgItem(parent,1110),id);return child;}
void Require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
void Pump(int ms){const auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}while(GetTickCount64()<end);}
bool Until(std::function<bool()> condition,int ms=2500){const auto end=GetTickCount64()+ms;do{Pump(20);if(condition())return true;}while(GetTickCount64()<end);return false;}
void ActivateSource(HWND target){
    SetWindowPos(target,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    RECT r{};GetWindowRect(target,&r);SetCursorPos(r.left+40,r.top+80);
    INPUT events[2]{};for(auto& event:events)event.type=INPUT_MOUSE;
    events[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;events[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,events,sizeof(INPUT));Pump(60);
    SetWindowPos(target,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
LRESULT CALLBACK HostProc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==kPrepareTranslationMenuMessage&&menuTracker){menuSource=menuTracker->Freeze();return 0;}
    if(m==kInvokeTranslationMenuMessage&&service){
        Require(!IsWindowVisible(OwnWindow(L"#32768"))&&GetCapture()==nullptr,"translation dispatched before menu closed/released capture");
        ++menuDispatched;service->Invoke(static_cast<translation::Entry>(wp),true,menuSource);return 0;
    }
    if(m==WM_HOTKEY&&service&&service->HandleHotkey(wp))return 0;if(m==WM_APP+1){SetFocus(GetDlgItem(w,101));SendMessageW(GetDlgItem(w,101),EM_SETSEL,0,-1);return 0;}if(m==WM_SETFOCUS&&GetDlgItem(w,101)){SetFocus(GetDlgItem(w,101));return 0;}if(m==WM_CLOSE){DestroyWindow(w);return 0;}return DefWindowProcW(w,m,wp,lp);
}
void TranslationMenu(HWND owner,int entry){
    (void)owner;
    AppSettings settings;TaskbarMonitorWindow monitor(GetModuleHandleW(nullptr),settings);
    WNDCLASSW wc{};wc.lpfnWndProc=HostProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PcTool.BackgroundController";RegisterClassW(&wc);
    // Match the application's persistent hidden, unowned menu controller.
    static HWND menuOwner=CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,wc.hInstance,nullptr);
    shared_platform::MenuSourceTracker tracker;tracker.Start();menuTracker=&tracker;
    static int tick,target;tick=0;target=entry;const int before=menuDispatched;
    auto timer=SetTimer(nullptr,0,100,[](HWND,UINT,UINT_PTR id,DWORD){
        auto menu=OwnWindow(L"#32768");HMENU root=menu?reinterpret_cast<HMENU>(SendMessageW(menu,0x01e1,0,0)):nullptr;
        if(++tick>30){KillTimer(nullptr,id);EndMenu();return;}
        // The first popup owns the translation submenu; locate it even after the child opens.
        EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM p)->BOOL{wchar_t name[40]{};GetClassNameW(w,name,40);if(wcscmp(name,L"#32768")==0){auto h=reinterpret_cast<HMENU>(SendMessageW(w,0x01e1,0,0));if(GetMenuItemCount(h)>8)*reinterpret_cast<HMENU*>(p)=h;}return TRUE;},reinterpret_cast<LPARAM>(&root));
        if(!root)return;RECT r{};auto sub=GetSubMenu(root,1);
        if(tick==1){if(GetMenuItemRect(nullptr,root,1,&r))SetCursorPos(r.right-8,(r.top+r.bottom)/2);}
        else if(GetMenuItemRect(nullptr,sub,target,&r)&&r.right>r.left){SetCursorPos((r.left+r.right)/2,(r.top+r.bottom)/2);INPUT events[2]{};events[0].type=events[1].type=INPUT_MOUSE;events[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;events[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,events,sizeof(INPUT));KillTimer(nullptr,id);}
    });
    monitor.ShowMenu(menuOwner,{250,180});KillTimer(nullptr,timer);Pump(80);menuTracker=nullptr;
    Require(menuDispatched==before+1,"real translation submenu click did not dispatch once");
}
std::wstring ClipboardText(){Require(OpenClipboard(nullptr)!=FALSE,"read restored clipboard");std::wstring text;auto memory=GetClipboardData(CF_UNICODETEXT);if(memory){auto* value=static_cast<const wchar_t*>(GlobalLock(memory));if(value){text=value;GlobalUnlock(memory);}}CloseClipboard();return text;}
std::wstring Status(HWND window){NMTTDISPINFOW tip{};tip.hdr.code=TTN_GETDISPINFOW;tip.hdr.idFrom=1200;SendMessageW(window,WM_NOTIFY,1200,reinterpret_cast<LPARAM>(&tip));return tip.lpszText?tip.lpszText:L"";}
HWND Host(const wchar_t* name){WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=HostProc;wc.lpszClassName=name;wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);return CreateWindowExW(0,name,L"Translation integration source",WS_OVERLAPPEDWINDOW|WS_VISIBLE,120,100,700,350,nullptr,nullptr,wc.hInstance,nullptr);}
void Snapshot(HWND w,const std::filesystem::path& path){RECT r{};GetWindowRect(w,&r);capture::Image image(r.right-r.left,r.bottom-r.top);HBITMAP b=capture::ToBitmap(image);HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,b);PrintWindow(w,dc,PW_CLIENTONLY|2);SelectObject(dc,old);DeleteDC(dc);image=capture::FromBitmap(b);DeleteObject(b);Require(capture::SavePng(image,path.wstring()),"save screenshot failed");}
void DesktopSnapshot(HWND w,const std::filesystem::path& path){
    RECT r{};GetWindowRect(w,&r);InflateRect(&r,32,32);capture::Image image(r.right-r.left,r.bottom-r.top);
    HBITMAP bitmap=capture::ToBitmap(image);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);auto old=SelectObject(dc,bitmap);
    BitBlt(dc,0,0,image.width,image.height,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);image=capture::FromBitmap(bitmap);DeleteObject(bitmap);Require(capture::SavePng(image,path.wstring()),"desktop snapshot");
}
void Command(HWND w,int id){SendMessageW(w,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(Child(w,id)));Pump(40);}
void Key(WORD key){INPUT events[2]{};for(auto& e:events)e.type=INPUT_KEYBOARD;events[0].ki.wVk=events[1].ki.wVk=key;events[1].ki.dwFlags=KEYEVENTF_KEYUP;Require(SendInput(2,events,sizeof(INPUT))==2,"fixture key injection");Pump(60);}
void AltE(){INPUT events[4]{};for(auto& e:events)e.type=INPUT_KEYBOARD;events[0].ki.wVk=events[3].ki.wVk=VK_MENU;events[1].ki.wVk=events[2].ki.wVk='E';events[2].ki.dwFlags=events[3].ki.dwFlags=KEYEVENTF_KEYUP;Require(SendInput(4,events,sizeof(INPUT))==4,"Alt+E injection");}
void BrowserCase(HWND host,HWND popup,HWND edit,const std::filesystem::path& output){
    std::filesystem::path browser=L"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
    if(!std::filesystem::exists(browser)){std::cout<<"NOT EXECUTED: Edge browser fixture (browser unavailable)\n";return;}
    const auto html=std::filesystem::absolute(output/L"browser-fixture.html"),profile=std::filesystem::absolute(output/L"browser-profile");
    {std::ofstream file(html);file<<u8R"(<!doctype html><meta charset="utf-8"><title>PcTool Browser Clipboard Fixture</title><style>body{font:20px sans-serif;background:#f5f5f5;margin:30px}main{height:600px;background:white;padding:24px}</style><main tabindex="0"><p id="text">中文取词 Browser English 123</p><p>F8 selects the test paragraph; F9 clears the selection. This page does not intercept copying.</p></main><script>addEventListener('keydown',e=>{if(e.key==='F8'){getSelection().selectAllChildren(document.querySelector('#text'));e.preventDefault()}if(e.key==='F9'){getSelection().removeAllRanges();document.querySelector('main').focus();e.preventDefault()}})</script>)";}
    std::wstring command=L"\""+browser.wstring()+L"\" --no-first-run --no-default-browser-check --user-data-dir=\""+profile.wstring()+L"\" --app=\"file:///"+html.generic_wstring()+L"\"";
    STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};Require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&process)!=FALSE,"launch browser fixture");CloseHandle(process.hThread);
    HWND browserWindow{};
    Until([&]{EnumWindows([](HWND w,LPARAM data)->BOOL{wchar_t title[256]{};GetWindowTextW(w,title,256);if(wcsstr(title,L"PcTool Browser Clipboard Fixture")){*reinterpret_cast<HWND*>(data)=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&browserWindow));return browserWindow!=nullptr;},10000);
    if(!browserWindow){CloseHandle(process.hProcess);throw std::runtime_error("browser fixture window unavailable");}
    struct Close{HWND w;HANDLE process;~Close(){PostMessageW(w,WM_CLOSE,0,0);CloseHandle(process);}} cleanup{browserWindow,process.hProcess};
    ActivateSource(browserWindow);Key(VK_F9);
    ULONGLONG maximum{};
    for(int i=0;i<10;++i){
        const auto sequence=GetClipboardSequenceNumber();const auto begin=GetTickCount64();AltE();
        Require(Until([&]{return IsWindowVisible(popup)&&GetClipboardSequenceNumber()!=sequence&&Status(popup)==L"识别内容为空";},450),"browser blank hotkey did not show immediately");
        maximum=std::max(maximum,GetTickCount64()-begin);Require(capture::WindowText(edit).empty(),"browser blank reused text");
        RECT r{};GetWindowRect(popup,&r);HWND top=WindowFromPoint({(r.left+r.right)/2,(r.top+r.bottom)/2});Require(GetAncestor(top,GA_ROOT)==popup,"browser blank popup occluded");
        Pump(80);Require(IsWindowVisible(popup),"browser blank popup disappeared");
        if(i==0||i==9)DesktopSnapshot(popup,output/(L"browser-blank-"+std::to_wstring(i)+L".png"));
    }
    std::cout<<"PASS browser page blank: 10 registered Alt+E calls, maximum observed latency "<<maximum<<" ms, actual z-order checked\n";
    Key(VK_ESCAPE);Require(!IsWindowVisible(popup),"passive browser popup ignored real Escape");ActivateSource(browserWindow);Key(VK_F8);
    for(int i=0;i<10;++i){AltE();Require(Until([&]{return IsWindowVisible(popup)&&capture::WindowText(edit)==L"中文取词 Browser English 123"&&Status(popup).empty();}),"browser real selection copy/repeat failed");Pump(80);Require(IsWindowVisible(popup),"browser text popup disappeared");}
    DesktopSnapshot(popup,output/L"browser-selection.png");std::cout<<"PASS browser selectable text: 10 registered Alt+E calls\n";
    SetForegroundWindow(browserWindow);Pump(60);TranslationMenu(host,2);
    Require(Until([&]{return capture::WindowText(edit)==L"中文取词 Browser English 123"&&Status(popup).empty();}),"browser menu selection failed");
    Require(menuSource.window==browserWindow,"browser menu source mismatch");
    std::cout<<"PASS browser real submenu selection\n";
    SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);
}
struct DeferredProvider:translation::TranslationProvider{
    std::vector<std::function<void(translation::TranslationResult)>> callbacks;
    translation::ProviderInfo Info()const override{return {L"test",L"test",true};}
    void Translate(std::wstring,std::shared_ptr<capture::Cancellation>,std::function<void(translation::TranslationResult)> done)override{callbacks.push_back(std::move(done));}
};
struct FixtureProvider:translation::TranslationProvider{
    std::wstring id;explicit FixtureProvider(std::wstring value):id(std::move(value)){}
    translation::ProviderInfo Info()const override{return {id,L"测试翻译源 "+id,true};}
    void Translate(std::wstring,std::shared_ptr<capture::Cancellation>,Completion done)override{done({L"翻译源待配置",{}});}
};
struct CountingProvider:translation::TranslationProvider{
    int calls{};std::wstring last;
    translation::ProviderInfo Info()const override{return {L"selection-test",L"测试翻译源",true};}
    void Translate(std::wstring text,std::shared_ptr<capture::Cancellation>,Completion done)override{++calls;last=std::move(text);done({L"测试结果",{}});}
};
void NotepadPlusCase(const std::filesystem::path& output,const std::filesystem::path& executable){
    Require(std::filesystem::exists(executable),"Notepad++ executable unavailable");
    std::filesystem::create_directories(output/L"npp-config");
    const auto source=std::filesystem::absolute(output/L"selection-fixture.txt");
    const std::wstring text=L"更新测试证书：\r\n1. 下载测试文件，放入本地测试目录。\r\n2. docker exec example nginx -t\r\n3. docker exec example nginx -s reload\r\n4. 查看证书日期：\r\necho | openssl s_client -connect example.invalid:443\r\n中文 English 123 mixed selection";
    {std::ofstream file(source,std::ios::binary);const wchar_t bom=0xfeff;file.write(reinterpret_cast<const char*>(&bom),2);file.write(reinterpret_cast<const char*>(text.data()),std::streamsize(text.size()*2));}
    std::wstring command=L"\""+executable.wstring()+L"\" -multiInst -nosession -noPlugin -settingsDir=\""+std::filesystem::absolute(output/L"npp-config").wstring()+L"\" \""+source.wstring()+L"\"";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)!=FALSE,"launch isolated Notepad++");CloseHandle(process.hThread);
    struct Cleanup{HANDLE process;HWND window{};~Cleanup(){if(window)PostMessageW(window,WM_CLOSE,0,0);CloseHandle(process);}} cleanup{process.hProcess};
    struct Find{DWORD pid;HWND window{};} found{process.dwProcessId};
    Require(Until([&]{EnumWindows([](HWND w,LPARAM l)->BOOL{auto& f=*reinterpret_cast<Find*>(l);DWORD pid{};GetWindowThreadProcessId(w,&pid);wchar_t cls[80]{};GetClassNameW(w,cls,80);if(pid==f.pid&&wcscmp(cls,L"Notepad++")==0&&IsWindowVisible(w)){f.window=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&found));return found.window!=nullptr;},5000),"isolated Notepad++ window missing");cleanup.window=found.window;
    translation::TranslationController controller;auto provider=std::make_shared<CountingProvider>();controller.SetProviders({provider});
    HWND host=Host(L"PcTool.NppSelectionTest");service=&controller;controller.Configure(host,true,[]{return true;},false);
    auto key=[](WORD code,bool up=false){INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=code;i.ki.dwFlags=up?KEYEVENTF_KEYUP:0;Require(SendInput(1,&i,sizeof(i))==1,"selection key injection");};
    HWND scintilla{};EnumChildWindows(found.window,[](HWND w,LPARAM p)->BOOL{wchar_t cls[80]{};GetClassNameW(w,cls,80);if(wcscmp(cls,L"Scintilla")==0&&IsWindowVisible(w)){*reinterpret_cast<HWND*>(p)=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&scintilla));Require(scintilla!=nullptr,"Notepad++ editor missing");
    struct ReleaseKeys{~ReleaseKeys(){INPUT i[3]{};const WORD codes[]{'E',VK_MENU,VK_CONTROL};for(int j=0;j<3;++j){i[j].type=INPUT_KEYBOARD;i[j].ki.wVk=codes[j];i[j].ki.dwFlags=KEYEVENTF_KEYUP;}SendInput(3,i,sizeof(INPUT));}} release;
    for(int trial=0;trial<15;++trial){
        const WORD trigger=trial<12?'E':'R';
        if(trial==12){auto bindings=hotkeys::Defaults;bindings[4]={MOD_ALT,'R'};controller.Configure(host,true,[]{return true;},false,bindings);}
        SetWindowPos(found.window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);RECT r{};GetWindowRect(scintilla,&r);SetCursorPos(r.left+100,r.top+30);INPUT click[2]{};for(auto& i:click)i.type=INPUT_MOUSE;click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,click,sizeof(INPUT));Pump(40);SetWindowPos(found.window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        key(VK_CONTROL);key('A');key('A',true);key(VK_CONTROL,true);Pump(60);
        Snapshot(found.window,output/L"npp-source.png");std::cout<<"Npp text bytes="<<SendMessageW(scintilla,2183,0,0)<<" selected="<<SendMessageW(scintilla,2143,0,0)<<","<<SendMessageW(scintilla,2145,0,0)<<"\n";
        Require(SendMessageW(scintilla,2145,0,0)>SendMessageW(scintilla,2143,0,0),"Notepad++ fixture text is not selected");
        const int before=provider->calls;
        key(VK_MENU);key(trigger);
        if(trial%3){Pump(150);Require(provider->calls==before,"copy dispatched while trigger still held");if(trial%3==2){key(trigger);Pump(60);}}
        if(trial%2){key(VK_MENU,true);key(trigger,true);}else{key(trigger,true);key(VK_MENU,true);}
        const bool copied=Until([&]{return provider->calls>before;},1800);
        HWND popup=OwnWindow(L"PcTool.Translation");if(popup)Snapshot(popup,output/(L"npp-trial-"+std::to_wstring(trial)+L".png"));
        std::cout<<"Notepad++ trial "<<trial<<" submitted="<<copied<<"\n";
        Require(copied,"Notepad++ selected text did not submit");Require(provider->calls==before+1&&provider->last==text,"Notepad++ text incomplete or duplicate submission");
        Require(Status(popup).empty()&&IsWindowVisible(popup),"Notepad++ successful copy retained warning or hidden window");
    }
    for(int trial=0;trial<10;++trial){
        const int before=provider->calls;key(VK_MENU);key('R');key('R',true);key(VK_MENU,true);
        Require(Until([&]{return provider->calls>before;}),"Notepad++ repeat from translation popup failed");
        Require(provider->calls==before+1&&provider->last==text,"Notepad++ repeat reused wrong source or submitted twice");
    }
    controller.Configure(host,false,[]{return true;},false);
    for(int trial=0;trial<10;++trial){
        const int before=provider->calls;
        if(trial<3){SetForegroundWindow(found.window);Pump(60);TranslationMenu(host,2);Require(menuSource.window==found.window&&menuSource.process==process.dwProcessId,"menu source tracker lost Notepad++ identity");}
        else controller.Invoke(translation::Entry::Selection,true,{found.window,process.dwProcessId});
        Require(Until([&]{return provider->calls>before;}),"menu selection unavailable with hotkeys disabled");
        Require(provider->calls==before+1&&provider->last==text,"menu selection wrong source or duplicate submission");
    }
    controller.Invoke(translation::Entry::Selection,true,{found.window,process.dwProcessId+1});
    Require(IsWindowVisible(OwnWindow(L"PcTool.Translation"))&&!Status(OwnWindow(L"PcTool.Translation")).empty(),"stale menu source missing failure feedback");
    controller.Shutdown();service=nullptr;DestroyWindow(host);
    std::cout<<"PASS Notepad++ 12 Alt+E, 3 custom Alt+R, 10 shortcut repeats, 10 disabled-hotkey menu selections and stale source rejection\n";
}
void Core(){
    auto provider=std::make_shared<DeferredProvider>();
    translation::Submission submission;submission.providers={provider,nullptr};submission.Submit(L"first");
    Require(submission.cards[1].phase==translation::ResultPhase::Unconfigured&&submission.cards[1].expanded,"unconfigured submission");
    submission.Submit(L"second");provider->callbacks[0]({L"stale",{}});Require(!submission.Poll(),"stale provider result");
    provider->callbacks[1]({L"valid",{}});Require(submission.Poll()&&submission.cards[0].text==L"valid","provider completion");
    submission.Submit(L"third");submission.Cancel();provider->callbacks[2]({L"hidden",{}});Require(!submission.Poll(),"hidden provider result");
    submission.Reset();Require(!submission.cards[0].expanded&&submission.cards[0].text.empty(),"edit clears result");
    submission.cards[0].Toggle();Require(!submission.cards[0].expanded,"empty card expanded");
    submission.cards[0].text=L" \r\n\t\u3000";submission.cards[0].Toggle();Require(!submission.cards[0].expanded,"whitespace card expanded");
    submission.cards[0].text=L"result";submission.cards[0].Toggle();Require(submission.cards[0].expanded,"nonempty card cannot expand");
    submission.cards[0].Toggle();Require(!submission.cards[0].expanded,"nonempty card cannot collapse");
    submission.Submit(L"empty response");provider->callbacks.back()({L"",{}});Require(submission.Poll()&&!submission.cards[0].expanded,"empty completion leaves blank body expanded");
    submission.Reset();
    {translation::Submission shortLived;shortLived.providers={provider};shortLived.Submit(L"destroy");}provider->callbacks.back()({L"after destruction",{}});
    for(UINT dpi:{96u,144u,192u}){
        submission.cards[0].expanded=submission.cards[1].expanded=true;
        const int line=MulDiv(20,dpi,96);translation::PopupLayout layout(MulDiv(480,dpi,96),dpi,line*2,{line,line*120},submission.cards);
        Require(layout.bodies[0].bottom<layout.headers[1].top&&layout.cards[1].bottom<layout.contentHeight,"content layout overlap");
        Require(layout.bodies[1].bottom-layout.bodies[1].top==line*120,"long result height capped");
        Require(layout.input.bottom-layout.input.top==line*2,"input height ignored");
    }
    for(int total:{0,20,100,100000}){
        translation::ScrollGeometry first(200,total,50,0,18),last(200,total,50,std::max(0,total-50),18);
        Require(first.top==0&&last.top==last.travel,"scrollbar endpoint geometry");
        Require(last.Position(last.travel)==last.maximum,"scrollbar endpoint seeking");
    }
    std::cout<<"PASS submission, unconfigured sources, late callbacks and result layout\n";
    RECT b{-800,-400,1200,900};
    for(POINT a: {POINT{-30,-20},POINT{100,100}}){auto r=translation::SelectionRect(a,{50,70},b);Require(r.left==std::min(a.x,50L)&&r.bottom==std::max(a.y,70L),"selection directions");}
    auto clipped=translation::SelectionRect({-1000,-800},{1800,1200},b);Require(EqualRect(&clipped,&b)!=FALSE,"selection bounds");
    std::cout<<"PASS selection direction and negative monitor coordinates\n";
}
}
int wmain(int argc,wchar_t** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    if(argc>1&&std::wstring(argv[1])==L"--copy-target"){
        HWND host=Host(L"PcTool.TranslationCopyTarget");HWND edit=CreateWindowExW(0,L"EDIT",L"中文取词 English 123\r\nsecond line",WS_CHILD|WS_VISIBLE|ES_MULTILINE,20,30,580,180,host,reinterpret_cast<HMENU>(101),GetModuleHandleW(nullptr),nullptr);
        SetForegroundWindow(host);SetFocus(edit);SendMessageW(edit,EM_SETSEL,0,-1);
        SetWindowSubclass(edit,[](HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR)->LRESULT{
            if(m==WM_APP+4){if(OpenClipboard(w))SetTimer(w,904,150,nullptr);return 0;}
            if(m==WM_TIMER&&wp==904){KillTimer(w,904);CloseClipboard();return 0;}
            if(m==WM_APP+3){SetPropW(w,L"DelayedCopyFixture",reinterpret_cast<HANDLE>(wp));return 0;}
            if(m==WM_TIMER&&wp==903){KillTimer(w,903);SendMessageW(w,WM_COPY,0,0);return 0;}
            if(m==WM_KEYDOWN&&wp=='C'&&(GetKeyState(VK_CONTROL)&0x8000)&&GetPropW(w,L"DelayedCopyFixture")){SetTimer(w,903,180,nullptr);return 0;}
            if(m==WM_APP+2){SetPropW(w,L"CopyImageFixture",reinterpret_cast<HANDLE>(wp));return 0;}
            if(m==WM_KEYDOWN&&wp=='C'&&(GetKeyState(VK_CONTROL)&0x8000)&&GetPropW(w,L"CopyImageFixture")){capture::CopyImage(w,capture::Image(2,2));return 0;}
            return DefSubclassProc(w,m,wp,lp);
        },1,0);
        while(IsWindow(host))Pump(20);CoUninitialize();return 0;
    }
    try{
        if(argc>3&&std::wstring(argv[1])==L"npp"){
            NotepadPlusCase(argv[2],argv[3]);CoUninitialize();return 0;
        }
        if(argc>1&&(std::wstring(argv[1])==L"sources"||std::wstring(argv[1])==L"sources-ui")){
            const int result=RunTranslationSourceTests(argc>2?std::filesystem::path(argv[2]):std::filesystem::temp_directory_path()/L"pctool-source-tests",std::wstring(argv[1])==L"sources-ui");CoUninitialize();return result;
        }
        Core();
        if(argc>1&&std::wstring(argv[1])==L"shadow"){
            HWND popup=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"PcTool shadow regression",WS_POPUP,120,120,430,250,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            ShowWindow(popup,SW_SHOWNOACTIVATE);
            {translation::PopupShadow shadow;shadow.Sync(popup,96);
                auto sync=+[](HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)->LRESULT{if(m==WM_WINDOWPOSCHANGED)reinterpret_cast<translation::PopupShadow*>(data)->Sync(w,96);return DefSubclassProc(w,m,wp,lp);};
                SetWindowSubclass(popup,sync,1,reinterpret_cast<DWORD_PTR>(&shadow));
                for(int i=0;i<5;++i){SetWindowPos(popup,HWND_TOPMOST,120,120,430,250,SWP_NOACTIVATE|SWP_SHOWWINDOW);shadow.Sync(popup,96);Pump(30);Require((GetWindowLongPtrW(popup,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,"shadow synchronization demoted popup");}
                RemoveWindowSubclass(popup,sync,1);
                DestroyWindow(popup);
            }
            std::cout<<"PASS shadow preserves owner topmost band\n";CoUninitialize();return 0;
        }
        if(argc<2||std::wstring(argv[1])!=L"ui"){CoUninitialize();return 0;}
        const std::filesystem::path output=argc>2?argv[2]:L"translation-test";std::filesystem::create_directories(output);
        translation::TranslationController controller;service=&controller;HWND host=Host(L"PcTool.TranslationTestHost");
        controller.SetProviders({std::make_shared<FixtureProvider>(L"1"),std::make_shared<FixtureProvider>(L"2")});
        ClipboardHistory history;Require(history.Start(),"history listener");
        const bool qAvailable=RegisterHotKey(host,900,MOD_ALT|MOD_NOREPEAT,'Q')!=FALSE;
        if(qAvailable)UnregisterHotKey(host,900);
        const bool eAvailable=RegisterHotKey(host,901,MOD_ALT|MOD_NOREPEAT,'E')!=FALSE;if(eAvailable)UnregisterHotKey(host,901);
        controller.Configure(host,true,[]{return true;},false);
        if(eAvailable){
            ActivateSource(host);AltE();
            Require(Until([&]{auto first=OwnWindow(L"PcTool.Translation");return first&&IsWindowVisible(first)&&!IsIconic(first)&&Status(first)==L"识别内容为空";},500),"first-ever empty Alt+E did not create visible popup");
            Require((GetWindowLongPtrW(OwnWindow(L"PcTool.Translation"),GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,"passive popup lost topmost band during shadow synchronization");
            DesktopSnapshot(OwnWindow(L"PcTool.Translation"),output/L"cold-blank.png");Key(VK_ESCAPE);
        }
        // Keep the next invocation ordered behind the physical Escape input.
        // A direct controller call can overtake queued keyboard delivery.
        if(qAvailable){
            INPUT keys[4]{};for(auto& key:keys)key.type=INPUT_KEYBOARD;
            keys[0].ki.wVk=keys[3].ki.wVk=VK_MENU;keys[1].ki.wVk=keys[2].ki.wVk='Q';
            keys[2].ki.dwFlags=keys[3].ki.dwFlags=KEYEVENTF_KEYUP;
            Require(SendInput(4,keys,sizeof(INPUT))==4,"Alt+Q injection");
            Require(Until([]{auto w=OwnWindow(L"PcTool.Translation");return w&&IsWindowVisible(w)&&GetForegroundWindow()==w;}),"manual hotkey after Escape");
        }else controller.HandleHotkey(translation::ManualHotkey);
        Pump(150);
        HWND popup=OwnWindow(L"PcTool.Translation"),edit=Child(popup,1101);Require(popup&&edit&&IsWindowVisible(popup),"manual entry");
        RECT initial{},work{};GetWindowRect(popup,&initial);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(popup,MONITOR_DEFAULTTONEAREST),&monitor);work=monitor.rcWork;
        Require(initial.right-initial.left==MulDiv(430,GetDpiForWindow(popup),96),"reference default width");Require(abs(initial.left+initial.right-work.left-work.right)<=1&&abs(initial.top+initial.bottom-work.top-work.bottom)<=1,"popup not centered");
        Require(!(GetWindowLongPtrW(popup,GWL_STYLE)&(WS_THICKFRAME|WS_BORDER|WS_DLGFRAME)),"native frame still enabled");
        SetCursorPos(initial.left+100,initial.top+20);SendMessageW(popup,WM_NCLBUTTONDOWN,HTCAPTION,0);Require(GetCapture()==popup,"custom drag capture");SetCursorPos(initial.left+150,initial.top+55);SendMessageW(popup,WM_MOUSEMOVE,MK_LBUTTON,0);RECT moved{};GetWindowRect(popup,&moved);Require(moved.left==initial.left+50&&moved.top==initial.top+35,"custom drag movement");DesktopSnapshot(popup,output/L"frame-drag.png");SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);RECT restored{};GetWindowRect(popup,&restored);Require(EqualRect(&initial,&restored)&&GetCapture()!=popup,"drag cancel restore");
        SetCursorPos(initial.right-1,initial.bottom-1);SendMessageW(popup,WM_NCLBUTTONDOWN,HTBOTTOMRIGHT,0);SetCursorPos(initial.right+39,initial.bottom+29);SendMessageW(popup,WM_MOUSEMOVE,MK_LBUTTON,0);SendMessageW(popup,WM_LBUTTONUP,0,0);GetWindowRect(popup,&moved);Require(moved.right-moved.left==initial.right-initial.left+40&&moved.bottom-moved.top==initial.bottom-initial.top+30,"borderless resize");
        controller.HandleHotkey(translation::ManualHotkey);GetWindowRect(popup,&restored);Require(restored.right-restored.left==initial.right-initial.left,"default width not reset");
        Command(popup,1102);ActivateSource(host);Pump(40);Require(IsWindowVisible(popup),"pinned inactive hidden");DesktopSnapshot(popup,output/L"frame-inactive.png");ActivateSource(popup);Command(popup,1102);
        int twoRows{},tenRows{};
        for(int count:{0,1,2,10,11}){
            std::wstring input;for(int row=0;row<count;++row){if(row)input+=L"\r\n";input+=L"line";}
            SetWindowTextW(edit,input.c_str());RECT r{};GetWindowRect(edit,&r);const int height=r.bottom-r.top;
            if(count==0)twoRows=height;if(count<=2)Require(height==twoRows,"input minimum two rows");if(count==10)tenRows=height;if(count==11)Require(height==tenRows,"input did not cap at ten rows");
            Require(bool(IsWindowVisible(Child(popup,1112)))==(count>10),"input scroll threshold");
        }
        std::wstring url=L"https://example.com/";for(int i=0;i<100;++i)url+=L"abcdefgh";SetWindowTextW(edit,url.c_str());Require(SendMessageW(edit,EM_GETLINECOUNT,0,0)>10,"long URL not wrapped");
        SendMessageW(edit,EM_SETSEL,3,10);RECT beforeRect{};GetWindowRect(popup,&beforeRect);SetWindowPos(popup,nullptr,0,0,beforeRect.right-beforeRect.left-30,beforeRect.bottom-beforeRect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        DWORD startSelection{},endSelection{};SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&startSelection),reinterpret_cast<LPARAM>(&endSelection));Require(startSelection==3&&endSelection==10,"resize changed input selection");
        SetWindowTextW(edit,L"short");RECT compact{};GetWindowRect(popup,&compact);SendMessageW(popup,WM_ENTERSIZEMOVE,0,0);SetWindowPos(popup,nullptr,0,0,compact.right-compact.left,compact.bottom-compact.top+80,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);SendMessageW(popup,WM_EXITSIZEMOVE,0,0);RECT manual{};GetWindowRect(popup,&manual);SetWindowTextW(edit,L"still short");RECT kept{};GetWindowRect(popup,&kept);Require(kept.bottom-kept.top==manual.bottom-manual.top,"manual height not preserved");controller.HandleHotkey(translation::ManualHotkey);GetWindowRect(popup,&kept);Require(kept.bottom-kept.top<manual.bottom-manual.top,"manual height not reset on reopen");
        SetWindowTextW(edit,L"原文测试 中文 English\r\n第二行");
        POINT textScroll{};SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&textScroll));Require(textScroll.y==0,"short input retained clipped scroll offset");
        SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);Require(!IsWindowVisible(popup),"escape hide");
        controller.HandleHotkey(translation::ManualHotkey);Pump(50);Require(capture::WindowText(edit).empty()&&!IsWindowVisible(Child(popup,1120)),"manual entry retained previous text/results");
        Command(popup,1102);SetForegroundWindow(host);Pump(80);Require(IsWindowVisible(popup),"pin retains popup");
        SetForegroundWindow(popup);Command(popup,1102);SetForegroundWindow(host);Pump(80);Require(!IsWindowVisible(popup),"unpin loses focus");
        controller.HandleHotkey(translation::ManualHotkey);Pump(50);
        for(UINT dpi:{96u,144u,192u}){
            RECT r{170,140,170+MulDiv(430,dpi,96),140+MulDiv(330,dpi,96)};SendMessageW(popup,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));Pump(70);
            RECT e{},p{};GetWindowRect(edit,&e);GetWindowRect(popup,&p);Require(e.left>=p.left&&e.right<=p.right&&e.bottom<p.bottom,"DPI editor bounds");
            SetWindowTextW(edit,L"undo fixture");SendMessageW(edit,EM_SETSEL,WPARAM(-1),LPARAM(-1));SendMessageW(edit,EM_EMPTYUNDOBUFFER,0,0);SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L" appended"));
            RECT undoBounds{};GetWindowRect(popup,&undoBounds);const UINT otherDpi=dpi==96?144:96;SendMessageW(popup,WM_DPICHANGED,MAKEWPARAM(otherDpi,otherDpi),reinterpret_cast<LPARAM>(&undoBounds));SendMessageW(popup,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&undoBounds));
            SendMessageW(edit,EM_UNDO,0,0);Require(capture::WindowText(edit)==L"undo fixture","DPI formatting broke text undo");SetWindowTextW(edit,L"原文测试 中文 English\r\n第二行");
            Snapshot(popup,output/(L"popup-"+std::to_wstring(dpi)+L".png"));
            DesktopSnapshot(popup,output/(L"shadow-"+std::to_wstring(dpi)+L".png"));
            if(dpi==96){
                const auto oldBrush=GetClassLongPtrW(host,GCLP_HBRBACKGROUND);HBRUSH dark=CreateSolidBrush(RGB(35,38,43));
                RECT hostRect{};GetWindowRect(host,&hostRect);SetWindowPos(host,nullptr,0,0,1000,850,SWP_NOZORDER|SWP_NOACTIVATE);
                SetClassLongPtrW(host,GCLP_HBRBACKGROUND,reinterpret_cast<LONG_PTR>(dark));InvalidateRect(host,nullptr,TRUE);Pump(60);DesktopSnapshot(popup,output/L"shadow-dark.png");
                SetClassLongPtrW(host,GCLP_HBRBACKGROUND,oldBrush);InvalidateRect(host,nullptr,TRUE);Pump(60);DesktopSnapshot(popup,output/L"shadow-white.png");
                SetWindowPos(host,nullptr,hostRect.left,hostRect.top,hostRect.right-hostRect.left,hostRect.bottom-hostRect.top,SWP_NOZORDER|SWP_NOACTIVATE);DeleteObject(dark);
            }
            SetWindowTextW(edit,L"");Pump(40);Snapshot(popup,output/(L"placeholder-"+std::to_wstring(dpi)+L".png"));
            RECT compactViewport{},compactEditor{};GetWindowRect(GetDlgItem(popup,1110),&compactViewport);GetWindowRect(edit,&compactEditor);
            RECT emptyBounds{};GetWindowRect(popup,&emptyBounds);
            for(int source=0;source<2;++source){
                HWND cards=GetDlgItem(popup,1110);RECT body{};GetWindowRect(Child(popup,1120+source),&body);
                POINT arrow{body.right-MulDiv(9,dpi,96),body.top-MulDiv(18,dpi,96)};ScreenToClient(cards,&arrow);
                for(int click=0;click<3;++click){SendMessageW(cards,WM_MOUSEMOVE,0,MAKELPARAM(arrow.x,arrow.y));SendMessageW(cards,WM_LBUTTONUP,0,MAKELPARAM(arrow.x,arrow.y));}
                RECT after{};GetWindowRect(popup,&after);Require(!IsWindowVisible(Child(popup,1120+source))&&EqualRect(&emptyBounds,&after),"empty arrow click expands or resizes popup");
            }
            Command(popup,1102);Snapshot(popup,output/(L"pinned-"+std::to_wstring(dpi)+L".png"));Command(popup,1102);
            std::wstring longInput;for(int i=0;i<16;++i)longInput+=L"原文换行测试 English 123 长文本滚动查看。\r\n";
            SetWindowTextW(edit,longInput.c_str());Pump(30);
            HWND inputBar=Child(popup,1112);Require(IsWindowVisible(inputBar)&&!(GetWindowLongPtrW(edit,GWL_STYLE)&WS_VSCROLL),"custom input scrollbar missing");
            RECT inputBarRect{};GetWindowRect(inputBar,&inputBarRect);Require(inputBarRect.right-inputBarRect.left==MulDiv(6,dpi,96),"input scrollbar width");
            RECT longViewport{},longEditor{},popupBounds{};GetWindowRect(GetDlgItem(popup,1110),&longViewport);GetWindowRect(edit,&longEditor);GetWindowRect(popup,&popupBounds);
            Require(longViewport.right-longViewport.left==popupBounds.right-popupBounds.left,"overall scrollbar adds a right gutter");
            Require(longEditor.right-longEditor.left==compactEditor.right-compactEditor.left,"input scrollbar visibility changes wrapping width");
            Require(popupBounds.right-inputBarRect.right==MulDiv(15,dpi,96),"input scrollbar too far from card edge");
            Snapshot(popup,output/(L"input-long-"+std::to_wstring(dpi)+L".png"));
            SendMessageW(inputBar,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,3));SendMessageW(inputBar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(2,inputBarRect.bottom-inputBarRect.top-2));SendMessageW(inputBar,WM_LBUTTONUP,0,0);
            POINT inputOffset{};SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&inputOffset));Require(inputOffset.y>0,"input thumb did not scroll text");
            SendMessageW(edit,EM_SETSEL,1,4);
            POINT wheelPoint{longEditor.left+20,longEditor.top+20};
            const LPARAM wheelLocation=MAKELPARAM(wheelPoint.x,wheelPoint.y);
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),wheelLocation);
            POINT afterWheel{};SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&afterWheel));
            Require(afterWheel.y<inputOffset.y,"input wheel up did not scroll text");
            Require(SendMessageW(inputBar,SBM_GETPOS,0,0)==afterWheel.y,"input wheel thumb out of sync");
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),wheelLocation);
            POINT afterDown{};SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&afterDown));Require(afterDown.y>afterWheel.y,"input wheel down did not scroll text");
            DWORD wheelStart{},wheelEnd{};SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&wheelStart),reinterpret_cast<LPARAM>(&wheelEnd));
            Require(wheelStart==1&&wheelEnd==4&&capture::WindowText(edit)==longInput,"input wheel changed text or selection");
            POINT zero{};SendMessageW(edit,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&zero));
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA/2)),wheelLocation);
            POINT half{};SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&half));Require(half.y==0,"partial wheel delta was rounded up");
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA/2)),wheelLocation);
            SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&half));Require(half.y>0,"partial wheel deltas were lost");
            const LPARAM trackLocation=MAKELPARAM(inputBarRect.left+2,inputBarRect.top+20);
            SendMessageW(inputBar,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),trackLocation);
            SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&afterWheel));Require(afterWheel.y<half.y,"wheel over input scrollbar routed to outer viewport");
            SendMessageW(GetDlgItem(popup,1110),WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(longEditor.left-MulDiv(5,dpi,96),longEditor.top+20));
            SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&afterDown));Require(afterDown.y>afterWheel.y,"wheel over input card padding did not scroll input");
            if(dpi==96){
                ActivateSource(popup);SetFocus(edit);SendMessageW(edit,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&zero));SetCursorPos(wheelPoint.x,wheelPoint.y);Pump(30);
                INPUT event{};event.type=INPUT_MOUSE;event.mi.dwFlags=MOUSEEVENTF_WHEEL;event.mi.mouseData=DWORD(-WHEEL_DELTA);
                Require(SendInput(1,&event,sizeof(event))==1,"inject input wheel");Pump(80);
                SendMessageW(edit,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&afterDown));Require(afterDown.y>0,"real mouse wheel did not scroll input");
                DesktopSnapshot(popup,output/L"input-native-wheel.png");
            }
            Snapshot(popup,output/(L"input-wheel-"+std::to_wstring(dpi)+L".png"));
            // Both levels overflow: input boundaries must never scroll the outer
            // viewport. Only moving the pointer outside the card changes layers.
            SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);SendMessageW(edit,WM_CHAR,L'\r',0);Pump(40);
            std::wstring wheelResult;for(int row=0;row<80;++row)wheelResult+=L"滚轮边界结果 English\r\n";
            SetWindowTextW(Child(popup,1120),wheelResult.c_str());Pump(30);
            HWND wheelOuter=GetDlgItem(popup,1111);Require(IsWindowVisible(wheelOuter),"wheel boundary fixture has no outer overflow");
            SendMessageW(popup,WM_VSCROLL,SB_TOP,reinterpret_cast<LPARAM>(wheelOuter));
            SCROLLINFO inputRange{sizeof(inputRange),SIF_ALL};SendMessageW(inputBar,SBM_GETSCROLLINFO,0,reinterpret_cast<LPARAM>(&inputRange));
            POINT bottom{0,inputRange.nMax-int(inputRange.nPage)+1};SendMessageW(edit,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&bottom));
            RECT wheelEditor{};GetWindowRect(edit,&wheelEditor);
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(wheelEditor.left+20,wheelEditor.top+20));
            Require(SendMessageW(wheelOuter,SBM_GETPOS,0,0)==0,"input bottom leaked wheel to outer viewport");
            SendMessageW(popup,WM_VSCROLL,SB_TOP,reinterpret_cast<LPARAM>(wheelOuter));SendMessageW(edit,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&zero));
            GetWindowRect(edit,&wheelEditor);
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(wheelEditor.left+20,wheelEditor.top+20));
            Require(SendMessageW(wheelOuter,SBM_GETPOS,0,0)==0,"outer moved before input reached boundary");
            SendMessageW(edit,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&zero));SendMessageW(popup,WM_VSCROLL,SB_LINEDOWN,reinterpret_cast<LPARAM>(wheelOuter));GetWindowRect(edit,&wheelEditor);
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(wheelEditor.left+20,wheelEditor.top+MulDiv(40,dpi,96)));
            Require(SendMessageW(wheelOuter,SBM_GETPOS,0,0)==MulDiv(21,dpi,96),"input top leaked wheel to outer viewport");
            RECT outerTrack{};GetWindowRect(wheelOuter,&outerTrack);
            SendMessageW(wheelOuter,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(outerTrack.left+1,outerTrack.top+20));
            Require(SendMessageW(wheelOuter,SBM_GETPOS,0,0)>MulDiv(21,dpi,96),"wheel outside input did not move outer viewport");
            SetWindowTextW(edit,L"原文测试 中文 English\r\n第二行");Pump(30);
            POINTL first{},second{};SendMessageW(edit,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&first),0);SendMessageW(edit,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&second),SendMessageW(edit,EM_LINEINDEX,1,0));
            Require(first.y>=0&&abs(second.y-first.y-MulDiv(21,dpi,96))<=1,"text line spacing or clipped first line");
            SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);SendMessageW(edit,WM_CHAR,L'\r',0);Pump(60);
            Snapshot(popup,output/(L"expanded-"+std::to_wstring(dpi)+L".png"));
            // Repeated card toggles must not move the arrow under the pointer
            // or disturb the input selection while child windows are resized.
            HWND cards=GetDlgItem(popup,1110);
            RECT anchor{},bodyRect{};GetWindowRect(popup,&anchor);GetWindowRect(Child(popup,1120),&bodyRect);
            POINT arrow{bodyRect.right-MulDiv(9,dpi,96),bodyRect.top-MulDiv(18,dpi,96)};ScreenToClient(cards,&arrow);
            POINT arrowScreen=arrow;ClientToScreen(cards,&arrowScreen);SetCursorPos(arrowScreen.x,arrowScreen.y);Pump(30);
            SendMessageW(edit,EM_SETSEL,1,4);
            RECT inputScreen{};GetWindowRect(edit,&inputScreen);
            const POINT probe{inputScreen.right-MulDiv(8,dpi,96),inputScreen.top+MulDiv(4,dpi,96)};
            std::atomic<bool> sampling{true};std::atomic<int> lostFrames{};std::atomic<int> totalFrames{};
            std::thread sampler([&]{
                HDC screen=GetDC(nullptr);std::ofstream log(output/(L"toggle-samples-"+std::to_wstring(dpi)+L".csv"));log<<"tick,color,root,popup,visible,topmost\n";
                while(sampling){
                    const auto color=GetPixel(screen,probe.x,probe.y);const auto root=GetAncestor(WindowFromPoint(probe),GA_ROOT);
                    if(color!=translation::PopupStyle::InputBackground){++lostFrames;log<<GetTickCount64()<<','<<color<<','<<root<<','<<popup<<','<<IsWindowVisible(popup)<<','<<bool(GetWindowLongPtrW(popup,GWL_EXSTYLE)&WS_EX_TOPMOST)<<'\n';}
                    ++totalFrames;Sleep(1);
                }ReleaseDC(nullptr,screen);
            });
            struct SamplingGuard{std::atomic<bool>& running;std::thread& worker;~SamplingGuard(){running=false;if(worker.joinable())worker.join();}} samplingGuard{sampling,sampler};
            for(int toggle=0;toggle<30;++toggle){
                SendMessageW(cards,WM_MOUSEMOVE,0,MAKELPARAM(arrow.x,arrow.y));
                SendMessageW(cards,WM_LBUTTONUP,0,MAKELPARAM(arrow.x,arrow.y));Pump(20);
                RECT current{};GetWindowRect(popup,&current);Require(current.left==anchor.left&&current.top==anchor.top,"card toggle recentered popup");
                Require(bool(IsWindowVisible(Child(popup,1120)))==(toggle%2==1),"card toggle visibility");
                DWORD a{},b{};SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));Require(a==1&&b==4,"card toggle lost input selection");
            }
            sampling=false;sampler.join();
            std::cout<<"Card frame sampling DPI "<<dpi<<": "<<lostFrames<<" changed / "<<totalFrames<<" samples\n";
            Require(lostFrames==0,"card toggle exposed background in unchanged input area");
            SendMessageW(cards,WM_MOUSEMOVE,0,MAKELPARAM(arrow.x,arrow.y));
            Snapshot(popup,output/(L"card-hover-"+std::to_wstring(dpi)+L".png"));
            SendMessageW(cards,WM_MOUSELEAVE,0,0);Pump(20);
            Snapshot(popup,output/(L"card-normal-"+std::to_wstring(dpi)+L".png"));
            HWND body=Child(popup,1120),bar=GetDlgItem(popup,1111);std::wstring bodyText;for(int row=0;row<80;++row)bodyText+=L"只读长结果 Mixed English line "+std::to_wstring(row)+L"\r\n";
            SetWindowTextW(body,bodyText.c_str());Pump(40);Require(IsWindowVisible(bar),"long content missing overall scrollbar");
            RECT shortInput{};GetWindowRect(edit,&shortInput);const auto shortBefore=SendMessageW(bar,SBM_GETPOS,0,0);
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(shortInput.left+10,shortInput.top+10));
            Require(SendMessageW(bar,SBM_GETPOS,0,0)==shortBefore,"short input leaked wheel to outer viewport");
            RECT outerBar{},fullView{},outerWindow{};GetWindowRect(bar,&outerBar);GetWindowRect(GetDlgItem(popup,1110),&fullView);GetWindowRect(popup,&outerWindow);
            Require(outerWindow.right-outerBar.right==MulDiv(1,dpi,96)&&fullView.right==outerWindow.right,"outer scrollbar not at window edge");
            Snapshot(popup,output/(L"long-top-"+std::to_wstring(dpi)+L".png"));
            if(dpi==96){
                ActivateSource(popup);RECT track{};GetWindowRect(bar,&track);SCROLLINFO barInfo{sizeof(barInfo),SIF_ALL};SendMessageW(bar,SBM_GETSCROLLINFO,0,reinterpret_cast<LPARAM>(&barInfo));translation::ScrollGeometry shape(track.bottom-track.top,barInfo.nMax+1,barInfo.nPage,barInfo.nPos,MulDiv(18,dpi,96));const int thickness=0;const int thumbCenter=shape.top+shape.length/2;
                Require(WindowFromPoint({(track.left+track.right)/2,track.top+thumbCenter})==bar,"resize edge intercepts scrollbar");
                auto drag=std::async(std::launch::async,[track,thickness,thumbCenter]{INPUT release{};release.type=INPUT_MOUSE;release.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&release,sizeof(release));Sleep(40);SetCursorPos((track.left+track.right)/2,track.top+thumbCenter);INPUT event{};event.type=INPUT_MOUSE;event.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;SendInput(1,&event,sizeof(event));Sleep(200);SetCursorPos((track.left+track.right)/2,track.bottom-thickness-5);Sleep(200);event.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&event,sizeof(event));});Pump(650);drag.get();Require(int(SendMessageW(bar,SBM_GETPOS,0,0))>0,"native scrollbar thumb drag");
                INPUT click{};click.type=INPUT_MOUSE;
                SendMessageW(popup,WM_VSCROLL,SB_TOP,reinterpret_cast<LPARAM>(bar));RECT textRect{},clipRect{};GetWindowRect(body,&textRect);GetWindowRect(GetDlgItem(popup,1110),&clipRect);
                SetCursorPos(textRect.left+12,textRect.top+8);click.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;SendInput(1,&click,sizeof(click));Pump(25);SetCursorPos(textRect.left+120,clipRect.bottom+8);Pump(180);click.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&click,sizeof(click));Pump(25);Require(int(SendMessageW(bar,SBM_GETPOS,0,0))>0,"drag selection edge autoscroll");
            }
            SendMessageW(popup,WM_VSCROLL,SB_BOTTOM,reinterpret_cast<LPARAM>(bar));Pump(40);Snapshot(popup,output/(L"long-bottom-"+std::to_wstring(dpi)+L".png"));
            RECT last{},view{};GetWindowRect(Child(popup,1121),&last);GetWindowRect(GetDlgItem(popup,1110),&view);Require(last.bottom<=view.bottom&&last.bottom>view.top,"last result cannot be reached");
            SendMessageW(popup,WM_VSCROLL,SB_TOP,reinterpret_cast<LPARAM>(bar));SendMessageW(body,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),0);Require(int(SendMessageW(bar,SBM_GETPOS,0,0))>0,"result wheel not forwarded");
            const auto scrollBefore=int(SendMessageW(bar,SBM_GETPOS,0,0));RECT preserve{};GetWindowRect(popup,&preserve);SetWindowPos(popup,nullptr,0,0,preserve.right-preserve.left+5,preserve.bottom-preserve.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);Require(int(SendMessageW(bar,SBM_GETPOS,0,0))==scrollBefore,"reflow lost reading position");
            SetWindowTextW(body,L"翻译源待配置");Require(!IsWindowVisible(bar),"short content failed to shrink");
            SetWindowTextW(edit,L"原文测试 中文 English\r\n第二行");
        }
        Require(!Child(popup,1104),"copy button still exists");SendMessageW(edit,EM_SETSEL,0,-1);SendMessageW(edit,WM_COPY,0,0);Require(OpenClipboard(nullptr)!=FALSE,"clipboard open");auto* text=static_cast<const wchar_t*>(GlobalLock(GetClipboardData(CF_UNICODETEXT)));Require(text&&std::wstring(text).find(L"原文测试")!=std::wstring::npos,"copy input");GlobalUnlock(GetClipboardData(CF_UNICODETEXT));CloseClipboard();
        auto before=capture::WindowText(edit);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);SendMessageW(edit,WM_CHAR,L'\r',0);Pump(50);
        HWND result=Child(popup,1120);Require(IsWindowVisible(result)&&capture::WindowText(result)==L"翻译源待配置","enter expands sources");Require(capture::WindowText(edit)==before,"enter inserted newline");
        Require(!Child(popup,1103)&&!Child(popup,1130)&&!Child(popup,1131),"removed playback controls still exist");
        // Test-only content goes into the real read-only result control, never a production provider.
        const std::wstring fixture=L"只读结果 test\r\nsecond line";SetWindowTextW(result,fixture.c_str());
        Pump(40);Snapshot(popup,output/L"before-select.png");RECT selectRect{};GetWindowRect(result,&selectRect);SetCursorPos(selectRect.left+4,selectRect.top+20);
        INPUT selectMouse{};selectMouse.type=INPUT_MOUSE;selectMouse.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;SendInput(1,&selectMouse,sizeof(INPUT));Pump(30);SetCursorPos(selectRect.left+140,selectRect.top+20);Pump(30);selectMouse.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&selectMouse,sizeof(INPUT));Pump(30);DWORD selStart{},selEnd{};SendMessageW(result,EM_GETSEL,reinterpret_cast<WPARAM>(&selStart),reinterpret_cast<LPARAM>(&selEnd));Require(selStart!=selEnd,"mouse result selection");
        SendMessageW(result,EM_SETSEL,0,-1);SendMessageW(result,WM_CHAR,L'X',0);SendMessageW(result,WM_PASTE,0,0);SendMessageW(result,WM_CLEAR,0,0);Require(capture::WindowText(result)==fixture,"result is editable");
        BYTE ctrlKeys[256]{};GetKeyboardState(ctrlKeys);const BYTE controlBefore=ctrlKeys[VK_CONTROL];ctrlKeys[VK_CONTROL]=0x80;SetKeyboardState(ctrlKeys);SendMessageW(result,EM_SETSEL,0,0);SendMessageW(result,WM_KEYDOWN,'A',0);ctrlKeys[VK_CONTROL]=controlBefore;SetKeyboardState(ctrlKeys);
        SendMessageW(result,WM_COPY,0,0);Require(OpenClipboard(nullptr)!=FALSE,"result clipboard");auto copiedText=static_cast<const wchar_t*>(GlobalLock(GetClipboardData(CF_UNICODETEXT)));Require(copiedText&&std::wstring(copiedText)==fixture,"result selection copy");GlobalUnlock(GetClipboardData(CF_UNICODETEXT));CloseClipboard();
        std::wstring longResult;for(int i=0;i<120;++i)longResult+=L"测试文本 line "+std::to_wstring(i)+L"\r\n";SetWindowTextW(result,longResult.c_str());Require(IsWindowVisible(GetDlgItem(popup,1111)),"overall scrollbar absent");
        SendMessageW(popup,WM_VSCROLL,SB_BOTTOM,reinterpret_cast<LPARAM>(GetDlgItem(popup,1111)));Require(int(SendMessageW(GetDlgItem(popup,1111),SBM_GETPOS,0,0))>0,"overall result scroll");
        Require(!(GetWindowLongPtrW(result,GWL_STYLE)&WS_VSCROLL),"nested result scrollbar");
        SendMessageW(popup,WM_VSCROLL,SB_TOP,reinterpret_cast<LPARAM>(GetDlgItem(popup,1111)));
        HWND cardView=GetDlgItem(popup,1110);RECT resultRect{};GetWindowRect(result,&resultRect);
        POINT titlePoint{resultRect.left+4,resultRect.top-15};ScreenToClient(cardView,&titlePoint);
        SendMessageW(cardView,WM_LBUTTONUP,0,MAKELPARAM(titlePoint.x,titlePoint.y));Require(IsWindowVisible(result),"title click collapsed source");
        POINT arrowPoint{resultRect.right-MulDiv(9,GetDpiForWindow(popup),96),resultRect.top-MulDiv(18,GetDpiForWindow(popup),96)};ScreenToClient(cardView,&arrowPoint);
        SendMessageW(cardView,WM_LBUTTONUP,0,MAKELPARAM(arrowPoint.x,arrowPoint.y));Require(!IsWindowVisible(result)&&IsWindowVisible(Child(popup,1121)),"independent arrow collapse");
        SetWindowTextW(edit,before.c_str());Require(!IsWindowVisible(result),"edit resets cards");
        SendMessageW(edit,WM_IME_STARTCOMPOSITION,0,0);SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);SendMessageW(edit,WM_IME_ENDCOMPOSITION,0,0);SendMessageW(edit,WM_CHAR,L'\r',0);Require(!IsWindowVisible(result),"IME enter submitted");
        BYTE keyboard[256]{};GetKeyboardState(keyboard);auto shift=keyboard[VK_SHIFT];keyboard[VK_SHIFT]=0x80;SetKeyboardState(keyboard);SendMessageW(edit,EM_SETSEL,WPARAM(-1),LPARAM(-1));SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);SendMessageW(edit,WM_CHAR,L'\r',0);keyboard[VK_SHIFT]=shift;SetKeyboardState(keyboard);Require(capture::WindowText(edit).size()>before.size()&&!IsWindowVisible(result),"Shift Enter newline");
        HWND shadow=OwnWindow(L"PcTool.TranslationShadow");Require(shadow&&IsWindowVisible(shadow),"shadow missing");Require((GetWindowLongPtrW(shadow,GWL_EXSTYLE)&WS_EX_TRANSPARENT)!=0,"shadow intercepts mouse");
        SetWindowTextW(edit,before.c_str());
        SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);std::wstring cmd=L"\""+std::wstring(exe)+L"\" --copy-target";STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};
        Require(CreateProcessW(exe,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&process)!=FALSE,"copy target process");CloseHandle(process.hThread);
        HWND target{};Require(Until([&]{target=FindWindowW(L"PcTool.TranslationCopyTarget",nullptr);return target!=nullptr;}),"copy target window");
        Require(capture::CopyText(host,L"original clipboard before TTime selection"),"seed clipboard");Pump(100);ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);Pump(50);Require(GetForegroundWindow()==target,"test source foreground");controller.HandleHotkey(translation::SelectionHotkey);
        const bool copied=Until([&]{return capture::WindowText(edit).find(L"中文取词")!=std::wstring::npos;});
        Snapshot(popup,output/L"selection-result.png");Require(copied,"cross-process Ctrl+C selection");Require(ClipboardText()==capture::WindowText(edit),"successful selection keeps copied text");Require(Status(popup).empty(),"received text clears warning");Require(IsWindowVisible(Child(popup,1120)),"selection did not auto submit");
        int hideCount=0;
        auto countHides=[](HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)->LRESULT{if(m==WM_SHOWWINDOW&&!wp)++*reinterpret_cast<int*>(data);return DefSubclassProc(w,m,wp,lp);};SetWindowSubclass(popup,countHides,77,reinterpret_cast<DWORD_PTR>(&hideCount));
        for(int repeat=0;repeat<10;++repeat){
            Require(GetForegroundWindow()==popup,"repeat starts with translation foreground");controller.HandleHotkey(translation::SelectionHotkey);
            Require(IsWindowVisible(popup),"repeat shortcut keeps popup visible");
            Require(Until([&]{Require(IsWindowVisible(popup),"repeat shortcut hid popup");return capture::WindowText(edit).find(L"中文取词")!=std::wstring::npos;}),"repeat uses original selection");
        }
        Require(hideCount==0,"repeated selection has hide/show cycle");RemoveWindowSubclass(popup,countHides,77);
        history.Show();Pump(100);Snapshot(FindWindowW(L"PcTool.ClipboardHistory",nullptr),output/L"clipboard-history.png");
        HWND childEdit=GetDlgItem(target,101);SendMessageW(childEdit,EM_SETSEL,0,0);SetForegroundWindow(target);Pump(50);const auto previous=capture::WindowText(edit);
        const auto emptyCopyStart=GetTickCount64();controller.HandleHotkey(translation::SelectionHotkey);controller.HandleHotkey(translation::SelectionHotkey);Require(IsWindowVisible(popup),"popup is visible before polling copy result");
        Require(Until([&]{return IsWindowVisible(popup);},450),"immediate empty selection popup latency");
        std::cout<<"Empty selection popup latency: "<<(GetTickCount64()-emptyCopyStart)<<" ms\n";
        PostMessageW(popup,WM_APP+79,0,0);Pump(30);Require(IsWindowVisible(popup),"stale deactivation hid new invocation");
        Require(capture::WindowText(edit).empty(),"stale clipboard not imported");Require(Status(popup)==L"识别内容为空","empty warning must be immediate");DesktopSnapshot(popup,output/L"empty-visible-desktop.png");Require(ClipboardText().empty(),"empty selection leaves clipboard empty");SetForegroundWindow(popup);SetFocus(edit);Pump(70);Require(Status(popup)==L"识别内容为空","focusing empty input must not report cancellation");
        ActivateSource(target);SendMessageW(childEdit,WM_APP+2,1,0);controller.HandleHotkey(translation::SelectionHotkey);
        Require(Until([&]{return Status(popup).find(L"识别内容为空")!=std::wstring::npos;}),"non-text clipboard rejected");Require(capture::WindowText(edit).empty()&&IsWindowVisible(popup),"image copied into input");Require(IsWindowVisible(GetDlgItem(popup,1201)),"empty warning visible");Require(!IsWindowVisible(Child(popup,1120)),"empty sources collapsed");Snapshot(popup,output/L"empty-warning.png");
        for(UINT warningDpi:{96u,144u,192u}){RECT bounds{170,140,170+MulDiv(430,warningDpi,96),140+MulDiv(330,warningDpi,96)};SendMessageW(popup,WM_DPICHANGED,MAKEWPARAM(warningDpi,warningDpi),reinterpret_cast<LPARAM>(&bounds));Pump(50);Snapshot(popup,output/(L"empty-warning-"+std::to_wstring(warningDpi)+L".png"));}
        Pump(3200);Require(!IsWindowVisible(GetDlgItem(popup,1201)),"warning expired");SendMessageW(childEdit,WM_APP+2,0,0);
        const std::wstring sourceBeforeWhitespace=L"中文取词 English 123\r\nsecond line";SendMessageW(childEdit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L" \t\r\n　"));ActivateSource(target);SendMessageW(childEdit,EM_SETSEL,0,-1);controller.HandleHotkey(translation::SelectionHotkey);
        Require(Until([&]{return Status(popup).find(L"识别内容为空")!=std::wstring::npos;}),"whitespace selection rejected");Require(IsWindowVisible(popup)&&capture::WindowText(edit).empty(),"whitespace must open empty translation");SendMessageW(childEdit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(sourceBeforeWhitespace.c_str()));
        ActivateSource(target);INPUT modifier{};modifier.type=INPUT_KEYBOARD;modifier.ki.wVk=VK_SHIFT;SendInput(1,&modifier,sizeof(modifier));
        controller.HandleHotkey(translation::SelectionHotkey);ActivateSource(host);modifier.ki.dwFlags=KEYEVENTF_KEYUP;SendInput(1,&modifier,sizeof(modifier));
        Require(Until([&]{return !IsWindowVisible(popup);}),"foreground change canceled copy");Require(capture::WindowText(edit).empty()&&!IsWindowVisible(popup),"foreground race imported text");
        Require(ClipboardText()!=L"original clipboard before TTime selection","cancel must not restore old clipboard");
        ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);controller.HandleHotkey(translation::SelectionHotkey);
        ActivateSource(host);Require(Until([&]{return !IsWindowVisible(popup);}),"cancel during copy");
        Require(ClipboardText()!=L"original clipboard before TTime selection","cancel must not restore old clipboard");Require(!(GetAsyncKeyState(VK_CONTROL)&0x8000)&&!(GetAsyncKeyState('C')&0x8000),"cancel releases synthetic keys");
        ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);Require(IsWindowVisible(popup),"blank surface opens popup immediately");
        Require(Until([&]{return Status(popup)==L"识别内容为空"&&IsWindowVisible(popup);}),"blank surface displays empty warning");
        controller.HandleHotkey(translation::SelectionHotkey);Require(IsWindowVisible(popup)&&Status(popup)==L"识别内容为空","repeat empty surface remains open");
        ShowWindow(popup,SW_MINIMIZE);Pump(40);ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);Pump(40);
        Require(IsWindowVisible(popup)&&!IsIconic(popup),"blank Alt+E did not restore minimized popup");
        // A transient activation notification must not turn passive presentation
        // into a request to dismiss the empty popup when the source still has focus.
        SendMessageW(popup,WM_ACTIVATE,WA_ACTIVE,reinterpret_cast<LPARAM>(host));SendMessageW(popup,WM_ACTIVATE,WA_INACTIVE,reinterpret_cast<LPARAM>(host));Pump(60);
        Require(IsWindowVisible(popup),"transient activation hid blank Alt+E popup");
        DesktopSnapshot(popup,output/L"blank-restored.png");
        if(eAvailable){
            for(int attempt=0;attempt<12;++attempt){
                switch(attempt%4){case 0:ShowWindow(popup,SW_HIDE);break;case 1:ShowWindow(popup,SW_MINIMIZE);break;case 2:SendMessageW(popup,WM_CLOSE,0,0);break;default:break;}
                ActivateSource(host);const auto sequence=GetClipboardSequenceNumber();AltE();
                Require(Until([&]{return GetClipboardSequenceNumber()!=sequence&&IsWindowVisible(popup)&&!IsIconic(popup)&&Status(popup)==L"识别内容为空";},500),"blank Alt+E visibility state matrix");
                SendMessageW(popup,WM_ACTIVATE,WA_ACTIVE,reinterpret_cast<LPARAM>(host));SendMessageW(popup,WM_ACTIVATE,WA_INACTIVE,reinterpret_cast<LPARAM>(host));Pump(40);
                RECT bounds{};GetWindowRect(popup,&bounds);Require(IsWindowVisible(popup)&&GetAncestor(WindowFromPoint({(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2}),GA_ROOT)==popup,"blank popup hidden or occluded after activation messages");
                Require(capture::WindowText(edit).empty(),"blank state reused previous input");
            }
            std::cout<<"PASS first-ever blank Alt+E and 12 hidden/minimized/closed/repeated state calls, including transient activation and actual z-order\n";
        }

        if(HWND desktop=GetShellWindow()){
            SetForegroundWindow(desktop);Pump(60);
            if(GetForegroundWindow()==desktop){controller.HandleHotkey(translation::SelectionHotkey);Require(IsWindowVisible(popup),"desktop blank opens popup");Require(Until([&]{return IsWindowVisible(popup)&&Status(popup)==L"识别内容为空";}),"desktop empty warning");std::cout<<"PASS desktop empty selection popup\n";}
            else std::cout<<"NOTE: desktop foreground activation unavailable; blank native surface tested.\n";
        }
        ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);SendMessageW(childEdit,WM_APP+3,1,0);controller.HandleHotkey(translation::SelectionHotkey);
        Require(Until([&]{Require(IsWindowVisible(popup),"slow copy popup hidden");return capture::WindowText(edit).find(L"中文取词")!=std::wstring::npos;}),"slow copy filled input");SendMessageW(childEdit,WM_APP+3,0,0);
        ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);SendMessageW(childEdit,WM_APP+3,1,0);controller.HandleHotkey(translation::SelectionHotkey);
        Require(Until([&]{return GetClipboardOwner()==popup;}),"foreign-writer fixture must wait for copy dispatch");
        Require(capture::CopyText(host,L"foreign clipboard writer"),"foreign clipboard fixture");Pump(350);Require(capture::WindowText(edit).empty(),"foreign clipboard or late source overwrote input");
        Require(Status(popup).find(L"其他程序")!=std::wstring::npos,"foreign clipboard rejection did not explain failure");
        ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);controller.HandleHotkey(translation::SelectionHotkey);
        INPUT newKeys[2]{};for(auto& event:newKeys){event.type=INPUT_KEYBOARD;event.ki.wVk=VK_F6;}newKeys[1].ki.dwFlags=KEYEVENTF_KEYUP;SendInput(2,newKeys,sizeof(INPUT));Pump(350);
        Require(IsWindowVisible(popup)&&capture::WindowText(edit).empty(),"new keyboard input cancels late copy without hide cycle");
        ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);controller.HandleHotkey(translation::SelectionHotkey);SetWindowTextW(edit,L"user input during delayed copy");Pump(350);Require(capture::WindowText(edit)==L"user input during delayed copy","late selection overwrote user input");SendMessageW(childEdit,WM_APP+3,0,0);
        ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);SendMessageW(childEdit,WM_APP+4,0,0);controller.HandleHotkey(translation::SelectionHotkey);
        Require(IsWindowVisible(popup)&&Status(popup)==L"识别内容为空","clipboard contention must not delay popup");Require(Until([&]{return capture::WindowText(edit).find(L"中文取词")!=std::wstring::npos;}),"clipboard contention retry");
        auto nativeClick=[](HWND control){RECT bounds{};GetWindowRect(control,&bounds);SetCursorPos((bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2);INPUT input[2]{};for(auto& i:input)i.type=INPUT_MOUSE;input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,input,sizeof(INPUT));Pump(80);};
        for(int control:{1101,1102}){
            ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);Require(IsWindowVisible(popup),"blank popup before real inside click");
            nativeClick(Child(popup,control));Require(IsWindowVisible(popup),"inside click hid nonactivated popup");
            if(control==1101){Require(GetFocus()==edit,"real click did not focus input");SetWindowTextW(edit,L"text after real click");Require(capture::WindowText(edit)==L"text after real click","inside input edit");}
            if(control==1102){ActivateSource(host);Pump(60);Require(IsWindowVisible(popup),"pinned popup hidden by external click");Command(popup,1102);}
        }
        ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);
        RECT card{},popupRect{};GetWindowRect(Child(popup,1120),&card);GetWindowRect(popup,&popupRect);
        SetCursorPos(popupRect.left+MulDiv(80,GetDpiForWindow(popup),96),card.top-MulDiv(18,GetDpiForWindow(popup),96));
        INPUT clicks[2]{};for(auto& i:clicks)i.type=INPUT_MOUSE;clicks[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;clicks[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,clicks,sizeof(INPUT));Pump(80);
        Require(IsWindowVisible(popup)&&!IsWindowVisible(Child(popup,1120)),"real title click must not expand");
        SetCursorPos(popupRect.right-MulDiv(33,GetDpiForWindow(popup),96),card.top-MulDiv(18,GetDpiForWindow(popup),96));
        SendInput(2,clicks,sizeof(INPUT));Pump(80);
        Require(IsWindowVisible(popup)&&!IsWindowVisible(Child(popup,1120)),"empty arrow click must stay collapsed without hiding popup");
        DesktopSnapshot(popup,output/L"inside-click-visible.png");
        ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);ActivateSource(host);Pump(80);Require(!IsWindowVisible(popup),"outside click did not hide popup");
        ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);RECT outside{};GetWindowRect(host,&outside);
        auto inputWhileBusy=std::async(std::launch::async,[outside]{Sleep(80);SetCursorPos(outside.left+30,outside.top+80);INPUT mouse[2]{};for(auto& e:mouse)e.type=INPUT_MOUSE;mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,mouse,sizeof(INPUT));});
        Sleep(1200);inputWhileBusy.get();Pump(80);Require(!IsWindowVisible(popup),"input hook lost during busy UI thread");
        if(eAvailable){
            ActivateSource(target);SendMessageW(target,WM_APP+1,0,0);
            for(int attempt=0;attempt<10;++attempt){
                const auto sequenceBeforeHotkey=GetClipboardSequenceNumber();
                INPUT keys[4]{};for(auto& key:keys)key.type=INPUT_KEYBOARD;keys[0].ki.wVk=VK_MENU;keys[1].ki.wVk='E';keys[2].ki.wVk='E';keys[2].ki.dwFlags=KEYEVENTF_KEYUP;keys[3].ki.wVk=VK_MENU;keys[3].ki.dwFlags=KEYEVENTF_KEYUP;
                Require(SendInput(4,keys,sizeof(INPUT))==4,"inject real Alt+E");
                Require(Until([&]{return GetClipboardSequenceNumber()!=sequenceBeforeHotkey&&IsWindowVisible(popup)&&capture::WindowText(edit).find(L"中文取词")!=std::wstring::npos;}),"real Alt+E failed to show and copy");Pump(80);Require(IsWindowVisible(popup),"late focus message hid hotkey popup");
            }
            std::cout<<"PASS 10 actual registered Alt+E keyboard sequences\n";
            BrowserCase(host,popup,edit,output);
        }else std::cout<<"NOTE: Alt+E externally occupied; direct entry tests only.\n";
        // Capture a real, clean desktop region; callback must receive original pixels, without tint/magnifier.
        controller.HandleHotkey(translation::ManualHotkey);SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);SetForegroundWindow(target);Pump(100);
        capture::Image selected;translation::TranslationCapture capture;
        Require(capture.Start([&](capture::Image image){selected=std::move(image);},[]{}),"selection start");Pump(70);
        HWND overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);RECT virtualBounds{};GetWindowRect(overlay,&virtualBounds);
        RECT targetBounds{};GetWindowRect(childEdit,&targetBounds);POINT a{targetBounds.left-virtualBounds.left,targetBounds.top-virtualBounds.top},b{targetBounds.right-virtualBounds.left,targetBounds.bottom-virtualBounds.top};
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y));Pump(50);Snapshot(overlay,output/L"capture-selection.png");
        SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));Require(!selected.Empty(),"selection completion");Require(capture::SavePng(selected,(output/L"clean-crop.png").wstring()),"crop snapshot");
        // Exercise the complete screenshot -> local OCR -> translation input route.
        SetWindowTextW(edit,L"等待 OCR 替换");
        SetForegroundWindow(target);Pump(70);TranslationMenu(host,1);Require(Until([&]{overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);return overlay!=nullptr;}),"menu OCR overlay");
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));
        Require(!IsWindowVisible(popup),"OCR opened popup before finding text");
        const bool recognized=Until([&]{return capture::WindowText(edit).find(L"English 123")!=std::wstring::npos;},12000);
        Snapshot(popup,output/L"ocr-result.png");Require(recognized,"OCR result filled input");Require(IsWindowVisible(Child(popup,1120)),"OCR did not auto submit");
        Require(FindWindowW(L"PcTool.OcrResult",nullptr)==nullptr,"unexpected OCR result window");
        // A completed OCR request must not overwrite edits made while it is running.
        SetForegroundWindow(target);Pump(30);controller.HandleHotkey(translation::CaptureHotkey);Pump(40);overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));SetWindowTextW(edit,L"保留用户的新输入");Pump(900);
        Require(capture::WindowText(edit)==L"保留用户的新输入","late OCR overwrote edit");
        // A selection smaller than the threshold leaves the capture active; right click cancels it.
        controller.HandleHotkey(translation::CaptureHotkey);Pump(40);overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(a.x+1,a.y+1));Require(IsWindow(overlay),"tiny selection committed");SendMessageW(overlay,WM_RBUTTONUP,0,0);
        Require(!FindWindowW(L"PcTool.TranslationCapture",nullptr),"right click cancel");
        SetForegroundWindow(target);Pump(30);controller.HandleHotkey(translation::CaptureHotkey);Pump(40);overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y+120));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,a.y+170));
        Require(Until([&]{return Status(popup)==L"识别内容为空";},8000),"blank OCR feedback");Require(capture::WindowText(edit).empty()&&IsWindowVisible(popup),"blank OCR must show empty popup");
        controller.HandleHotkey(translation::CaptureHotkey);Pump(40);overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);
        SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));SendMessageW(edit,WM_KEYDOWN,VK_ESCAPE,0);Pump(700);
        Require(!IsWindowVisible(popup),"late OCR reopened hidden popup");
        controller.HandleHotkey(translation::CaptureHotkey);Pump(50);overlay=FindWindowW(L"PcTool.TranslationCapture",nullptr);if(overlay)SendMessageW(overlay,WM_KEYDOWN,VK_ESCAPE,0);Require(!FindWindowW(L"PcTool.TranslationCapture",nullptr),"cancel overlay");
        // Real Windows Notepad, isolated file/process. Never attach to a pre-existing user window.
        wchar_t system[32768]{};GetSystemDirectoryW(system,32768);const auto notepad=std::filesystem::path(system)/L"notepad.exe";
        if(std::filesystem::exists(notepad)){
            const auto sourcePath=std::filesystem::absolute(output/L"notepad-source.txt");
            const std::wstring sourceText=L"真实记事本取词 Notepad 456\r\n第二行";
            {std::ofstream file(sourcePath,std::ios::binary);const wchar_t bom=0xfeff;file.write(reinterpret_cast<const char*>(&bom),2);file.write(reinterpret_cast<const char*>(sourceText.data()),std::streamsize(sourceText.size()*2));}
            std::wstring command=L"\""+notepad.wstring()+L"\" \""+sourcePath.wstring()+L"\"";
            STARTUPINFOW info{sizeof(info)};PROCESS_INFORMATION app{};
            if(CreateProcessW(notepad.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&info,&app)){
                CloseHandle(app.hThread);
                struct Find {DWORD pid;HWND window{};} found{app.dwProcessId};
                Until([&]{EnumWindows([](HWND w,LPARAM l)->BOOL{auto& f=*reinterpret_cast<Find*>(l);DWORD pid{};GetWindowThreadProcessId(w,&pid);if(pid==f.pid&&IsWindowVisible(w)){f.window=w;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&found));return found.window!=nullptr;},4000);
                HWND nativeEdit=found.window?FindWindowExW(found.window,nullptr,L"Edit",nullptr):nullptr;
                if(nativeEdit){
                    ActivateSource(found.window);SendMessageW(nativeEdit,EM_SETSEL,0,-1);SetWindowTextW(edit,L"等待真实应用取词");TranslationMenu(host,2);
                    const bool imported=Until([&]{return capture::WindowText(edit).find(L"Notepad 456")!=std::wstring::npos;});
                    PostMessageW(found.window,WM_CLOSE,0,0);WaitForSingleObject(app.hProcess,3000);CloseHandle(app.hProcess);
                    Require(imported,"real Notepad selection");Snapshot(popup,output/L"notepad-result.png");
                    SetForegroundWindow(popup);controller.HandleHotkey(translation::SelectionHotkey);Require(IsWindowVisible(popup)&&Status(popup).find(L"已关闭")!=std::wstring::npos&&capture::WindowText(edit).empty()&&!IsWindowVisible(Child(popup,1120))&&!IsWindowVisible(Child(popup,1121)),"closed source shows empty input, collapsed cards and specific reason");
                    std::cout<<"PASS real Windows Notepad selection and closed-source feedback\n";
                }else{if(found.window)PostMessageW(found.window,WM_CLOSE,0,0);CloseHandle(app.hProcess);std::cout<<"NOTE: classic Notepad editor unavailable; real-app case not executed\n";}
            }else std::cout<<"NOTE: Notepad launch failed; real-app case not executed\n";
        }
        ActivateSource(host);controller.HandleHotkey(translation::SelectionHotkey);
        controller.Configure(host,false,{},false);Require(IsWindowVisible(popup),"disabling hotkeys closed the translation window");
        SendMessageW(popup,WM_CLOSE,0,0);ActivateSource(host);
        TranslationMenu(host,0);Require(IsWindowVisible(popup)&&capture::WindowText(edit).empty(),"menu input unavailable with hotkeys disabled");
        SendMessageW(popup,WM_CLOSE,0,0);Require(capture::CopyText(host,L"clipboard after window hidden"),"hidden clipboard fixture");Pump(70);Require(!IsWindowVisible(popup),"hidden request reopened popup");
        if(qAvailable){Require(RegisterHotKey(host,900,MOD_ALT|MOD_NOREPEAT,'Q')!=FALSE,"hotkey released");UnregisterHotKey(host,900);}
        else std::cout<<"NOTE: Alt+Q is occupied externally; entry behavior tested through controller dispatch.\n";
        controller.Shutdown();history.Shutdown();PostMessageW(target,WM_CLOSE,0,0);WaitForSingleObject(process.hProcess,3000);CloseHandle(process.hProcess);DestroyWindow(host);service=nullptr;
        std::cout<<"PASS: manual input, pin/hide, DPI, copy, cross-process selection, history, stale clipboard, selection, OCR route, cancel and shutdown\n";
    }catch(const std::exception& e){if(HWND target=FindWindowW(L"PcTool.TranslationCopyTarget",nullptr)){DWORD_PTR ignored{};SendMessageTimeoutW(target,WM_CLOSE,0,0,SMTO_ABORTIFHUNG,1000,&ignored);}std::cerr<<"FAIL: "<<e.what()<<"\n";CoUninitialize();return 1;}
    CoUninitialize();return 0;
}

