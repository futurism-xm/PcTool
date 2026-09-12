#pragma once

#include "app/app_settings.h"
#include "monitor/system_monitor.h"

#include <windows.h>

#include <string>
#include <vector>

class TaskbarMonitorWindow final {
public:
    TaskbarMonitorWindow(HINSTANCE instance, AppSettings& settings);
    ~TaskbarMonitorWindow();

    TaskbarMonitorWindow(const TaskbarMonitorWindow&) = delete;
    TaskbarMonitorWindow& operator=(const TaskbarMonitorWindow&) = delete;

    bool Create(HWND controller);
    void Destroy();
    void ShowMenu(HWND controller,POINT position) { controller_=controller;ShowContextMenu(position); }

private:
    friend struct TaskbarLayoutTest;
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    std::wstring MenuShortcutText(const wchar_t* label,const wchar_t* shortcut) const;
    static LRESULT CALLBACK MenuMessageHook(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass() const;
    bool FindTaskbarWindows();
    void AdjustPosition(bool force);
    void AdjustClassicTaskbar(bool force);
    void AdjustModernTaskbar(bool force);
    [[nodiscard]] int FindAvailableTaskListRight(const RECT& current) const noexcept;
    void RestoreTaskList();
    void RecreateFont();
    struct PaintedColumn { std::wstring firstText, secondText; int width{}; };
    std::vector<PaintedColumn> BuildColumns(HDC dc) const;
    void Paint(HDC printTarget=nullptr);
    void ShowContextMenu(POINT screenPoint);
    void HandleMenuCommand(UINT command);
    [[nodiscard]] bool KeepsMenuOpen(UINT command) const noexcept;
    [[nodiscard]] UINT MenuCommandAtPoint(POINT screenPoint) const;
    [[nodiscard]] UINT HighlightedMenuCommand() const;
    void RefreshMenuCheckmark(UINT command) const;

    [[nodiscard]] std::vector<MonitorItem> VisibleItems() const;
    [[nodiscard]] int CalculateRequiredWidth() const;
    [[nodiscard]] int StableItemWidth(
        HDC deviceContext, MonitorItem item, bool splitCpuFrequency) const;
    [[nodiscard]] std::wstring GetItemText(MonitorItem item) const;
    [[nodiscard]] int Scale(int value) const noexcept;

    static std::wstring FormatRate(double bytesPerSecond);
    static thread_local TaskbarMonitorWindow* activeMenuOwner_;

    HINSTANCE instance_{};
    HWND controller_{};
    AppSettings& settings_;
    SystemMonitor monitor_;

    HWND window_{};
    HWND taskbar_{};
    HWND parent_{};
    HWND taskList_{};
    HWND notifyArea_{};
    bool classicTaskbar_{};
    bool taskListReserved_{};
    RECT originalTaskListRect_{};
    int reservedWidth_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    HFONT font_{};
    HHOOK menuMessageHook_{};
    HMENU contextMenu_{};
    HMENU monitorMenu_{};
    HMENU translationMenu_{};
    UINT deferredMenuCommand_{};
};
