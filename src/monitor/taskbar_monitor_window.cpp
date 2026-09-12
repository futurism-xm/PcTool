#include "monitor/taskbar_monitor_window.h"

#include "app/app_messages.h"
#include "monitor/monitor_trace.h"

#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <utility>

namespace {
constexpr wchar_t kWindowClassName[] = L"PcTool.TaskbarMonitor";
constexpr UINT_PTR kSampleTimer = 1;
constexpr UINT kSampleIntervalMs = 1000;
constexpr int kColumnGapDip = 5;
constexpr double kDisplayedRateDivisor = 8.0;

constexpr UINT kMonitorCommandBase = 4100;
constexpr UINT kTaskManagerCommand = 4200;
constexpr UINT kClipboardHistoryCommand = 4205;
constexpr UINT kPaintCommand = 4206;
constexpr UINT kArchiveCommand = 4207;
constexpr UINT kAutoStartCommand = 4201;
constexpr UINT kExitCommand = 4202;
constexpr UINT kTranslationCommand = 4203;
constexpr UINT kTranslationSourcesCommand = 4208;
constexpr UINT kScreenshotCommand = 4204;
constexpr UINT kMonitorDisplayCommand = 4209;
constexpr UINT kHotkeySettingsCommand = 4210;
constexpr UINT kTranslationEntryBase = 4220;

struct MenuItemInfo {
    MonitorItem item;
    const wchar_t* label;
};

constexpr std::array<MenuItemInfo, 5> kMenuItems{{
    {MonitorItem::UploadSpeed, L"上传速度"},
    {MonitorItem::DownloadSpeed, L"下载速度"},
    {MonitorItem::CpuUsage, L"CPU 利用率"},
    {MonitorItem::MemoryUsage, L"内存利用率"},
    {MonitorItem::CpuFrequency, L"CPU 频率"},
}};

bool EqualRectValue(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

int RectWidth(const RECT& rect) noexcept {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) noexcept {
    return rect.bottom - rect.top;
}
}  // namespace

thread_local TaskbarMonitorWindow* TaskbarMonitorWindow::activeMenuOwner_ = nullptr;

TaskbarMonitorWindow::TaskbarMonitorWindow(
    HINSTANCE instance, AppSettings& settings)
    : instance_(instance), settings_(settings) {}

TaskbarMonitorWindow::~TaskbarMonitorWindow() {
    Destroy();
}

bool TaskbarMonitorWindow::RegisterWindowClass() const {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass)) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool TaskbarMonitorWindow::FindTaskbarWindows() {
    taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar_) {
        return false;
    }

    HWND rebar = FindWindowExW(taskbar_, nullptr, L"ReBarWindow32", nullptr);
    if (!rebar) {
        rebar = FindWindowExW(taskbar_, nullptr, L"WorkerW", nullptr);
    }

    if (rebar) {
        taskList_ = FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr);
        if (!taskList_) {
            taskList_ = FindWindowExW(rebar, nullptr, L"MSTaskListWClass", nullptr);
        }
    }

    classicTaskbar_ = rebar && taskList_;
    parent_ = classicTaskbar_ ? rebar : taskbar_;
    notifyArea_ = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr);
    dpi_ = GetDpiForWindow(taskbar_);
    if (dpi_ == 0) {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    return parent_ != nullptr;
}

bool TaskbarMonitorWindow::Create(HWND controller) {
    TraceMonitor("create.begin");
    if(window_&&IsWindow(window_)&&parent_&&IsWindow(parent_)){controller_=controller;return true;}
    Destroy();
    TraceMonitor("create.destroyed");
    controller_ = controller;
    if (!RegisterWindowClass() || !FindTaskbarWindows()) {
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClassName, L"PcTool Monitor",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 1, 1, parent_, nullptr, instance_, this);
    if (!window_) {
        return false;
    }

    RecreateFont();
    TraceMonitor("create.sample.begin");
    monitor_.Sample(settings_.IsItemVisible(MonitorItem::CpuFrequency));
    TraceMonitor("create.sample.end");
    AdjustPosition(true);
    TraceMonitor("create.positioned");
    SetTimer(window_, kSampleTimer, kSampleIntervalMs, nullptr);
    InvalidateRect(window_, nullptr, TRUE);
    return true;
}

