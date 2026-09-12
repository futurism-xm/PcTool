#include "shared/platform/app_storage.h"
#include "system_tools/paint.h"
#include "system_tools/task_manager.h"
#include "archive/archive_service.h"
#include "app/app_messages.h"
#include "app/app_settings.h"
#include "capture/screenshot_overlay.h"
#include "monitor/taskbar_monitor_window.h"
#include "monitor/monitor_trace.h"
#include "app/capture_tools.h"
#include "clipboard/clipboard_history.h"
#include "translation/translation_controller.h"
#include "translation/source_settings.h"
#include "shared/ui/resource.h"
#include "shared/ui/tray_icon.h"
#include "hotkeys/hotkey_settings.h"
#include <roapi.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <cstdlib>
#include <cwchar>

namespace {
constexpr wchar_t kControllerClassName[] = L"PcTool.BackgroundController";
constexpr UINT_PTR kReattachTimer = 1;
constexpr UINT_PTR kTrayExitTimer = 2;
constexpr int kScreenshotHotkeyId = 1;
constexpr int kClipboardHotkeyId = 2;

bool IsTrafficMonitorRunning() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    bool found = false;
    if (Process32FirstW(snapshot, &process)) {
        do {
            if (_wcsicmp(process.szExeFile, L"TrafficMonitor.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    return found;
}

bool HandlePinnedWindowCommand() {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (!arguments) {
        return false;
    }
    const bool centerCommand = argumentCount == 3 &&
        _wcsicmp(arguments[1], kCenterPinnedWindowArgument) == 0;
    if (centerCommand) {
        wchar_t* end = nullptr;
        const unsigned long long rawHandle = wcstoull(
            arguments[2], &end, 10);
        if (rawHandle != 0 && end && *end == L'\0') {
            const HWND pinnedWindow = reinterpret_cast<HWND>(
                static_cast<UINT_PTR>(rawHandle));
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(pinnedWindow, WM_SYSCOMMAND,
                kCenterPinnedWindowCommand, 0,
                SMTO_ABORTIFHUNG, 1000, &ignored);
        }
    }
    LocalFree(arguments);
    return centerCommand;
}

class ApplicationController final {
public:
    ApplicationController(HINSTANCE instance, AppSettings& settings)
        : instance_(instance), settings_(settings), taskbarMonitor_(instance, settings),
          screenshotOverlay_(instance) { screenshotOverlay_.SetCaptureTools(&captureTools_); }

    bool Create() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = kControllerClassName;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        window_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kControllerClassName, L"PcTool Background Controller", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, instance_, this);
        return window_ != nullptr;
    }

private:
    static constexpr UINT TrayMessage=WM_APP+107;
    bool AddTray(){return tray_.Show();}
    bool RemoveTray(){return tray_.Remove();}
    void CloseWithTrayCleanup(){
        closing_=true;KillTimer(window_,kReattachTimer);taskbarMonitor_.Destroy();
        if(RemoveTray()||++exitCleanupAttempts_>=5){KillTimer(window_,kTrayExitTimer);DestroyWindow(window_);}
        else SetTimer(window_,kTrayExitTimer,200,nullptr);
    }
    void UpdateMonitorDisplay(){
        if(closing_)return;
        TraceMonitor(settings_.MonitorInTaskbar()?"display.taskbar.begin":"display.tray.begin");
        KillTimer(window_,kReattachTimer);
        if(settings_.MonitorInTaskbar()){
            // Explorer can synchronously resize/notify our embedded child when
            // its tray changes. Remove the icon before attaching that child.
            if(!RemoveTray()){SetTimer(window_,kReattachTimer,250,nullptr);return;}
            if(taskbarMonitor_.Create(window_)){TraceMonitor("display.taskbar.created");TraceMonitor("display.taskbar.end");return;}
            AddTray(); // Keep a reachable menu while Explorer is unavailable.
        }else{
            // Likewise detach first: Shell_NotifyIcon while the child belongs
            // to Explorer creates a cross-thread wait during tray relayout.
            taskbarMonitor_.Destroy();
            if(AddTray()){TraceMonitor("display.tray.added");TraceMonitor("display.tray.end");return;}
            // A failed ADD may still have reached Explorer. Clean that identity
            // before attaching a taskbar child and trying again later.
            if(RemoveTray())taskbarMonitor_.Create(window_);
        }
        SetTimer(window_,kReattachTimer,1500,nullptr);
    }
    void UpdateTranslationHotkeys() {
        translation_.Configure(window_,settings_.TranslationEnabled()&&!editingHotkeys_,[this]{
            if(captureTools_.FocusActive())return false;
            if(screenshotOverlay_.FocusActive())return false;
            return true;
        },true,settings_.Hotkeys());
    }
    void UpdateClipboardHotkey(){
        if(clipboardHotkeyRegistered_)UnregisterHotKey(window_,kClipboardHotkeyId);
        clipboardHotkeyRegistered_=false;if(editingHotkeys_)return;
        const auto key=settings_.Hotkeys()[1];
        if(hotkeys::Empty(key))return;
        clipboardHotkeyRegistered_=RegisterHotKey(window_,kClipboardHotkeyId,key.modifiers|MOD_NOREPEAT,key.key)!=FALSE;
        if(!clipboardHotkeyRegistered_)MessageBoxW(window_,(L"无法注册 "+hotkeys::Text(key)+L"，快捷键可能已被其他软件占用。仍可从菜单打开剪切板历史。").c_str(),L"PcTool",MB_OK|MB_ICONWARNING);
    }
    void UpdateExtraHotkeys(){
        for(int i=0;i<4;++i){UnregisterHotKey(window_,20+i);if(editingHotkeys_||hotkeys::Empty(settings_.Hotkeys()[5+i]))continue;const auto binding=settings_.Hotkeys()[5+i];if(!RegisterHotKey(window_,20+i,binding.modifiers|MOD_NOREPEAT,binding.key))MessageBoxW(window_,(std::wstring(hotkeys::Names[5+i])+L"：无法注册 "+hotkeys::Text(binding)+L"，请检查快捷键占用。").c_str(),L"PcTool",MB_OK|MB_ICONWARNING);}
    }
    void SetHotkeyCapture(bool capture){
        if(editingHotkeys_==capture)return;editingHotkeys_=capture;if(!IsWindow(window_))return;
        UpdateScreenshotHotkey(false);UpdateClipboardHotkey();UpdateTranslationHotkeys();UpdateExtraHotkeys();
    }
    void ShowHotkeySettings(){
        if(hotkeySettingsOpen_){if(auto w=FindWindowW(L"PcTool.HotkeySettings",nullptr)){ShowWindow(w,SW_RESTORE);SetForegroundWindow(w);}return;}
        hotkeySettingsOpen_=true;
        hotkeys::ShowSettings(window_,settings_.Hotkeys(),[this](const hotkeys::Bindings& values,std::wstring& error){
            const bool wasCapturing=editingHotkeys_;SetHotkeyCapture(true);
            const bool saved=[&]{
            if(!hotkeys::Valid(values)){error=L"快捷键无效或重复，请检查绑定。";return false;}
            std::vector<int> registered;size_t failed=values.size();DWORD failure=0;
            for(size_t i=0;i<values.size();++i){if(hotkeys::Empty(values[i]))continue;if(!RegisterHotKey(window_,800+int(i),values[i].modifiers|MOD_NOREPEAT,values[i].key)){failure=GetLastError();failed=i;break;}registered.push_back(800+int(i));}
            for(int id:registered)UnregisterHotKey(window_,id);
            if(failed!=values.size()){error=std::wstring(hotkeys::Names[failed])+L"："+hotkeys::Text(values[failed])+L" 无法注册，可能被系统或其他程序占用（"+std::to_wstring(failure)+L"）。";return false;}
            const auto previous=settings_.Hotkeys();settings_.SetHotkeys(values);
            if(!settings_.Save()){settings_.SetHotkeys(previous);settings_.Save();error=L"保存快捷键失败，请重试。";return false;}return true;
            }();SetHotkeyCapture(wasCapturing);return saved;
        },[this](bool capture){SetHotkeyCapture(capture);});
        hotkeySettingsOpen_=false;editingHotkeys_=false;if(!IsWindow(window_))return;
        UpdateScreenshotHotkey(true);UpdateClipboardHotkey();UpdateTranslationHotkeys();UpdateExtraHotkeys();
    }
    void UpdateScreenshotHotkey(bool showError) {
        if (!settings_.ScreenshotEnabled()||editingHotkeys_||hotkeys::Empty(settings_.Hotkeys()[0])) {
            if (screenshotHotkeyRegistered_) {
                UnregisterHotKey(window_, kScreenshotHotkeyId);
                screenshotHotkeyRegistered_ = false;
            }
            screenshotOverlay_.Cancel();
            return;
        }

        if (screenshotHotkeyRegistered_) {
            return;
        }
        screenshotHotkeyRegistered_ = RegisterHotKey(
            window_, kScreenshotHotkeyId,
            settings_.Hotkeys()[0].modifiers | MOD_NOREPEAT, settings_.Hotkeys()[0].key) != FALSE;
        if (!screenshotHotkeyRegistered_ && showError) {
            MessageBoxW(window_,
                (L"无法注册 "+hotkeys::Text(settings_.Hotkeys()[0])+L" 截图快捷键。\n\n该快捷键可能正被其他软件占用，请在快捷键管理中修改。").c_str(),
                L"PcTool - 截图快捷键", MB_OK | MB_ICONWARNING);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        ApplicationController* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ApplicationController*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ApplicationController*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        if (!self) {
            return DefWindowProcW(window, message, wParam, lParam);
        }

        if (message == self->taskbarCreatedMessage_) {
            self->tray_.ExplorerRestarted();
            self->taskbarMonitor_.Destroy();
            self->UpdateMonitorDisplay();
            return 0;
        }

        switch (message) {
        case WM_CREATE:
            {
                wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
                self->tray_.Bind(window,LoadIconW(self->instance_,MAKEINTRESOURCEW(IDI_APP_ICON)),TrayMessage,shared_ui::TrayIdentity(path));
            }
            self->taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
            self->UpdateMonitorDisplay();
            self->UpdateScreenshotHotkey(true);
            self->UpdateTranslationHotkeys();
            self->clipboardHistory_.Start();
            self->UpdateClipboardHotkey();
            self->UpdateExtraHotkeys();self->menuSourceTracker_.Start();
            return 0;

        case WM_HOTKEY:
            if(self->editingHotkeys_)return 0;
            if(wParam>=20&&wParam<24){constexpr UINT commands[]={kLaunchTaskManagerMessage,kLaunchPaintMessage,kOpenArchiveManagerMessage,kShowHotkeySettingsMessage};PostMessageW(window,commands[wParam-20],0,0);return 0;}
            if(self->translation_.HandleHotkey(wParam))return 0;
            if (wParam == kClipboardHotkeyId) {
                SendMessageW(window, kShowClipboardHistoryMessage, 0, 0);
                return 0;
            }
            if (wParam == kScreenshotHotkeyId && self->settings_.ScreenshotEnabled()) {
                if(self->translation_.FocusCapture())return 0;
                if (!self->screenshotOverlay_.Start()) {
                    MessageBoxW(window, L"无法启动截图功能。", L"PcTool",
                        MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);

        case kShowClipboardHistoryMessage:
            if(self->clipboardHistory_.Start()) self->clipboardHistory_.Show();
            else MessageBoxW(window,L"无法启动剪切板监听，请重试。",L"PcTool",MB_OK|MB_ICONWARNING);
            return 0;
        case kLaunchPaintMessage:
            LaunchSystemPaint(window);
            return 0;
        case kLaunchTaskManagerMessage:
            LaunchTaskManager(window);return 0;
        case kPrepareTranslationMenuMessage:
            self->menuSource_=self->menuSourceTracker_.Freeze();return 0;
        case kInvokeTranslationMenuMessage:
            if(wParam<3)self->translation_.Invoke(static_cast<translation::Entry>(wParam),true,self->menuSource_);return 0;
        case kOpenArchiveManagerMessage:
            archive::OpenManager(window);
            return 0;
        case kShowTranslationSourcesMessage:
            self->translation_.ShowSourceSettings(window);
            return 0;
        case kShowHotkeySettingsMessage:
            self->ShowHotkeySettings();return 0;
        case kAppSettingsChangedMessage:
            self->UpdateScreenshotHotkey(true);
            self->UpdateTranslationHotkeys();
            return 0;
        case kMonitorDisplayChangedMessage:
            self->UpdateMonitorDisplay();return 0;
        case TrayMessage:
            if(LOWORD(lParam)==WM_CONTEXTMENU||LOWORD(lParam)==NIN_SELECT||LOWORD(lParam)==NIN_KEYSELECT){POINT point{};GetCursorPos(&point);self->taskbarMonitor_.ShowMenu(window,point);}
            return 0;

        case WM_TIMER:
            if (wParam == kReattachTimer) self->UpdateMonitorDisplay();
            if (wParam == kTrayExitTimer) self->CloseWithTrayCleanup();
            return 0;

        case WM_CLOSE:
            self->CloseWithTrayCleanup();
            return 0;

        case WM_DESTROY:
            for(int i=0;i<4;++i)UnregisterHotKey(window,20+i);
            self->taskbarMonitor_.Destroy();
            self->RemoveTray();
            self->translation_.Shutdown();
            KillTimer(window, kReattachTimer);
            KillTimer(window, kTrayExitTimer);
            if (self->clipboardHotkeyRegistered_) {
                UnregisterHotKey(window, kClipboardHotkeyId);
                self->clipboardHotkeyRegistered_ = false;
            }
            self->clipboardHistory_.Shutdown();
            self->captureTools_.Shutdown();
            self->screenshotOverlay_.Cancel();
            if (self->screenshotHotkeyRegistered_) {
                UnregisterHotKey(window, kScreenshotHotkeyId);
                self->screenshotHotkeyRegistered_ = false;
            }
            self->taskbarMonitor_.Destroy();
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    HINSTANCE instance_{};
    HWND window_{};
    AppSettings& settings_;
    TaskbarMonitorWindow taskbarMonitor_;
    ScreenshotOverlay screenshotOverlay_;
    CaptureTools captureTools_;
    ClipboardHistory clipboardHistory_;
    translation::TranslationController translation_;
    bool screenshotHotkeyRegistered_{};
    bool clipboardHotkeyRegistered_{};
    shared_ui::TrayIcon tray_;
    bool closing_{};
    unsigned exitCleanupAttempts_{};
    bool editingHotkeys_{};
    bool hotkeySettingsOpen_{};
    shared_platform::MenuSourceTracker menuSourceTracker_;
    shared_platform::MenuSource menuSource_{};
    UINT taskbarCreatedMessage_{};
};
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    // Check before every auxiliary entry point, including --archive and pinned windows.
    if (GetFileAttributesW((app_storage::Root() / L".pctool-uninstalling").c_str()) != INVALID_FILE_ATTRIBUTES)
        return ERROR_INSTALL_ALREADY_RUNNING;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv && argc>=2 && wcscmp(argv[1],L"--archive")==0) {
        const std::wstring path=argc>=3?argv[2]:L"";LocalFree(argv);
        return archive::OpenManager(nullptr,path)?0:1;
    }
    if(argv)LocalFree(argv);
    if (HandlePinnedWindowCommand()) {
        return 0;
    }
    if (commandLine && wcsstr(commandLine, L"--exit-existing")) {
        if (HWND controller = FindWindowW(kControllerClassName, nullptr)) {
            PostMessageW(controller, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    if (FindWindowW(kControllerClassName, nullptr)) {
        return 0;
    }

    HANDLE instanceMutex=CreateMutexW(nullptr,TRUE,(L"Local\\PcTool.Instance."+app_storage::UserSid()).c_str());
    if(!instanceMutex||GetLastError()==ERROR_ALREADY_EXISTS){if(instanceMutex)CloseHandle(instanceMutex);return 0;}
    struct InstanceGuard{HANDLE value;~InstanceGuard(){ReleaseMutex(value);CloseHandle(value);}} instanceGuard{instanceMutex};
    try{app_storage::Initialize();app_storage::CleanCache();}
    catch(...){MessageBoxW(nullptr,L"无法写入安装目录中的 Data/Cache，请检查目录权限或重新安装。",L"PcTool",MB_OK|MB_ICONERROR);return 1;}
    // Both applications resize MSTaskSwWClass to reserve taskbar space. They
    // cannot safely own that Explorer window at the same time.
    if (IsTrafficMonitorRunning()) {
        MessageBoxW(nullptr,
            L"检测到 TrafficMonitor 正在运行。\n\n"
            L"PcTool 和 TrafficMonitor 会同时调整任务栏区域，从而造成持续闪烁。"
            L"请先退出 TrafficMonitor，再启动 PcTool。",
            L"PcTool - 任务栏监控冲突", MB_OK | MB_ICONWARNING);
        return 2;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&controls);

    const HRESULT comResult = RoInitialize(RO_INIT_SINGLETHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comResult);

    AppSettings settings;
    try{settings.Load();}catch(...){MessageBoxW(nullptr,L"无法读取安装目录中的配置文件。",L"PcTool",MB_OK|MB_ICONERROR);return 1;}
    settings.ApplyAutoStart();
    settings.Save();

    const int exitCode = [&]() {
        ApplicationController controller(instance, settings);
        if (!controller.Create()) {
            MessageBoxW(nullptr, L"任务栏监控窗口创建失败。", L"PcTool", MB_OK | MB_ICONERROR);
            return 1;
        }
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if(translation::HandleSourceSettingsMessage(message))continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }(); // All tool windows and worker COM objects die before the apartment.

    if (shouldUninitializeCom) {
        RoUninitialize();
    }
    return exitCode;
}
