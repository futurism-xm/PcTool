#pragma once

#include <windows.h>

constexpr UINT kAppSettingsChangedMessage = WM_APP + 101;
constexpr UINT kCenterPinnedWindowCommand = 0x1FF0;
constexpr wchar_t kCenterPinnedWindowArgument[] = L"--center-pin";

constexpr UINT kShowClipboardHistoryMessage = WM_APP + 102;
constexpr UINT kLaunchPaintMessage = WM_APP + 103;
constexpr UINT kOpenArchiveManagerMessage = WM_APP + 104;
constexpr UINT kShowTranslationSourcesMessage = WM_APP + 105;
constexpr UINT kMonitorDisplayChangedMessage = WM_APP + 106;
constexpr UINT kShowHotkeySettingsMessage = WM_APP + 108;
constexpr UINT kLaunchTaskManagerMessage = WM_APP + 109;
constexpr UINT kPrepareTranslationMenuMessage = WM_APP + 110;
constexpr UINT kInvokeTranslationMenuMessage = WM_APP + 111;