void TaskbarMonitorWindow::Destroy() {
    TraceMonitor("destroy.begin");
    if (window_ && IsWindow(window_)) {
        KillTimer(window_, kSampleTimer);
    }
    RestoreTaskList();
    TraceMonitor("destroy.restored");
    if (window_ && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    window_ = nullptr;
    TraceMonitor("destroy.window-gone");
    taskbar_ = nullptr;
    parent_ = nullptr;
    taskList_ = nullptr;
    notifyArea_ = nullptr;
    classicTaskbar_ = false;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

LRESULT CALLBACK TaskbarMonitorWindow::WindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    TaskbarMonitorWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TaskbarMonitorWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TaskbarMonitorWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::wstring TaskbarMonitorWindow::MenuShortcutText(const wchar_t* label,const wchar_t* shortcut) const {
    if(!shortcut||!*shortcut)return label;
    // Keep MFT_STRING: making even one row owner-drawn changes Windows' menu
    // rendering, including check marks on otherwise native rows.
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,dpi_);
    HFONT font=CreateFontIndirectW(&metrics.lfMenuFont); HDC dc=GetDC(window_);
    auto old=SelectObject(dc,font);
    const int gap=Scale(5);
    std::vector<std::wstring> padding(size_t(gap)+1);
    std::vector<bool> reachable(size_t(gap)+1,false); reachable[0]=true;
    for(int width=1;width<=gap;++width) for(wchar_t space:{L' ',L'\u2009',L'\u200a'}) {
        SIZE extent{}; GetTextExtentPoint32W(dc,&space,1,&extent);
        if(extent.cx>0 && extent.cx<=width && reachable[width-extent.cx]) {
            padding[width]=padding[width-extent.cx]+space; reachable[width]=true; break;
        }
    }
    int width=gap; while(width>0 && !reachable[width]) --width;
    SelectObject(dc,old); DeleteObject(font); ReleaseDC(window_,dc);
    return std::wstring(label)+padding[width]+shortcut;
}

LRESULT CALLBACK TaskbarMonitorWindow::MenuMessageHook(
    int code, WPARAM wParam, LPARAM lParam) {
    TaskbarMonitorWindow* self = activeMenuOwner_;
    if (code == MSGF_MENU && self) {
        const auto* message = reinterpret_cast<const MSG*>(lParam);
        // Native menus treat Alt and character keys as dismissal/mnemonics.
        // Keep this settings menu visible while a screenshot chord is entered.
        // Registered hotkeys still reach their owner before menu teardown, so
        // the screenshot can freeze the desktop with the menu in it.
        if (message->message == WM_HOTKEY && message->hwnd == self->controller_) {
            DispatchMessageW(message);
            return 1;
        }
        if (message->message == WM_SYSKEYDOWN || message->message == WM_SYSKEYUP ||
            message->message == WM_CHAR || message->message == WM_SYSCHAR ||
            message->message == WM_KEYUP) {
            return 1;
        }
        if (message->message == WM_KEYDOWN) {
            const auto key = message->wParam;
            const bool navigation = key == VK_ESCAPE || key == VK_RETURN || key == VK_SPACE ||
                key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
                key == VK_HOME || key == VK_END;
            if (!navigation || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) {
                return 1;
            }
        }
        UINT command = 0;
        if (message->message == WM_LBUTTONUP || message->message == WM_RBUTTONUP) {
            command = self->MenuCommandAtPoint(message->pt);
        } else if (message->message == WM_KEYDOWN &&
                   (message->wParam == VK_RETURN || message->wParam == VK_SPACE)) {
            command = self->HighlightedMenuCommand();
        }

        if(command==kMonitorDisplayCommand){
            // Shell_NotifyIcon / Explorer child destruction cannot run while
            // the native menu still owns capture across the taskbar's threads.
            // Unwind TrackPopupMenu first, then post the placement change.
            self->deferredMenuCommand_=command;
            EndMenu();
            return 1;
        }
        if (self->KeepsMenuOpen(command)) {
            self->HandleMenuCommand(command);
            self->RefreshMenuCheckmark(command);
            return 1;
        }
    }
    return CallNextHookEx(
        self ? self->menuMessageHook_ : nullptr, code, wParam, lParam);
}

LRESULT TaskbarMonitorWindow::HandleMessage(
    UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == kSampleTimer) {
            monitor_.Sample(settings_.IsItemVisible(MonitorItem::CpuFrequency));
            // Explorer can resize the task-button area when applications are
            // opened or closed. Only correct it when the geometry changed.
            AdjustPosition(false);
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;

    case WM_PRINTCLIENT:
        Paint(reinterpret_cast<HDC>(wParam));
        return 0;

    case WM_PAINT:
        Paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_RBUTTONUP: {
        POINT point{};
        GetCursorPos(&point);
        ShowContextMenu(point);
        return 0;
    }

    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            GetCursorPos(&point);
        }
        ShowContextMenu(point);
        return 0;
    }

    case WM_NCDESTROY:
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        window_ = nullptr;
        return 0;

    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

