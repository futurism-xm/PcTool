#include "monitor/taskbar_monitor_window.h"
#include "app/app_messages.h"
#include <stdexcept>
#include <iostream>
#include <set>
struct TaskbarLayoutTest {
    static inline int menuStep=0;
    static inline bool hotkeyWithMenu=false,menuFailure=false;
    static inline bool sourcesRequested=false;
    static void Key(WORD key,bool up=false) {
        INPUT input{}; input.type=INPUT_KEYBOARD; input.ki.wVk=key; input.ki.dwFlags=up?KEYEVENTF_KEYUP:0;
        if(SendInput(1,&input,sizeof(input))!=1) menuFailure=true;
    }
    static LRESULT CALLBACK MenuController(HWND w,UINT m,WPARAM wp,LPARAM lp) {
        if(m==kShowTranslationSourcesMessage){sourcesRequested=true;return 0;}
        if(m==WM_HOTKEY) {
            HWND menu=FindWindowW(L"#32768",nullptr);
            hotkeyWithMenu=menu && IsWindowVisible(menu);
            EndMenu(); return 0;
        }
        if(m==WM_TIMER) {
            if(!FindWindowW(L"#32768",nullptr)) { menuFailure=true; EndMenu(); return 0; }
            switch(menuStep++) {
            case 0: {
                if(auto* owner=TaskbarMonitorWindow::activeMenuOwner_){
                    MENUITEMINFOW monitorItem{sizeof(monitorItem)};monitorItem.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_STATE;
                    GetMenuItemInfoW(owner->contextMenu_,0,TRUE,&monitorItem);
                    RECT monitorRect{};GetMenuItemRect(nullptr,owner->contextMenu_,0,&monitorRect);
                    if(monitorItem.wID!=4209||!monitorItem.hSubMenu||!(monitorItem.fState&MFS_CHECKED)||
                        owner->MenuCommandAtPoint({monitorRect.left+45,(monitorRect.top+monitorRect.bottom)/2})!=4209||
                        owner->MenuCommandAtPoint({monitorRect.right-5,(monitorRect.top+monitorRect.bottom)/2})!=0)menuFailure=true;
                    MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_SUBMENU;GetMenuItemInfoW(owner->contextMenu_,1,TRUE,&item);
                    RECT r{};GetMenuItemRect(nullptr,owner->contextMenu_,1,&r);
                    if(item.wID!=4203||!item.hSubMenu||owner->MenuCommandAtPoint({r.left+45,(r.top+r.bottom)/2})!=4203||owner->MenuCommandAtPoint({r.right-5,(r.top+r.bottom)/2})!=0)menuFailure=true;
                    wchar_t label[128]{};GetMenuStringW(owner->contextMenu_,1,label,128,MF_BYPOSITION);if(wcscmp(label,L"中英翻译")!=0||GetMenuItemCount(owner->translationMenu_)!=5)menuFailure=true;
                    for(int i=0;i<3;++i)if(GetMenuItemID(owner->translationMenu_,i)!=4220+UINT(i))menuFailure=true;
                    MENUITEMINFOW separator{sizeof(separator)};separator.fMask=MIIM_FTYPE;GetMenuItemInfoW(owner->contextMenu_,8,TRUE,&separator);
                    if(!(separator.fType&MFT_SEPARATOR)||GetMenuItemID(owner->contextMenu_,7)!=4207||GetMenuItemID(owner->contextMenu_,9)!=4210||GetMenuItemID(owner->contextMenu_,10)!=4201)menuFailure=true;
                }
                std::set<UINT> commands;
                if(auto* owner=TaskbarMonitorWindow::activeMenuOwner_) for(int i=0;i<GetMenuItemCount(owner->contextMenu_);++i) {
                    MENUITEMINFOW info{sizeof(info)}; info.fMask=MIIM_FTYPE;
                    GetMenuItemInfoW(owner->contextMenu_,i,TRUE,&info);
                    if(info.fType&MFT_OWNERDRAW) menuFailure=true;
                    UINT id=GetMenuItemID(owner->contextMenu_,i);
                    if(id && id!=UINT(-1) && !commands.insert(id).second) menuFailure=true;
                }
            } Key('Z'); Key('Z',true); break;
            case 1: Key(VK_MENU); break;
            case 2: Key(VK_MENU,true); break;
            case 3: Key(VK_CONTROL); break;
            case 4: Key(VK_MENU); break;
            case 5: Key(VK_F9); Key(VK_F9,true); break;
            default: menuFailure=true; EndMenu(); break;
            }
            return 0;
        }
        return DefWindowProcW(w,m,wp,lp);
    }
    static void RunMenu() {
        AppSettings settings; auto instance=GetModuleHandleW(nullptr);
        TaskbarMonitorWindow monitor(instance,settings);
        WNDCLASSW wc{}; wc.hInstance=instance; wc.lpfnWndProc=MenuController; wc.lpszClassName=L"PcTool.MenuTest"; RegisterClassW(&wc);
        HWND owner=CreateWindowExW(0,wc.lpszClassName,L"PcTool menu input test",WS_OVERLAPPEDWINDOW,100,100,500,400,nullptr,nullptr,instance,nullptr);
        ShowWindow(owner,SW_SHOW); SetForegroundWindow(owner); monitor.controller_=owner;
        if(!RegisterHotKey(owner,81,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F9)) throw std::runtime_error("Menu test hotkey unavailable");
        SetTimer(owner,1,200,nullptr); monitor.ShowContextMenu({200,200}); KillTimer(owner,1);
        Key(VK_F9,true); Key(VK_MENU,true); Key(VK_CONTROL,true);
        UnregisterHotKey(owner,81);
        // Escape and an outside click must still dismiss the menu normally.
        SetTimer(owner,2,200,[](HWND w,UINT,UINT_PTR id,DWORD){ KillTimer(w,id); Key(VK_ESCAPE); Key(VK_ESCAPE,true); });
        monitor.ShowContextMenu({200,200});
        menuStep=0;
        SetTimer(owner,4,180,[](HWND w,UINT,UINT_PTR id,DWORD){
            auto* active=TaskbarMonitorWindow::activeMenuOwner_;if(!active){menuFailure=true;KillTimer(w,id);EndMenu();return;}
            if(menuStep++==0){RECT r{};GetMenuItemRect(nullptr,active->contextMenu_,1,&r);SetCursorPos(r.right-8,(r.top+r.bottom)/2);}
            else {RECT r{};if(GetMenuItemRect(nullptr,active->translationMenu_,4,&r)&&r.right>r.left){SetCursorPos((r.left+r.right)/2,(r.top+r.bottom)/2);INPUT mouse[2]{};mouse[0].type=mouse[1].type=INPUT_MOUSE;mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(2,mouse,sizeof(INPUT));KillTimer(w,id);}else if(menuStep>15){menuFailure=true;KillTimer(w,id);EndMenu();}}
        });
        monitor.ShowContextMenu({200,200});KillTimer(owner,4);
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        SetTimer(owner,3,200,[](HWND w,UINT,UINT_PTR id,DWORD){ KillTimer(w,id); SetCursorPos(120,150); INPUT mouse[2]{}; mouse[0].type=mouse[1].type=INPUT_MOUSE; mouse[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; mouse[1].mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(2,mouse,sizeof(INPUT)); });
        monitor.ShowContextMenu({200,200});
        DestroyWindow(owner); monitor.controller_=nullptr;
        if(menuFailure || !hotkeyWithMenu||!sourcesRequested) throw std::runtime_error("Menu input, split arrow hit or translation settings dispatch failed");
        std::cout<<"MENU PASS: ordinary keys, Alt, screenshot chord, Escape, outside click, translation text/arrow hit and settings submenu dispatch\n";
    }
    static void Run() {
        // Redirect HKCU in this test process only; never modify the user's settings.
        const auto testKey=L"Software\\PcToolMonitorTest-"+std::to_wstring(GetCurrentProcessId());
        HKEY isolated{};
        if(RegCreateKeyExW(HKEY_CURRENT_USER,testKey.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&isolated,nullptr)!=ERROR_SUCCESS)
            throw std::runtime_error("Cannot create isolated settings fixture");
        if(RegOverridePredefKey(HKEY_CURRENT_USER,isolated)!=ERROR_SUCCESS){RegCloseKey(isolated);throw std::runtime_error("Cannot isolate settings");}
        AppSettings saved; saved.Load();bool persistenceOk=saved.MonitorInTaskbar();
        persistenceOk=hotkeys::Valid(saved.Hotkeys())&&saved.Hotkeys()==hotkeys::Defaults&&persistenceOk;
        auto custom=hotkeys::Defaults;custom[4]={MOD_CONTROL|MOD_SHIFT,'T'};saved.SetHotkeys(custom);
        saved.SetMonitorInTaskbar(false);persistenceOk=saved.Save()&&persistenceOk;
        AppSettings loaded;loaded.Load();persistenceOk=!loaded.MonitorInTaskbar()&&persistenceOk;
        persistenceOk=loaded.Hotkeys()==custom&&persistenceOk;
        // A five-entry installation must retain its old meanings; new actions default unbound.
        HKEY legacy{};if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\PcTool",0,KEY_SET_VALUE,&legacy)==ERROR_SUCCESS){for(int i=5;i<9;++i)RegDeleteValueW(legacy,(L"Hotkey"+std::to_wstring(i)).c_str());RegCloseKey(legacy);}else persistenceOk=false;
        AppSettings upgraded;upgraded.Load();persistenceOk=upgraded.Hotkeys()==custom&&persistenceOk;
        for(int i=5;i<9;++i)custom[i]={MOD_CONTROL|MOD_ALT|MOD_SHIFT,UINT(VK_F13+i-5)};
        loaded.SetHotkeys(custom);persistenceOk=loaded.Save()&&persistenceOk;upgraded.Load();persistenceOk=upgraded.Hotkeys()==custom&&persistenceOk;
        loaded.SetHotkeys({});persistenceOk=loaded.Save()&&persistenceOk;upgraded.Load();persistenceOk=upgraded.Hotkeys()==hotkeys::Bindings{}&&persistenceOk;
        loaded.SetMonitorInTaskbar(true);persistenceOk=loaded.Save()&&persistenceOk;
        AppSettings restored;restored.Load();persistenceOk=restored.MonitorInTaskbar()&&persistenceOk;
        RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);RegCloseKey(isolated);RegDeleteTreeW(HKEY_CURRENT_USER,testKey.c_str());
        if(!persistenceOk)throw std::runtime_error("Monitor placement default or persistence failed");
        AppSettings settings;
        auto instance=GetModuleHandleW(nullptr);
        TaskbarMonitorWindow monitor(instance,settings);
        monitor.RegisterWindowClass();
        WNDCLASSW wc{}; wc.hInstance=instance; wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"PcTool.FakeTaskbar"; RegisterClassW(&wc);
        HWND shell=CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,50,500,3000,100,nullptr,nullptr,instance,nullptr);
        HWND parent=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,100,0,2800,100,shell,nullptr,instance,nullptr);
        HWND task=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,40,0,2650,100,parent,nullptr,instance,nullptr);
        HWND tray=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,2500,0,500,100,shell,nullptr,instance,nullptr);
        // Hidden top-level fixture still has WS_VISIBLE children; explicitly show
        // off-screen so IsWindowVisible follows real taskbar semantics.
        SetWindowPos(shell,nullptr,-10000,-10000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW|SWP_NOACTIVATE);
        monitor.taskbar_=shell; monitor.parent_=parent; monitor.taskList_=task; monitor.notifyArea_=tray; monitor.classicTaskbar_=true;
        monitor.window_=CreateWindowExW(0,L"PcTool.TaskbarMonitor",L"",WS_CHILD|WS_VISIBLE,0,0,1,1,parent,nullptr,instance,&monitor);
        const HWND existingMonitor=monitor.window_;
        if(!monitor.Create(shell)||monitor.window_!=existingMonitor)throw std::runtime_error("Repeated Create rebuilt the attached monitor");
        constexpr MonitorItem items[]={MonitorItem::UploadSpeed,MonitorItem::DownloadSpeed,MonitorItem::CpuUsage,MonitorItem::MemoryUsage,MonitorItem::CpuFrequency};
        for(UINT dpi:{96,144,192}) {
            monitor.dpi_=dpi; monitor.RecreateFont();
            NONCLIENTMETRICSW menuMetrics{sizeof(menuMetrics)};
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(menuMetrics),&menuMetrics,0,dpi);
            HFONT menuFont=CreateFontIndirectW(&menuMetrics.lfMenuFont); HDC menuDc=GetDC(shell); auto oldMenuFont=SelectObject(menuDc,menuFont);
            auto label=monitor.MenuShortcutText(L"剪切板历史",L"Alt+C");
            if(monitor.MenuShortcutText(L"输入翻译",L"")!=L"输入翻译")throw std::runtime_error("Unbound menu label retains shortcut padding");
            const auto longLabel=monitor.MenuShortcutText(L"划词翻译",L"Ctrl+Alt+Shift+F24");
            if(longLabel.find(L"Ctrl+Alt+Shift+F24")==std::wstring::npos)throw std::runtime_error("Long menu binding clipped in label");
            auto padding=label.substr(5,label.size()-10); SIZE gap{};
            GetTextExtentPoint32W(menuDc,padding.c_str(),int(padding.size()),&gap);
            SelectObject(menuDc,oldMenuFont); DeleteObject(menuFont); ReleaseDC(shell,menuDc);
            if(gap.cx!=monitor.Scale(5)) throw std::runtime_error("Native menu shortcut gap is not 5 DIP");

            for(int mask=1;mask<32;++mask) {
                for(int i=0;i<5;++i) if((mask&(1<<i)) && !settings.IsItemVisible(items[i])) settings.ToggleItem(items[i]);
                for(int i=0;i<5;++i) if(!(mask&(1<<i)) && settings.IsItemVisible(items[i])) settings.ToggleItem(items[i]);
                for(int trayX:{2500,2200,2700}) {
                    MoveWindow(tray,trayX,0,3000-trayX,100,FALSE);
                    monitor.AdjustClassicTaskbar(false);
                    RECT bounds{},trayBounds{},list{}; GetWindowRect(monitor.window_,&bounds); GetWindowRect(tray,&trayBounds); GetWindowRect(task,&list);
                    if(bounds.right>trayBounds.left || list.right!=bounds.left || bounds.right-bounds.left!=monitor.CalculateRequiredWidth()) throw std::runtime_error("Taskbar reservation crossed tray or lost measured width");
                    HDC dc=GetDC(monitor.window_); auto font=SelectObject(dc,monitor.font_);
                    int x=monitor.Scale(3); const auto columns=monitor.BuildColumns(dc);
                    for(const auto& column:columns) {
                        for(const auto& text:{column.firstText,column.secondText}) { SIZE extent{}; GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&extent); if(extent.cx>column.width) throw std::runtime_error("Column clipped its text"); }
                        x+=column.width+monitor.Scale(5);
                    }
                    if(x+monitor.Scale(10)>bounds.right-bounds.left) throw std::runtime_error("Missing right padding");
                    SelectObject(dc,font); ReleaseDC(monitor.window_,dc);
                    monitor.AdjustClassicTaskbar(false); RECT after{}; GetWindowRect(monitor.window_,&after);
                    if(!EqualRect(&bounds,&after)) throw std::runtime_error("Stable layout moved");
                }
            }
        }
        monitor.Destroy(); DestroyWindow(shell);
        std::cout<<"TASKBAR PASS: 31 item combinations, 3 DPI values, changing tray, nonzero parent offset, stable bounds and full text\n";
    }
};
int main(int argc,char**) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try { if(argc>1) TaskbarLayoutTest::RunMenu(); else TaskbarLayoutTest::Run(); return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
