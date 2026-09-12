#include <winsock2.h>
#include <ws2tcpip.h>
#include "translation/source_provider.h"
#include "translation/source_settings.h"
#include "translation/translation_controller.h"
#include "translation/translation_submission.h"
#include "shared/ui/capture_ui.h"
#include "shared/platform/capture_platform.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <fstream>
#include <future>
#include <thread>
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <richedit.h>
namespace {
void Check(bool pass,const char* message){if(!pass)throw std::runtime_error(message);}
void Pump(int ms){auto end=GetTickCount64()+ms;do{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){if(!translation::HandleSourceSettingsMessage(m)){TranslateMessage(&m);DispatchMessageW(&m);}}Sleep(5);}while(GetTickCount64()<end);}
template<class F>bool Until(F test,int ms=4000){auto end=GetTickCount64()+ms;do{Pump(10);if(test())return true;}while(GetTickCount64()<end);return false;}
HWND Find(const wchar_t* cls){return FindWindowW(cls,nullptr);}
void Click(HWND window,int id){SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(window,id)));Pump(20);}
void AnswerNextDialog(HWND owner,int answer){SetPropW(owner,L"FixtureDialogAnswer",reinterpret_cast<HANDLE>(INT_PTR(answer)));SetTimer(owner,77,25,[](HWND w,UINT,UINT_PTR timer,DWORD){HWND dialog=GetLastActivePopup(w);if(dialog&&dialog!=w){wchar_t cls[32]{};GetClassNameW(dialog,cls,32);if(wcscmp(cls,L"#32770")==0||wcscmp(cls,L"PcTool.TranslationConfirmation")==0){KillTimer(w,timer);const int answer=int(reinterpret_cast<INT_PTR>(GetPropW(w,L"FixtureDialogAnswer")));PostMessageW(dialog,WM_COMMAND,answer,0);RemovePropW(w,L"FixtureDialogAnswer");}}});}
void Snapshot(HWND w,const std::filesystem::path& path){RECT r{};GetWindowRect(w,&r);capture::Image im(r.right-r.left,r.bottom-r.top);HBITMAP bitmap=capture::ToBitmap(im);HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,bitmap);PrintWindow(w,dc,2);SelectObject(dc,old);DeleteDC(dc);im=capture::FromBitmap(bitmap);DeleteObject(bitmap);Check(capture::SavePng(im,path.wstring()),"settings snapshot");}
struct DialogRun {
    static inline DialogRun* active{};
    HWND owner;std::vector<int> answers;int count{};std::filesystem::path screenshots;
    DialogRun(HWND w,std::vector<int> replies,std::filesystem::path output={}):owner(w),answers(std::move(replies)),screenshots(std::move(output)){
        active=this;SetTimer(owner,78,25,[](HWND,UINT,UINT_PTR,DWORD){if(active)active->Reply();});
    }
    ~DialogRun(){KillTimer(owner,78);active=nullptr;}
    void Reply(){
        HWND dialog=FindWindowW(L"PcTool.TranslationConfirmation",nullptr);if(!dialog||GetWindow(dialog,GW_OWNER)!=owner)return;
        KillTimer(owner,78);if(count==0&&!screenshots.empty())for(UINT dpi:{96u,144u,192u}){RECT r{40,40,40+MulDiv(460,dpi,96),40+MulDiv(240,dpi,96)};SendMessageW(dialog,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));Pump(30);Snapshot(dialog,screenshots/(L"discard-confirmation-"+std::to_wstring(dpi)+L".png"));}
        SetTimer(owner,78,25,[](HWND,UINT,UINT_PTR,DWORD){if(active)active->Reply();});const int answer=count<int(answers.size())?answers[count]:IDNO;++count;
        if(answer==-1){HWND close=GetDlgItem(dialog,IDCANCEL);SendMessageW(close,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(12,12));SendMessageW(close,WM_LBUTTONUP,0,MAKELPARAM(12,12));}
        else if(answer==-2)PostMessageW(GetDlgItem(dialog,IDNO),WM_KEYDOWN,VK_ESCAPE,0);
        else SendMessageW(dialog,WM_COMMAND,answer,0);
    }
};
class Server {
    SOCKET listener_{INVALID_SOCKET};std::thread thread_;unsigned short port_{};
public:
    std::string received;
    Server(std::string body,unsigned status=200,bool stream=false,int wait=0){
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Check(listener_!=INVALID_SOCKET,"fixture socket");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        Check(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"fixture bind");int size=sizeof(address);getsockname(listener_,reinterpret_cast<sockaddr*>(&address),&size);port_=ntohs(address.sin_port);listen(listener_,1);
        thread_=std::thread([this,body=std::move(body),status,stream,wait]{
            SOCKET peer=accept(listener_,nullptr,nullptr);if(peer==INVALID_SOCKET)return;DWORD timeout=4000;setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            char buf[4096];size_t needed=0;while(true){int n=recv(peer,buf,sizeof(buf),0);if(n<=0)break;received.append(buf,n);auto end=received.find("\r\n\r\n");if(end!=std::string::npos){auto lower=received.substr(0,end);std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(tolower(c));});auto at=lower.find("content-length:");needed=at==std::string::npos?0:std::stoul(lower.substr(at+15));if(received.size()>=end+4+needed)break;}}
            if(wait)Sleep(wait);
            const auto header="HTTP/1.1 "+std::to_string(status)+" Test\r\nContent-Type: "+(stream?std::string("text/event-stream"):std::string("application/json"))+"; charset=utf-8\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n";
            send(peer,header.data(),int(header.size()),0);
            if(stream){for(size_t i=0;i<body.size();i+=7){if(send(peer,body.data()+i,int(std::min<size_t>(7,body.size()-i)),0)<=0)break;Sleep(1);}}
            else send(peer,body.data(),int(body.size()),0);shutdown(peer,SD_BOTH);closesocket(peer);
        });
    }
    ~Server(){shutdown(listener_,SD_BOTH);closesocket(listener_);if(thread_.joinable())thread_.join();}
    std::wstring Url()const{return L"http://127.0.0.1:"+std::to_wstring(port_)+L"/v1";}
    void Join(){if(thread_.joinable())thread_.join();}
};
translation::SourceConfig Config(translation::SourceKind kind){auto s=translation::NewSource(kind);s.key=L"fixture-secret";s.model=L"fixture-model";s.region=L"eastasia";s.appId=L"fixture-app";s.enabled=s.verified=true;return s;}
void Core(const std::filesystem::path& output){
    std::cout<<"source store"<<std::endl;
    using namespace translation;using namespace winrt::Windows::Data::Json;
    for(const auto* endpoint:{L"http://192.0.2.10:8080/v1",L"http://example.invalid/v1",L"https://example.invalid:8443/v1",L"http://localhost:8080/v1"}){
        auto config=Config(SourceKind::OpenAI);config.endpoint=endpoint;
        Check(ValidateConfig(config).empty(),"HTTP/HTTPS custom endpoint rejected");
        Check(BuildTranslationRequest(config,L"Hello",false).url==std::wstring(endpoint)+L"/chat/completions","endpoint scheme/port changed");
    }
    for(const auto* endpoint:{L"ftp://example.invalid/v1",L"file:///test",L"http://user:password@example.invalid/v1",L"http://example.invalid/v1?key=test",L"http://example.invalid/v1#fragment"}){
        auto config=Config(SourceKind::OpenAI);config.endpoint=endpoint;
        Check(!ValidateConfig(config).empty(),"invalid custom endpoint accepted");
    }
    const auto folder=output/L"store";SourceStore store(folder);std::vector<SourceConfig> sources;
    for(auto kind:{SourceKind::OpenAI,SourceKind::DeepL,SourceKind::Azure,SourceKind::Baidu})sources.push_back(Config(kind));
    store.Save(sources);std::cout<<"saved"<<std::endl;auto restored=store.Load();std::cout<<"loaded "<<restored.size()<<std::endl;Check(restored.size()==4&&restored[0].key==sources[0].key&&restored[3].id==sources[3].id,"DPAPI store roundtrip");
    std::ifstream file(folder/L"sources.json");std::string persisted((std::istreambuf_iterator<char>(file)),{});file.close();Check(persisted.find("fixture-secret")==std::string::npos,"plaintext key persisted");
    std::swap(sources[0],sources[3]);sources[1].enabled=false;store.Save(sources);restored=store.Load();Check(restored[0].id==sources[0].id&&!restored[1].enabled,"order/disabled persistence");
    auto draft=NewSource(SourceKind::DeepL);draft.enabled=true;sources.push_back(draft);store.Save(sources);Check(!store.Load().back().enabled,"unverified persisted as enabled");
    Check(ContainsHan(L"mixed 汉字")&&!ContainsHan(L"Hello 123"),"translation direction");
    auto baidu=Config(SourceKind::Baidu);baidu.appId=L"2015063000000001";baidu.key=L"12345678";
    auto signedRequest=BuildTranslationRequest(baidu,L"apple",false,"1435660288");Check(signedRequest.body.find("sign=f89f9594663708c1605f3d736d01d2d4")!=std::string::npos,"Baidu official signature vector");
    const std::string bodies[]={u8R"({"choices":[{"message":{"content":"你好"}}]})",u8R"({"translations":[{"text":"你好"}]})",u8R"([{"translations":[{"text":"你好","to":"zh-Hans"}]}])",u8R"({"trans_result":[{"src":"hello","dst":"你好"}]})"};
    for(int i=0;i<4;++i){std::cout<<"protocol "<<i<<std::endl;auto s=Config(SourceKind(i));Server server(bodies[i]);auto p=MakeSourceProviderForTest(s,[&](const HttpMessage& message,auto cancel,auto chunks){auto redirected=message;redirected.url=server.Url();return SendTranslationHttp(redirected,cancel,chunks);});
        std::promise<TranslationResult> promise;auto result=promise.get_future();p->Translate(L"Hello",std::make_shared<capture::Cancellation>(),[&](auto value){promise.set_value(value);});Check(result.wait_for(std::chrono::seconds(5))==std::future_status::ready,"HTTP completion timeout");auto value=result.get();if(!value.error.empty())std::wcerr<<value.error<<L'\n';Check(value.error.empty()&&value.text==L"你好","protocol translated result");server.Join();
        if(i==0)Check(server.received.find("Bearer fixture-secret")!=std::string::npos&&server.received.find("fixture-model")!=std::string::npos,"OpenAI auth/model");
        if(i==1)Check(server.received.find("DeepL-Auth-Key fixture-secret")!=std::string::npos,"DeepL auth");
        if(i==2)Check(server.received.find("Ocp-Apim-Subscription-Region: eastasia")!=std::string::npos,"Azure region");
        if(i==3)Check(server.received.find("sign=")!=std::string::npos&&server.received.find("to=zh")!=std::string::npos,"Baidu body");
    }
    const std::string stream=u8"data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\ndata: [DONE]\n\n";
    {Server server(stream,200,true);auto s=Config(SourceKind::OpenAI);s.endpoint=server.Url();auto p=MakeSourceProvider(s);std::promise<TranslationResult> promise;auto future=promise.get_future();std::atomic<int> updates{};
        p->TranslateStreaming(L"Hello",std::make_shared<capture::Cancellation>(),[&](auto r){promise.set_value(r);},[&](auto){++updates;});Check(future.wait_for(std::chrono::seconds(5))==std::future_status::ready,"SSE timeout");Check(future.get().text==L"你好"&&updates>=2,"split SSE UTF8 stream");}
    {Server server(R"({"error":{"message":"limit"}})",429);auto s=Config(SourceKind::OpenAI);s.endpoint=server.Url();auto p=MakeSourceProvider(s);std::promise<TranslationResult> promise;auto future=promise.get_future();p->Translate(L"Hello",std::make_shared<capture::Cancellation>(),[&](auto r){promise.set_value(r);});Check(future.wait_for(std::chrono::seconds(5))==std::future_status::ready&&future.get().error.find(L"限流")!=std::wstring::npos,"429 error");}
    {Server server(bodies[0],200,false,600);auto s=Config(SourceKind::OpenAI);s.endpoint=server.Url();auto p=MakeSourceProvider(s);auto cancel=std::make_shared<capture::Cancellation>();std::atomic<bool> called{};p->Translate(L"Hello",cancel,[&](auto){called=true;});Sleep(80);const auto start=GetTickCount64();cancel->requested=true;p.reset();Check(GetTickCount64()-start<500&&!called,"HTTP cancellation did not release promptly");}
    {Server server(bodies[0],200,false,61000);auto s=Config(SourceKind::OpenAI);s.endpoint=server.Url();auto p=MakeSourceProvider(s);std::promise<TranslationResult> promise;auto future=promise.get_future();p->Translate(L"Hello",std::make_shared<capture::Cancellation>(),[&](auto r){promise.set_value(r);});Check(future.wait_for(std::chrono::seconds(65))==std::future_status::ready&&future.get().error.find(L"超时")!=std::wstring::npos,"native HTTP total deadline");}
    Check(!ParseTranslationResponse(SourceKind::DeepL,"garbage",200).error.empty(),"malformed result accepted");
    {struct Deferred:TranslationProvider{ProviderInfo info;Completion complete;Progress progress;explicit Deferred(const wchar_t* id):info{id,id,true}{}ProviderInfo Info()const override{return info;}void Translate(std::wstring,std::shared_ptr<capture::Cancellation>,Completion c)override{complete=std::move(c);}void TranslateStreaming(std::wstring t,std::shared_ptr<capture::Cancellation> c,Completion d,Progress p)override{progress=std::move(p);Translate(std::move(t),std::move(c),std::move(d));}};
        auto a=std::make_shared<Deferred>(L"a"),b=std::make_shared<Deferred>(L"b");Submission submission;submission.providers={a,b};submission.Submit(L"Hello");for(int i=0;i<100;++i)a->progress(std::to_wstring(i));b->complete({L"second",{}});Check(submission.Poll()&&submission.cards[0].text==L"99"&&submission.cards[1].text==L"second","independent stream sources");
        auto stale=a->complete;submission.providers={b};submission.Reset();stale({L"obsolete",{}});Check(!submission.Poll()&&submission.cards.size()==1&&submission.cards[0].id==L"b"&&submission.cards[0].text.empty(),"removed source callback applied");
    }
    std::cout<<"PASS source store/DPAPI/order, four protocols over loopback HTTP, signing, streamed UTF8, 429, cancellation and malformed responses\n";
}
void UI(const std::filesystem::path& output){
    HWND backdrop=CreateWindowExW(0,L"STATIC",L"",WS_POPUP|WS_VISIBLE|SS_WHITERECT,GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    struct BackdropCleanup{HWND w;~BackdropCleanup(){DestroyWindow(w);}} backdropCleanup{backdrop};SetForegroundWindow(backdrop);UpdateWindow(backdrop);
    using namespace translation;SourceStore store(output/L"ui-store");std::vector<SourceConfig> configs;unsigned changes{};
    SourceSettings settings(store,configs,[&]{++changes;});settings.Show(nullptr);HWND window=Find(L"PcTool.TranslationSettings");Check(window&&IsWindowVisible(window),"settings window missing");settings.Show(nullptr);Check(window==Find(L"PcTool.TranslationSettings"),"settings duplicated");
    for(UINT dpi:{96u,144u,192u}){RECT r{30,30,30+MulDiv(1040,dpi,96),30+MulDiv(690,dpi,96)};SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));Pump(60);Snapshot(window,output/(L"settings-empty-"+std::to_wstring(dpi)+L".png"));
        GetWindowRect(window,&r);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);Check(r.left>=mi.rcWork.left&&r.top>=mi.rcWork.top&&r.right<=mi.rcWork.right&&r.bottom<=mi.rcWork.bottom,"rounded settings exceeded work area");
        InflateRect(&r,28,28);capture::Image image(r.right-r.left,r.bottom-r.top);HBITMAP bitmap=capture::ToBitmap(image);auto screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);auto old=SelectObject(dc,bitmap);BitBlt(dc,0,0,image.width,image.height,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);image=capture::FromBitmap(bitmap);DeleteObject(bitmap);Check(capture::SavePng(image,(output/(L"settings-frame-"+std::to_wstring(dpi)+L".png")).wstring()),"settings frame screenshot");
    }
    RECT reset{30,30,1070,720};SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(96,96),reinterpret_cast<LPARAM>(&reset));
    Click(window,2109);HWND picker=Find(L"PcTool.TranslationSourcePicker");Check(picker&&IsWindowVisible(picker),"source picker missing");Snapshot(picker,output/L"picker.png");Check(IsWindowEnabled(GetDlgItem(picker,2210)),"default selection cannot be added");Check(configs.empty(),"selecting card created source prematurely");Snapshot(picker,output/L"picker-selected.png");
    for(UINT dpi:{96u,144u,192u}){RECT r{30,30,30+MulDiv(640,dpi,96),30+MulDiv(360,dpi,96)};SendMessageW(picker,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));Pump(20);Snapshot(picker,output/(L"picker-selected-"+std::to_wstring(dpi)+L".png"));}
    Click(picker,2211);Check(configs.empty()&&IsWindowEnabled(window)&&!Find(L"PcTool.TranslationSourcePicker"),"picker close created draft or left owner disabled");
    Click(window,2109);picker=Find(L"PcTool.TranslationSourcePicker");Check(IsWindowEnabled(GetDlgItem(picker,2210)),"reopened default selection cannot be added");MSG escape{picker,WM_KEYDOWN,VK_ESCAPE,0};Check(HandleSourceSettingsMessage(escape),"picker Escape routing");Pump(30);Check(!Find(L"PcTool.TranslationSourcePicker")&&configs.empty(),"picker Escape created source");
    Click(window,2109);picker=Find(L"PcTool.TranslationSourcePicker");Click(picker,2210);Check(configs.size()==1&&configs[0].kind==SourceKind::OpenAI&&!configs[0].enabled,"default source add failed");
    Server server(u8R"({"choices":[{"message":{"content":"Hello, world."}}]})");
    SetWindowTextW(GetDlgItem(window,2102),server.Url().c_str());SetWindowTextW(GetDlgItem(window,2103),L"test-key");SetWindowTextW(GetDlgItem(window,2104),L"test-model");Click(window,2108);Check(Until([&]{return configs[0].verified;}),"native verify/save did not complete");Check(configs[0].enabled&&store.Load()[0].key==L"test-key","native saved credentials");
    SetWindowTextW(GetDlgItem(window,2101),L"本地测试服务");Check(store.Load()[0].name==L"本地测试服务","rename not saved");
    Check(SendDlgItemMessageW(window,2103,EM_GETPASSWORDCHAR,0,0)!=0,"key not initially masked");Click(window,2111);Check(SendDlgItemMessageW(window,2103,EM_GETPASSWORDCHAR,0,0)==0&&capture::WindowText(GetDlgItem(window,2111))==L"隐藏密钥","eye did not reveal key");Snapshot(window,output/L"eye-revealed.png");Click(window,2111);Check(SendDlgItemMessageW(window,2103,EM_GETPASSWORDCHAR,0,0)!=0,"eye did not mask key");Snapshot(window,output/L"eye-masked.png");
    {const auto saved=configs[0].endpoint;Server failure(R"({"error":{"message":"bad key"}})",401);SetWindowTextW(GetDlgItem(window,2102),failure.Url().c_str());Click(window,2108);Check(Until([&]{return IsWindowEnabled(GetDlgItem(window,2102));}),"failed verification did not restore form");Check(configs[0].endpoint==saved,"failed verification replaced saved source");
        AnswerNextDialog(window,IDNO);SendMessageW(window,WM_CLOSE,0,0);Check(IsWindowVisible(window)&&capture::WindowText(GetDlgItem(window,2102))==failure.Url(),"cancel discard lost edits");
        {DialogRun replies(window,{-1});SendMessageW(window,WM_CLOSE,0,0);Check(replies.count==1&&IsWindowEnabled(window)&&capture::WindowText(GetDlgItem(window,2102))==failure.Url(),"confirmation X did not retain editing");}
        {DialogRun replies(window,{-2});Click(window,2109);Check(replies.count==1&&!Find(L"PcTool.TranslationSourcePicker")&&capture::WindowText(GetDlgItem(window,2102))==failure.Url(),"confirmation Escape did not retain editing");}
        {DialogRun replies(window,{IDYES},output);Click(window,2109);Check(replies.count==1&&!Find(L"PcTool.TranslationSourcePicker"),"discard replayed add action");Check(capture::WindowText(GetDlgItem(window,2102))==saved&&configs[0].endpoint==saved&&store.Load()[0].endpoint==saved,"discard did not restore saved connection");
            Click(window,2109);Check(replies.count==1&&Find(L"PcTool.TranslationSourcePicker"),"fresh add click did not open picker");
            Click(Find(L"PcTool.TranslationSourcePicker"),2201);Click(Find(L"PcTool.TranslationSourcePicker"),2210);Check(replies.count==1&&configs.size()==2,"discard prompted again when adding selected source");}
        AnswerNextDialog(window,IDYES);Click(window,2110);Check(configs.size()==1,"discard fixture cleanup");
    }
    {const auto saved=configs[0].endpoint;Server delayed(u8R"({"choices":[{"message":{"content":"late"}}]})",200,false,300);SetWindowTextW(GetDlgItem(window,2102),delayed.Url().c_str());Click(window,2108);Click(window,2108);Pump(360);Check(configs[0].endpoint==saved&&IsWindowEnabled(GetDlgItem(window,2102)),"cancel verification applied late configuration");SetWindowTextW(GetDlgItem(window,2102),saved.c_str());}
    HWND list=GetDlgItem(window,2112);RECT listRect{};GetClientRect(list,&listRect);SendMessageW(list,WM_LBUTTONDOWN,0,MAKELPARAM(listRect.right-28,32));Check(!configs[0].enabled,"toggle disable");SendMessageW(list,WM_LBUTTONDOWN,0,MAKELPARAM(listRect.right-28,32));Check(configs[0].enabled,"toggle enable");
    for(int i=1;i<4;++i){Click(window,2109);Click(Find(L"PcTool.TranslationSourcePicker"),2200+i);Click(Find(L"PcTool.TranslationSourcePicker"),2210);Check(configs.size()==size_t(i+1),"add provider type");Snapshot(window,output/(L"provider-"+std::to_wstring(i)+L".png"));}
    SetWindowTextW(GetDlgItem(window,2106),L"unsaved-app-id");{DialogRun replies(window,{IDYES,IDNO});Click(window,2110);Check(replies.count==1&&configs.size()==4,"discard replayed delete action");Click(window,2110);Check(replies.count==2&&configs.size()==4,"fresh delete did not ask for confirmation");}
    SetWindowTextW(GetDlgItem(window,2106),L"unsaved-app-id");{DialogRun replies(window,{IDYES});SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(30,28));SendMessageW(list,WM_LBUTTONUP,0,MAKELPARAM(30,28));Check(replies.count==1&&capture::WindowText(GetDlgItem(window,2101))==configs[3].name,"discard replayed source selection");SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(30,28));SendMessageW(list,WM_LBUTTONUP,0,MAKELPARAM(30,28));Check(replies.count==1&&capture::WindowText(GetDlgItem(window,2101))==configs[0].name,"fresh source selection failed");}
    Click(window,2109);Click(Find(L"PcTool.TranslationSourcePicker"),2200);Click(Find(L"PcTool.TranslationSourcePicker"),2210);Check(configs.size()==5&&configs[4].id!=configs[0].id,"duplicate provider instance ID");AnswerNextDialog(window,IDYES);Click(window,2110);Check(configs.size()==4,"confirmed source deletion");
    // Select and drag the fourth source to the top without editing its connection.
    const auto last=configs.back().id;SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(30,3*64+28));SendMessageW(list,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(30,28));SendMessageW(list,WM_LBUTTONUP,0,MAKELPARAM(30,28));Check(configs[0].id==last&&store.Load()[0].id==last,"source drag ordering");
    for(UINT dpi:{96u,144u,192u}){RECT r{30,30,30+MulDiv(1040,dpi,96),30+MulDiv(690,dpi,96)};SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));Pump(30);Snapshot(window,output/(L"settings-filled-"+std::to_wstring(dpi)+L".png"));}
    RECT compact{30,30,1070,720};SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(96,96),reinterpret_cast<LPARAM>(&compact));
    for(int i=0;i<8;++i){Click(window,2109);Click(Find(L"PcTool.TranslationSourcePicker"),2201);Click(Find(L"PcTool.TranslationSourcePicker"),2210);}
    Check(IsWindowVisible(GetDlgItem(window,2113)),"long source list scrollbar missing");SendMessageW(list,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA*10)),0);Pump(30);Snapshot(window,output/L"settings-list-scroll.png");
    SetWindowTextW(GetDlgItem(window,2103),L"unsaved-fixture");{DialogRun replies(window,{IDYES});SendMessageW(window,WM_CLOSE,0,0);Check(replies.count==1&&IsWindowVisible(window)&&capture::WindowText(GetDlgItem(window,2103)).empty(),"discard replayed close action");SendMessageW(window,WM_CLOSE,0,0);Check(!IsWindow(window)&&replies.count==1,"fresh close click failed");}
    settings.Show(nullptr);Check(Find(L"PcTool.TranslationSettings"),"settings reopen");settings.Close();
    TranslationController controller;controller.SetProviders({});HWND host=CreateWindowExW(0,L"STATIC",L"source fixture",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);controller.Configure(host,true,[]{return true;},false);controller.HandleHotkey(ManualHotkey);Pump(40);HWND popup=Find(L"PcTool.Translation");Check(popup&&IsWindowVisible(popup),"empty source popup missing");HWND viewport=GetDlgItem(popup,1110);Check(!GetDlgItem(viewport,1120),"unconfigured placeholder still exists");Snapshot(popup,output/L"no-source-popup.png");RECT view{};GetClientRect(viewport,&view);SendMessageW(viewport,WM_LBUTTONUP,0,MAKELPARAM(50,view.bottom-20));Check(Find(L"PcTool.TranslationSettings")&&!IsWindowVisible(popup),"empty prompt did not open settings");SendMessageW(Find(L"PcTool.TranslationSettings"),WM_CLOSE,0,0);
    struct ResultProvider:TranslationProvider{std::wstring id;explicit ResultProvider(const wchar_t* s):id(s){}ProviderInfo Info()const override{return {id,L"测试源 "+id,true,SourceKind(id.front()-L'A')};}void Translate(std::wstring,std::shared_ptr<capture::Cancellation>,Completion done)override{done({L"只读翻译结果 "+id,{}});}};
    controller.SetProviders({std::make_shared<ResultProvider>(L"A"),std::make_shared<ResultProvider>(L"B"),std::make_shared<ResultProvider>(L"C")});controller.HandleHotkey(ManualHotkey);HWND edit=GetDlgItem(viewport,1101);SetWindowTextW(edit,L"Hello");SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);Pump(80);
    HWND third=GetDlgItem(viewport,1122);Check(third&&IsWindowVisible(third)&&capture::WindowText(third)==L"只读翻译结果 C","third dynamic source missing");
    const auto text=capture::WindowText(third);SendMessageW(third,WM_CHAR,L'X',0);Check(capture::WindowText(third)==text,"dynamic result is editable");
    BYTE keyboard[256]{},original[256]{};GetKeyboardState(original);memcpy(keyboard,original,256);keyboard[VK_CONTROL]|=0x80;SetKeyboardState(keyboard);SendMessageW(third,WM_KEYDOWN,'A',0);SetKeyboardState(original);CHARRANGE range{};SendMessageW(third,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));std::vector<wchar_t> selectedText(size_t(range.cpMax-range.cpMin)+2);SendMessageW(third,EM_GETSELTEXT,0,reinterpret_cast<LPARAM>(selectedText.data()));std::wstring selected(selectedText.data());if(!selected.empty()&&selected.back()==L'\r')selected.pop_back();Check(range.cpMin==0&&selected==text,"third source Ctrl+A");
    std::wstring longText;for(int i=0;i<60;++i)longText+=L"第三个源的长结果 English\r\n";SetWindowTextW(third,longText.c_str());Pump(40);Check(IsWindowVisible(GetDlgItem(popup,1111)),"dynamic long result scrollbar");Snapshot(popup,output/L"three-source-results.png");
    SendMessageW(viewport,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),0);Pump(30);Snapshot(popup,output/L"result-icons-scrolled.png");
    std::vector<std::shared_ptr<TranslationProvider>> iconProviders;for(int i=0;i<4;++i){auto source=Config(SourceKind(i));source.name=SourceName(source.kind);iconProviders.push_back(MakeSourceProvider(source));}
    controller.SetProviders(std::move(iconProviders));controller.HandleHotkey(ManualHotkey);Pump(30);
    for(UINT dpi:{96u,144u,192u}){RECT r{30,30,30+MulDiv(430,dpi,96),30+MulDiv(340,dpi,96)};SendMessageW(popup,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));Pump(30);Snapshot(popup,output/(L"result-source-icons-"+std::to_wstring(dpi)+L".png"));}
    controller.SetProviders({});Check(!GetDlgItem(viewport,1120)&&!GetDlgItem(viewport,1122),"removed dynamic controls remain");controller.Shutdown();DestroyWindow(host);
    std::cout<<"PASS native settings add/verify/save/enable/rename/order/reopen, four forms, DPI layouts and empty popup\n";
}
}
int RunTranslationSourceTests(const std::filesystem::path& output,bool ui){
    std::filesystem::create_directories(output);WSADATA data{};Check(WSAStartup(MAKEWORD(2,2),&data)==0,"WSAStartup");
    ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);
    try{if(ui)UI(output);else Core(output);}catch(const winrt::hresult_error& e){std::wcerr<<L"HRESULT "<<std::hex<<unsigned(e.code())<<L" "<<e.message().c_str()<<std::endl;Gdiplus::GdiplusShutdown(token);WSACleanup();throw std::runtime_error("source HRESULT failure");}catch(...){std::cerr<<"source test failed"<<std::endl;Gdiplus::GdiplusShutdown(token);WSACleanup();throw;}
    Gdiplus::GdiplusShutdown(token);WSACleanup();return 0;
}