void TaskbarMonitorWindow::AdjustPosition(bool force) {
    if (!window_ || !IsWindow(window_) || !taskbar_ || !IsWindow(taskbar_)) {
        return;
    }
    const UINT currentDpi = GetDpiForWindow(taskbar_);
    if (currentDpi && currentDpi != dpi_) { dpi_ = currentDpi; RecreateFont(); force = true; }
    notifyArea_ = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr);
    if (classicTaskbar_) {
        AdjustClassicTaskbar(force);
    } else {
        AdjustModernTaskbar(force);
    }
}

void TaskbarMonitorWindow::AdjustClassicTaskbar(bool force) {
    if (!taskList_ || !IsWindow(taskList_) || !parent_ || !IsWindow(parent_)) {
        return;
    }

    RECT current{};
    GetWindowRect(taskList_, &current);
    MapWindowPoints(HWND_DESKTOP, parent_, reinterpret_cast<POINT*>(&current), 2);

    (void)force;
    originalTaskListRect_ = current;
    originalTaskListRect_.right = FindAvailableTaskListRight(current);
    const int monitorWidth = CalculateRequiredWidth();
    const int originalWidth = RectWidth(originalTaskListRect_);
    const int newTaskListWidth = std::max(0, originalWidth - monitorWidth);
    RECT adjusted = originalTaskListRect_;
    adjusted.right = adjusted.left + newTaskListWidth;

    const bool taskListChanged =
        !EqualRectValue(current, adjusted);
    if (taskListChanged) {
        MoveWindow(taskList_, adjusted.left, adjusted.top,
            RectWidth(adjusted), RectHeight(adjusted), TRUE);
        RedrawWindow(taskList_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    RECT parentClient{};
    GetClientRect(parent_, &parentClient);
    const int availableHeight = RectHeight(parentClient);
    const int height = std::max(Scale(24), availableHeight - Scale(2));
    const int y = std::max(0, (availableHeight - height) / 2);
    RECT expectedMonitor{
        adjusted.right, y,
        adjusted.right + monitorWidth, y + height};
    RECT currentMonitor{};
    GetWindowRect(window_, &currentMonitor);
    MapWindowPoints(HWND_DESKTOP, parent_,
        reinterpret_cast<POINT*>(&currentMonitor), 2);
    if (taskListChanged || !EqualRectValue(currentMonitor, expectedMonitor)) {
        SetWindowPos(window_, HWND_TOP,
            expectedMonitor.left, expectedMonitor.top, monitorWidth, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    reservedWidth_ = monitorWidth;
    taskListReserved_ = true;
}

int TaskbarMonitorWindow::FindAvailableTaskListRight(
    const RECT& current) const noexcept {
    RECT parentClient{};
    if (!parent_ || !GetClientRect(parent_, &parentClient)) {
        return current.right;
    }

    LONG availableRight = parentClient.right;
    if (notifyArea_ && IsWindowVisible(notifyArea_)) {
        RECT tray{}; GetWindowRect(notifyArea_, &tray);
        MapWindowPoints(HWND_DESKTOP, parent_, reinterpret_cast<POINT*>(&tray), 2);
        availableRight = std::min(availableRight, tray.left - Scale(4));
    }
    for (HWND sibling = GetWindow(parent_, GW_CHILD);
         sibling; sibling = GetWindow(sibling, GW_HWNDNEXT)) {
        if (sibling == taskList_ || sibling == window_ || !IsWindowVisible(sibling)) {
            continue;
        }

        RECT siblingRect{};
        if (!GetWindowRect(sibling, &siblingRect)) {
            continue;
        }
        MapWindowPoints(HWND_DESKTOP, parent_,
            reinterpret_cast<POINT*>(&siblingRect), 2);
        const bool overlapsVertically =
            siblingRect.bottom > current.top && siblingRect.top < current.bottom;
        const bool occupiesSpaceToTheRight =
            siblingRect.left > current.left;
        if (overlapsVertically && occupiesSpaceToTheRight) {
            availableRight = std::min<LONG>(availableRight, siblingRect.left);
        }
    }
    return static_cast<int>(std::max<LONG>(current.left, availableRight));
}

void TaskbarMonitorWindow::AdjustModernTaskbar(bool force) {
    RECT taskbarRect{};
    GetWindowRect(taskbar_, &taskbarRect);
    (void)force;
    const int monitorWidth = CalculateRequiredWidth();
    const int taskbarHeight = RectHeight(taskbarRect);
    const int height = std::max(Scale(24), taskbarHeight - Scale(2));

    int x = RectWidth(taskbarRect) - monitorWidth - Scale(160);
    if (notifyArea_ && IsWindow(notifyArea_)) {
        RECT notify{};
        GetWindowRect(notifyArea_, &notify);
        x = notify.left - taskbarRect.left - monitorWidth - Scale(4);
    }
    x = std::max(0, x);
    const int y = std::max(0, (taskbarHeight - height) / 2);
    RECT actual{}; GetWindowRect(window_, &actual);
    MapWindowPoints(HWND_DESKTOP, parent_, reinterpret_cast<POINT*>(&actual), 2);
    const RECT expected{x,y,x+monitorWidth,y+height};
    if (!EqualRectValue(actual,expected))
        SetWindowPos(window_, HWND_TOP, x, y, monitorWidth, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    reservedWidth_ = monitorWidth;
}

void TaskbarMonitorWindow::RestoreTaskList() {
    if (taskListReserved_ && taskList_ && IsWindow(taskList_)) {
        GetWindowRect(taskList_, &originalTaskListRect_);
        MapWindowPoints(HWND_DESKTOP,parent_,reinterpret_cast<POINT*>(&originalTaskListRect_),2);
        originalTaskListRect_.right=FindAvailableTaskListRight(originalTaskListRect_);
        MoveWindow(taskList_, originalTaskListRect_.left, originalTaskListRect_.top,
            RectWidth(originalTaskListRect_), RectHeight(originalTaskListRect_), TRUE);
    }
    taskListReserved_ = false;
    reservedWidth_ = 0;
    SetRectEmpty(&originalTaskListRect_);
}

void TaskbarMonitorWindow::RecreateFont() {
    if (font_) {
        DeleteObject(font_);
    }
    font_ = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void TaskbarMonitorWindow::Paint(HDC printTarget) {
    PAINTSTRUCT paint{};
    HDC target = printTarget ? printTarget : BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);

    HDC deviceContext = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, std::max(1L,client.right), std::max(1L,client.bottom));
    HGDIOBJ oldBuffer = SelectObject(deviceContext,buffer);
    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(deviceContext, &client, background);
    DeleteObject(background);

    const HFONT previousFont = static_cast<HFONT>(SelectObject(deviceContext, font_));
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(0, 230, 180));

    const auto columns = BuildColumns(deviceContext);

    int x = Scale(3);
    const int rowHeight = std::max(1, RectHeight(client) / 2);
    for (const PaintedColumn& column : columns) {
        RECT firstRect{x, 0, x + column.width, rowHeight};
        DrawTextW(deviceContext, column.firstText.c_str(), -1, &firstRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (!column.secondText.empty()) {
            RECT secondRect{x, rowHeight, x + column.width, RectHeight(client)};
            DrawTextW(deviceContext, column.secondText.c_str(), -1, &secondRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        x += column.width + Scale(kColumnGapDip);
    }

    SelectObject(deviceContext, previousFont);
    BitBlt(target,0,0,client.right,client.bottom,deviceContext,0,0,SRCCOPY);
    SelectObject(deviceContext,oldBuffer); DeleteObject(buffer); DeleteDC(deviceContext);
    if(!printTarget) EndPaint(window_, &paint);
}

void TaskbarMonitorWindow::ShowContextMenu(POINT screenPoint) {
    if(controller_)SendMessageW(controller_,kPrepareTranslationMenuMessage,0,0);
    deferredMenuCommand_=0;
    HMENU menu = CreatePopupMenu();
    HMENU monitorMenu = CreatePopupMenu();
    if (!menu || !monitorMenu) {
        if (monitorMenu) {
            DestroyMenu(monitorMenu);
        }
        if (menu) {
            DestroyMenu(menu);
        }
        return;
    }

    for (std::size_t index = 0; index < kMenuItems.size(); ++index) {
        const auto& item = kMenuItems[index];
        const UINT flags = MF_STRING |
            (settings_.IsItemVisible(item.item) ? MF_CHECKED : MF_UNCHECKED);
        AppendMenuW(monitorMenu, flags,
            kMonitorCommandBase + static_cast<UINT>(index), item.label);
    }

    MENUITEMINFOW monitorItem{sizeof(monitorItem)};monitorItem.fMask=MIIM_ID|MIIM_STRING|MIIM_SUBMENU|MIIM_STATE;
    monitorItem.wID=kMonitorDisplayCommand;monitorItem.dwTypeData=const_cast<wchar_t*>(L"性能监控");monitorItem.hSubMenu=monitorMenu;monitorItem.fState=settings_.MonitorInTaskbar()?MFS_CHECKED:MFS_UNCHECKED;
    InsertMenuItemW(menu,0,TRUE,&monitorItem);
    translationMenu_=CreatePopupMenu();
    for(UINT i=0;i<3;++i)AppendMenuW(translationMenu_,MF_STRING,kTranslationEntryBase+i,MenuShortcutText(hotkeys::Names[i+2],hotkeys::Text(settings_.Hotkeys()[i+2]).c_str()).c_str());
    AppendMenuW(translationMenu_,MF_SEPARATOR,0,nullptr);
    AppendMenuW(translationMenu_,MF_STRING,kTranslationSourcesCommand,L"翻译源设置");
    std::wstring translationLabel=L"中英翻译";
    MENUITEMINFOW translationItem{sizeof(translationItem)};translationItem.fMask=MIIM_ID|MIIM_STRING|MIIM_SUBMENU|MIIM_STATE;
    translationItem.wID=kTranslationCommand;translationItem.dwTypeData=translationLabel.data();translationItem.hSubMenu=translationMenu_;translationItem.fState=settings_.TranslationEnabled()?MFS_CHECKED:MFS_UNCHECKED;
    InsertMenuItemW(menu,1,TRUE,&translationItem);
    AppendMenuW(menu,
        MF_STRING | (settings_.ScreenshotEnabled() ? MF_CHECKED : MF_UNCHECKED),
        kScreenshotCommand, MenuShortcutText(L"屏幕截图",hotkeys::Text(settings_.Hotkeys()[0]).c_str()).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTaskManagerCommand, MenuShortcutText(L"任务管理器",hotkeys::Text(settings_.Hotkeys()[5]).c_str()).c_str());
    AppendMenuW(menu, MF_STRING, kClipboardHistoryCommand, MenuShortcutText(L"剪切板历史",hotkeys::Text(settings_.Hotkeys()[1]).c_str()).c_str());
    AppendMenuW(menu, MF_STRING, kPaintCommand, MenuShortcutText(L"调用画图",hotkeys::Text(settings_.Hotkeys()[6]).c_str()).c_str());
    AppendMenuW(menu, MF_STRING, kArchiveCommand, MenuShortcutText(L"7-zip",hotkeys::Text(settings_.Hotkeys()[7]).c_str()).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kHotkeySettingsCommand, MenuShortcutText(L"快捷键管理",hotkeys::Text(settings_.Hotkeys()[8]).c_str()).c_str());
    AppendMenuW(menu,
        MF_STRING | (settings_.AutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
        kAutoStartCommand, L"开机自动运行");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出 PcTool");

    contextMenu_ = menu;
    monitorMenu_ = monitorMenu;
    activeMenuOwner_ = this;
    menuMessageHook_ = SetWindowsHookExW(
        WH_MSGFILTER, MenuMessageHook, nullptr, GetCurrentThreadId());

    const HWND menuOwner = controller_ && IsWindow(controller_)
        ? controller_ : window_;
    SetForegroundWindow(menuOwner);
    const UINT command = TrackPopupMenuEx(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
    PostMessageW(menuOwner, WM_NULL, 0, 0);

    if (menuMessageHook_) {
        UnhookWindowsHookEx(menuMessageHook_);
    }
    menuMessageHook_ = nullptr;
    activeMenuOwner_ = nullptr;
    contextMenu_ = nullptr;
    monitorMenu_ = nullptr;
    translationMenu_=nullptr;
    DestroyMenu(menu);

    const UINT selected=deferredMenuCommand_?deferredMenuCommand_:command;
    deferredMenuCommand_=0;
    if (selected != 0) {
        HandleMenuCommand(selected);
    }
}

bool TaskbarMonitorWindow::KeepsMenuOpen(UINT command) const noexcept {
    return (command >= kMonitorCommandBase &&
            command < kMonitorCommandBase + kMenuItems.size()) ||
        command == kTranslationCommand ||
        command == kScreenshotCommand ||
        command == kAutoStartCommand;
}

UINT TaskbarMonitorWindow::MenuCommandAtPoint(POINT screenPoint) const {
    for (HMENU menu : {translationMenu_, monitorMenu_, contextMenu_}) {
        if (!menu) {
            continue;
        }
        const int position = MenuItemFromPoint(nullptr, menu, screenPoint);
        if (position >= 0) {
            MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID;GetMenuItemInfoW(menu,position,TRUE,&item);
            if(item.wID==kTranslationCommand||item.wID==kMonitorDisplayCommand){RECT r{};GetMenuItemRect(nullptr,menu,position,&r);if(screenPoint.x>=r.right-Scale(28))return 0;}
            return item.wID;
        }
    }
    return 0;
}

UINT TaskbarMonitorWindow::HighlightedMenuCommand() const {
    for (HMENU menu : {translationMenu_, monitorMenu_, contextMenu_}) {
        if (!menu) {
            continue;
        }
        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            const UINT state = GetMenuState(menu, position, MF_BYPOSITION);
            if (state != static_cast<UINT>(-1) && (state & MF_HILITE) != 0) {
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID;GetMenuItemInfoW(menu,position,TRUE,&item);return item.wID;
            }
        }
    }
    return 0;
}

void TaskbarMonitorWindow::RefreshMenuCheckmark(UINT command) const {
    HMENU menu = contextMenu_;
    bool checked = false;
    if (command >= kMonitorCommandBase &&
        command < kMonitorCommandBase + kMenuItems.size()) {
        menu = monitorMenu_;
        checked = settings_.IsItemVisible(
            kMenuItems[command - kMonitorCommandBase].item);
    } else if (command == kMonitorDisplayCommand) {
        checked = settings_.MonitorInTaskbar();
    } else if (command == kTranslationCommand) {
        checked = settings_.TranslationEnabled();
    } else if (command == kScreenshotCommand) {
        checked = settings_.ScreenshotEnabled();
    } else if (command == kAutoStartCommand) {
        checked = settings_.AutoStartEnabled();
    } else {
        return;
    }

    if (menu) {
        CheckMenuItem(menu, command,
            MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));

        const int count = GetMenuItemCount(menu);
        for (int position = 0; position < count; ++position) {
            MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID;GetMenuItemInfoW(menu,position,TRUE,&item);
            if (item.wID != command) {
                continue;
            }
            RECT itemRect{};
            if (GetMenuItemRect(nullptr, menu, position, &itemRect)) {
                const POINT center{
                    (itemRect.left + itemRect.right) / 2,
                    (itemRect.top + itemRect.bottom) / 2};
                if (HWND menuWindow = WindowFromPoint(center)) {
                    RedrawWindow(menuWindow, nullptr, nullptr,
                        RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
                }
            }
            break;
        }
    }
}

void TaskbarMonitorWindow::HandleMenuCommand(UINT command) {
    if(command>=kTranslationEntryBase&&command<kTranslationEntryBase+3){PostMessageW(controller_,kInvokeTranslationMenuMessage,command-kTranslationEntryBase,0);return;}
    if(command==kHotkeySettingsCommand){PostMessageW(controller_,kShowHotkeySettingsMessage,0,0);return;}
    if(command==kMonitorDisplayCommand){
        TraceMonitor("menu.toggle");
        const bool previous=settings_.MonitorInTaskbar();settings_.SetMonitorInTaskbar(!previous);
        if(!settings_.Save()){settings_.SetMonitorInTaskbar(previous);MessageBoxW(controller_,L"保存性能监控显示设置失败。",L"PcTool",MB_OK|MB_ICONWARNING);}
        else PostMessageW(controller_,kMonitorDisplayChangedMessage,0,0);
        return;
    }
    if(command==kTranslationSourcesCommand){PostMessageW(controller_,kShowTranslationSourcesMessage,0,0);return;}
    if (command >= kMonitorCommandBase && command < kMonitorCommandBase + kMenuItems.size()) {
        const MonitorItem item = kMenuItems[command - kMonitorCommandBase].item;
        if (settings_.ToggleItem(item)) {
            settings_.Save();
            if(window_){AdjustPosition(true);InvalidateRect(window_, nullptr, TRUE);}
        }
        return;
    }

    if (command == kTranslationCommand) {
        const bool oldValue = settings_.TranslationEnabled();
        settings_.SetTranslationEnabled(!oldValue);
        if (!settings_.Save()) {
            settings_.SetTranslationEnabled(oldValue);
            MessageBoxW(window_, L"保存中英翻译设置失败。", L"PcTool",
                MB_OK | MB_ICONWARNING);
        } else if(controller_) {
            PostMessageW(controller_,kAppSettingsChangedMessage,0,0);
        }
        return;
    }

    if (command == kScreenshotCommand) {
        const bool oldValue = settings_.ScreenshotEnabled();
        settings_.SetScreenshotEnabled(!oldValue);
        if (!settings_.Save()) {
            settings_.SetScreenshotEnabled(oldValue);
            MessageBoxW(window_, L"保存屏幕截图设置失败。", L"PcTool",
                MB_OK | MB_ICONWARNING);
        } else if (controller_) {
            PostMessageW(controller_, kAppSettingsChangedMessage, 0, 0);
        }
        return;
    }

    if (command == kArchiveCommand) { PostMessageW(controller_, kOpenArchiveManagerMessage, 0, 0); return; }
    if (command == kClipboardHistoryCommand || command == kPaintCommand) {
        PostMessageW(controller_, command == kClipboardHistoryCommand ? kShowClipboardHistoryMessage : kLaunchPaintMessage, 0, 0);
        return;
    }
    if (command == kTaskManagerCommand) {
        PostMessageW(controller_,kLaunchTaskManagerMessage,0,0);
        return;
    }

    if (command == kAutoStartCommand) {
        const bool oldValue = settings_.AutoStartEnabled();
        settings_.SetAutoStartEnabled(!oldValue);
        if (!settings_.ApplyAutoStart() || !settings_.Save()) {
            settings_.SetAutoStartEnabled(oldValue);
            settings_.ApplyAutoStart();
            MessageBoxW(window_, L"修改开机启动设置失败。", L"PcTool", MB_OK | MB_ICONWARNING);
        }
        return;
    }

    if (command == kExitCommand) {
        PostMessageW(controller_, WM_CLOSE, 0, 0);
    }
}

std::vector<MonitorItem> TaskbarMonitorWindow::VisibleItems() const {
    std::vector<MonitorItem> result;
    result.reserve(kMenuItems.size());
    for (const auto& item : kMenuItems) {
        if (settings_.IsItemVisible(item.item)) {
            result.push_back(item.item);
        }
    }
    return result;
}

int TaskbarMonitorWindow::CalculateRequiredWidth() const {
    const std::vector<MonitorItem> items = VisibleItems();
    if (!window_ || !font_) {
        return Scale(180);
    }

    HDC deviceContext = GetDC(window_);
    if (!deviceContext) {
        return Scale(180);
    }
    const HFONT previousFont = static_cast<HFONT>(SelectObject(deviceContext, font_));

    int width = Scale(3) + Scale(10);
    const auto columns = BuildColumns(deviceContext);
    for (const auto& column : columns) width += column.width + Scale(kColumnGapDip);
    SelectObject(deviceContext, previousFont);
    ReleaseDC(window_, deviceContext);
    return std::max(1, width);
}

std::vector<TaskbarMonitorWindow::PaintedColumn> TaskbarMonitorWindow::BuildColumns(HDC deviceContext) const {
    const auto items = VisibleItems();
    std::vector<PaintedColumn> columns;
    columns.reserve((items.size() + 1) / 2);

    for (std::size_t index = 0; index < items.size(); index += 2) {
        PaintedColumn column;
        column.firstText = GetItemText(items[index]);
        const bool splitCpuFrequency =
            index + 1 >= items.size() && items[index] == MonitorItem::CpuFrequency;
        column.width = StableItemWidth(
            deviceContext, items[index], splitCpuFrequency);
        if (index + 1 < items.size()) {
            column.secondText = GetItemText(items[index + 1]);
            column.width = std::max(column.width,
                StableItemWidth(deviceContext, items[index + 1], false));
        } else if (items[index] == MonitorItem::CpuFrequency) {
            column.firstText = L"CPU频率";
            std::array<wchar_t, 32> frequency{};
            swprintf_s(frequency.data(), frequency.size(), L"%.2f GHz",
                monitor_.Metrics().cpuFrequencyGhz);
            column.secondText = frequency.data();
        }
        for (const auto& text : {column.firstText, column.secondText}) {
            SIZE extent{}; GetTextExtentPoint32W(deviceContext,text.c_str(),int(text.size()),&extent);
            column.width=std::max(column.width,int(extent.cx));
        }
        column.width += Scale(2);
        columns.push_back(std::move(column));
    }

    return columns;
}

int TaskbarMonitorWindow::StableItemWidth(
    HDC deviceContext, MonitorItem item, bool splitCpuFrequency) const {
    const auto measure = [deviceContext](const wchar_t* text) {
        SIZE size{};
        GetTextExtentPoint32W(deviceContext, text,
            static_cast<int>(wcslen(text)), &size);
        return size.cx;
    };

    switch (item) {
    case MonitorItem::UploadSpeed:
    case MonitorItem::DownloadSpeed:
        return measure(L"↓ 999.9 M/s");
    case MonitorItem::CpuUsage:
        return measure(L"CPU: 100%");
    case MonitorItem::MemoryUsage:
        return measure(L"内存: 100%");
    case MonitorItem::CpuFrequency:
        if (splitCpuFrequency) {
            return std::max(measure(L"CPU频率"), measure(L"9.99 GHz"));
        }
        return measure(L"CPU频率: 9.99 GHz");
    }
    return Scale(90);
}

std::wstring TaskbarMonitorWindow::GetItemText(MonitorItem item) const {
    const SystemMetrics& metrics = monitor_.Metrics();
    std::array<wchar_t, 48> text{};
    switch (item) {
    case MonitorItem::UploadSpeed:
        return L"↑ " + FormatRate(metrics.uploadBytesPerSecond);
    case MonitorItem::DownloadSpeed:
        return L"↓ " + FormatRate(metrics.downloadBytesPerSecond);
    case MonitorItem::CpuUsage:
        swprintf_s(text.data(), text.size(), L"CPU: %ld%%",
            std::lround(metrics.cpuUsagePercent));
        break;
    case MonitorItem::MemoryUsage:
        swprintf_s(text.data(), text.size(), L"内存: %lu%%",
            metrics.memoryUsagePercent);
        break;
    case MonitorItem::CpuFrequency:
        swprintf_s(text.data(), text.size(), L"CPU频率: %.2f GHz",
            metrics.cpuFrequencyGhz);
        break;
    }
    return text.data();
}

int TaskbarMonitorWindow::Scale(int value) const noexcept {
    return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}

std::wstring TaskbarMonitorWindow::FormatRate(double bytesPerSecond) {
    // Match the displayed transfer rate to the application's expected scale.
    static constexpr std::array<const wchar_t*, 3> units{L"K/s", L"M/s", L"G/s"};
    double value = std::max(0.0, bytesPerSecond) /
        kDisplayedRateDivisor / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::array<wchar_t, 32> text{};
    swprintf_s(text.data(), text.size(),
        value >= 100.0 ? L"%.0f %ls" : L"%.1f %ls",
        value, units[unit]);
    return text.data();
}
