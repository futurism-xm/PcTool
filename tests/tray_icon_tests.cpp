#include "shared/ui/tray_icon.h"
#include <iostream>
#include <stdexcept>
using shared_ui::TrayIcon;
void Check(bool value,const char* error){if(!value)throw std::runtime_error(error);}
void Native(){
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);const auto identity=shared_ui::TrayIdentity(path);
    struct Cleanup {GUID identity;~Cleanup(){NOTIFYICONDATAW data{sizeof(data)};data.uFlags=NIF_GUID;data.guidItem=identity;Shell_NotifyIconW(NIM_DELETE,&data);}} cleanup{identity};
    auto present=[&]{NOTIFYICONDATAW data{sizeof(data)};data.uFlags=NIF_GUID;data.guidItem=identity;return Shell_NotifyIconW(NIM_MODIFY,&data)!=FALSE;};
    HWND first=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Tray cleanup fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(first!=nullptr,"fixture window");
    TrayIcon initial;initial.Bind(first,LoadIconW(nullptr,IDI_APPLICATION),WM_APP,identity);
    Check(initial.Show()&&present(),"native add");
    // Simulate an interrupted process losing its HWND without deleting the icon.
    DestroyWindow(first);
    HWND second=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Tray recovery fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    TrayIcon recovered;recovered.Bind(second,LoadIconW(nullptr,IDI_APPLICATION),WM_APP,identity);
    const bool adopted=recovered.Show()&&present();
    const bool removed=recovered.Remove()&&!present();DestroyWindow(second);
    Check(adopted&&removed,"native orphan recovery / final cleanup");
    std::cout<<"PASS native GUID registration, destroyed owner recovery, deletion verified after owner lifetime\n";
}
int main(int argc,char**){try{
    if(argc>1){Native();return 0;}
    const auto identity=shared_ui::TrayIdentity(L"D:\\PcTool\\PcTool.exe");
    Check(IsEqualGUID(identity,shared_ui::TrayIdentity(L"d:\\pctool\\pctool.exe")),"identity changed with case");
    Check(!IsEqualGUID(identity,shared_ui::TrayIdentity(L"D:\\Other\\PcTool.exe")),"different unsigned locations shared identity");
    bool exists=false,ready=true,failDelete=false,loseAddReply=false;int adds=0,deletes=0;
    auto notify=[&](DWORD message,NOTIFYICONDATAW& data){
        Check((data.uFlags&NIF_GUID)&&IsEqualGUID(data.guidItem,identity),"missing stable identity");
        if(message==NIM_ADD){++adds;exists=true;return !loseAddReply;}
        if(message==NIM_DELETE){++deletes;if(failDelete)return false;bool previous=exists;exists=false;return previous;}
        if(message==NIM_MODIFY||message==NIM_SETVERSION)return exists;
        return false;
    };
    TrayIcon tray(notify,[&]{return ready;});tray.Bind(HWND(1),nullptr,WM_APP,identity);
    Check(tray.Remove()&&tray.CurrentState()==TrayIcon::State::Absent,"clean startup absence");
    Check(tray.Show()&&exists&&adds==1,"initial registration");
    Check(tray.Show()&&adds==1,"duplicate add");
    failDelete=true;Check(!tray.Remove()&&tray.CurrentState()==TrayIcon::State::Present,"failed deletion forgotten");
    failDelete=false;Check(tray.Remove()&&!exists,"delete retry failed");
    loseAddReply=true;Check(!tray.Show()&&exists&&tray.CurrentState()==TrayIcon::State::Unknown,"lost add reply not uncertain");
    const int previousAdds=adds;Check(tray.Show()&&adds==previousAdds,"retry added another icon instead of adoption");
    Check(tray.Remove()&&!exists,"cleanup adopted icon");
    Check(!tray.Show()&&exists,"lost response fixture");Check(tray.Remove()&&!exists,"failed ADD was not cleaned");
    loseAddReply=false;Check(tray.Show(),"registration after failure");ready=false;
    const int previousDeletes=deletes;Check(!tray.Remove()&&deletes==previousDeletes&&tray.CurrentState()==TrayIcon::State::Present,"unresponsive shell cleared state");
    ready=true;Check(tray.Remove(),"shell recovery cleanup");
    exists=true;TrayIcon restarted(notify,[&]{return ready;});restarted.Bind(HWND(2),nullptr,WM_APP,identity);
    failDelete=true;Check(!restarted.Show()&&!restarted.Show(),"failed old-owner deletion was treated as a successful new registration");failDelete=false;
    Check(restarted.Remove()&&!exists,"new controller did not remove prior process icon");
    Check(restarted.Show(),"restart registration");exists=false;restarted.ExplorerRestarted();Check(restarted.Show()&&exists,"Explorer restart recovery");
    Check(restarted.Remove(),"final cleanup");
    std::cout<<"PASS stable identity, duplicate prevention, failed deletion retry, lost ADD reply, shutdown cleanup, controller/Explorer restart\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
