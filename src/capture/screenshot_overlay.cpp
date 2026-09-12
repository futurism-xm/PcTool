#include "shared/platform/app_storage.h"
#include "shared/annotation/annotation_painter.h"
#include "shared/annotation/annotation_fragments.h"
#include "shared/annotation/text_layout.h"
#include "shared/annotation/annotation_style.h"
#include "capture/screenshot_overlay.h"

#include "app/app_messages.h"
#include "shared/ui/resource.h"
#include "app/capture_tools.h"
#include "shared/platform/capture_platform.h"
#include "shared/ui/toolbar_icons.h"

#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <propkey.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <imm.h>
#include <windowsx.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace capture::text_layout;
constexpr wchar_t kScreenshotWindowClass[] = L"PcTool.ScreenshotOverlay";
constexpr wchar_t kPinnedScreenshotWindowClass[] = L"PcTool.PinnedScreenshot";
constexpr COLORREF kSelectionColor = RGB(0, 174, 255);
constexpr BYTE kOutsideDimAlpha = 180;
constexpr COLORREF kToolbarBackgroundColor = capture::ToolbarBackground;
constexpr COLORREF kToolbarBorderColor = capture::ToolbarBorder;
constexpr COLORREF kToolbarIconColor = capture::ToolbarInk;
constexpr COLORREF kToolbarDisabledColor = capture::ToolbarDisabled;
constexpr COLORREF kToolbarSelectedColor = capture::ToolbarSelected;
constexpr COLORREF kToolbarDividerColor = capture::ToolbarDivider;
constexpr COLORREF kNumberLabelBackgroundColor = RGB(105, 105, 105);
constexpr int kToolbarHeightDip = capture::ToolbarHeightDip;
constexpr int kToolbarGripWidthDip = 0;
constexpr int kToolbarButtonWidthDip = capture::ToolbarButtonDip;
constexpr int kFinishButtonWidthDip = 68;
constexpr int kToolbarGapDip = 5;
constexpr int kToolbarPlacementHeightToleranceDip = 3;
constexpr int kHandleRadiusDip = 2;
constexpr int kMinimumSelectionDip = 4;
constexpr int kTextEditorId = 1001;
constexpr UINT kRefreshTextEditorLayoutMessage = WM_APP + 0x43;
constexpr int kColorPickerHotkeyId = 2;
constexpr auto kLineSizesDip=capture::AnnotationLineSizes;
constexpr std::array<int, 3> kMosaicSizesDip{8, 18, 32};
constexpr int kMinimumFontSizePoints=capture::MinimumFontPoints;
constexpr int kMaximumFontSizePoints=capture::MaximumFontPoints;
constexpr int kDefaultTextEditorWidthDip = 120;
constexpr BYTE kAnnotationTextQuality = ANTIALIASED_QUALITY;
constexpr int kFontSizeControlWidthDip = 96;
constexpr int kFontSizePopupItemHeightDip = 22;
constexpr auto kAnnotationColors=capture::AnnotationColors;

int RectWidth(const RECT& rect) noexcept {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) noexcept {
    return rect.bottom - rect.top;
}

bool ContainsPoint(const RECT& rect, POINT point) noexcept {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}

RECT OffsetRectValue(RECT rect, int x, int y) noexcept {
    OffsetRect(&rect, x, y);
    return rect;
}

void FillSolidRect(HDC deviceContext, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(deviceContext, &rect, brush);
    DeleteObject(brush);
}

std::wstring ReadWindowText(HWND window) {
    if (!window) {
        return {};
    }
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

int CALLBACK CharacterWordBreakProc(
    LPWSTR, int current, int length, int code) {
    switch (code) {
    case WB_ISDELIMITER:
        return FALSE;
    case WB_LEFT:
        return std::max(0, current - 1);
    case WB_RIGHT:
        return std::min(length, current + 1);
    default:
        return current;
    }
}

std::vector<std::wstring> ReadEditVisualLines(
    HWND editor, const std::wstring& value) {
    std::vector<std::wstring> lines;
    if (!editor) {
        return lines;
    }
    const int lineCount = std::max<int>(1,
        static_cast<int>(SendMessageW(editor, EM_GETLINECOUNT, 0, 0)));
    lines.reserve(static_cast<std::size_t>(lineCount));
    for (int line = 0; line < lineCount; ++line) {
        const LRESULT lineStartResult = SendMessageW(
            editor, EM_LINEINDEX, line, 0);
        if (lineStartResult < 0) {
            lines.emplace_back();
            continue;
        }
        const std::size_t lineStart = std::min<std::size_t>(
            static_cast<std::size_t>(lineStartResult), value.size());
        const int lineLength = std::max<int>(0,
            static_cast<int>(SendMessageW(
                editor, EM_LINELENGTH, lineStartResult, 0)));
        std::wstring lineText(value.data() + lineStart,
            std::min<int>(lineLength,
                static_cast<int>(value.size() - lineStart)));
        if (!lineText.empty() && lineText.back() == L'\r') {
            lineText.pop_back();
        }
        lines.push_back(std::move(lineText));
    }
    return lines;
}

bool HasMeaningfulText(const std::wstring& value) {
    return std::any_of(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) == 0;
    });
}

std::wstring TrimTextWhitespace(const std::wstring& value) {
    const auto isNotWhitespace = [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) == 0;
    };
    const auto first = std::find_if(
        value.begin(), value.end(), isNotWhitespace);
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if(
        value.rbegin(), value.rend(), isNotWhitespace).base();
    return std::wstring(first, last);
}

double DistanceToSegment(POINT point, POINT first, POINT second) noexcept {
    const double dx = static_cast<double>(second.x - first.x);
    const double dy = static_cast<double>(second.y - first.y);
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.0) {
        return std::hypot(static_cast<double>(point.x - first.x),
            static_cast<double>(point.y - first.y));
    }
    const double projection = std::clamp(
        (static_cast<double>(point.x - first.x) * dx +
         static_cast<double>(point.y - first.y) * dy) / lengthSquared,
        0.0, 1.0);
    const double nearestX = first.x + projection * dx;
    const double nearestY = first.y + projection * dy;
    return std::hypot(point.x - nearestX, point.y - nearestY);
}

bool ContainsPointInclusive(const RECT& rect, POINT point) noexcept {
    return point.x >= rect.left && point.x <= rect.right &&
        point.y >= rect.top && point.y <= rect.bottom;
}

void ActivatePinnedWindowForInteraction(HWND interactionWindow) {
    if (interactionWindow) {
        const HWND taskbarWindow = GetAncestor(interactionWindow, GA_ROOTOWNER);
        SetForegroundWindow(taskbarWindow ? taskbarWindow : interactionWindow);
    }
}

bool IsPinnedScreenshotWindow(HWND window) {
    std::array<wchar_t, 64> className{};
    return window &&
        GetClassNameW(window, className.data(),
            static_cast<int>(className.size())) != 0 &&
        std::wcscmp(className.data(), kPinnedScreenshotWindowClass) == 0;
}

struct PinnedScreenshotData {
    HBITMAP bitmap{};
    HBITMAP baseBitmap{};
    SIZE contentSize{};
    int shadowExtent{};
    POINT dragOffset{};
    POINT pointerDown{};
    bool dragging{};
    bool moved{};
    bool editingDrag{};
    ScreenshotOverlay* owner{};
    std::wstring appUserModelId;
};

std::wstring CurrentExecutablePath() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    return std::wstring(path.data(), length);
}

void SetWindowTaskbarProperties(
    HWND window, const std::wstring& appUserModelId) {
    IPropertyStore* properties = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(
            window, IID_PPV_ARGS(&properties))) || !properties) {
        return;
    }

    PROPVARIANT appId{};
    appId.vt = VT_LPWSTR;
    appId.pwszVal = const_cast<PWSTR>(appUserModelId.c_str());
    properties->SetValue(PKEY_AppUserModel_ID, appId);

    PROPVARIANT preventPinning{};
    preventPinning.vt = VT_BOOL;
    preventPinning.boolVal = VARIANT_TRUE;
    properties->SetValue(PKEY_AppUserModel_PreventPinning, preventPinning);
    properties->Commit();
    properties->Release();
}

IShellLinkW* CreateCenterPinnedWindowTask(
    HWND window, const std::wstring& executablePath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link) {
        return nullptr;
    }

    const std::wstring arguments = std::wstring(kCenterPinnedWindowArgument) +
        L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(window));
    if (FAILED(link->SetPath(executablePath.c_str())) ||
        FAILED(link->SetArguments(arguments.c_str())) ||
        FAILED(link->SetIconLocation(executablePath.c_str(), 0))) {
        link->Release();
        return nullptr;
    }

    IPropertyStore* properties = nullptr;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&properties))) ||
        !properties) {
        link->Release();
        return nullptr;
    }
    PROPVARIANT title{};
    title.vt = VT_LPWSTR;
    title.pwszVal = const_cast<PWSTR>(L"居中显示");
    const bool configured =
        SUCCEEDED(properties->SetValue(PKEY_Title, title)) &&
        SUCCEEDED(properties->Commit());
    properties->Release();
    if (!configured) {
        link->Release();
        return nullptr;
    }
    return link;
}

void CreatePinnedWindowJumpList(
    HWND window, const std::wstring& appUserModelId) {
    const std::wstring executablePath = CurrentExecutablePath();
    if (executablePath.empty()) {
        return;
    }

    ICustomDestinationList* destinations = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinations))) ||
        !destinations) {
        return;
    }
    if (FAILED(destinations->SetAppID(appUserModelId.c_str()))) {
        destinations->Release();
        return;
    }

    UINT maximumSlots = 0;
    IObjectArray* removedItems = nullptr;
    if (FAILED(destinations->BeginList(&maximumSlots,
            IID_PPV_ARGS(&removedItems)))) {
        destinations->Release();
        return;
    }
    if (removedItems) {
        removedItems->Release();
    }

    IObjectCollection* tasks = nullptr;
    IShellLinkW* centerTask = CreateCenterPinnedWindowTask(
        window, executablePath);
    HRESULT result = CoCreateInstance(CLSID_EnumerableObjectCollection,
        nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks));
    if (SUCCEEDED(result) && tasks && centerTask) {
        result = tasks->AddObject(centerTask);
    } else {
        result = E_FAIL;
    }
    if (centerTask) {
        centerTask->Release();
    }

    IObjectArray* taskArray = nullptr;
    if (SUCCEEDED(result)) {
        result = tasks->QueryInterface(IID_PPV_ARGS(&taskArray));
    }
    if (SUCCEEDED(result)) {
        result = destinations->AddUserTasks(taskArray);
    }
    if (SUCCEEDED(result)) {
        result = destinations->CommitList();
    }
    if (taskArray) {
        taskArray->Release();
    }
    if (tasks) {
        tasks->Release();
    }
    if (FAILED(result)) {
        destinations->AbortList();
    }
    destinations->Release();
}

void ConfigurePinnedWindowTaskbar(HWND window, PinnedScreenshotData* data) {
    if (!window || !data) {
        return;
    }
    data->appUserModelId = L"PcTool.Pin." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(reinterpret_cast<UINT_PTR>(window));
    SetWindowTaskbarProperties(window, data->appUserModelId);
    CreatePinnedWindowJumpList(window, data->appUserModelId);
}

void DeletePinnedWindowJumpList(const std::wstring& appUserModelId) {
    if (appUserModelId.empty()) {
        return;
    }
    ICustomDestinationList* destinations = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinations))) &&
        destinations) {
        destinations->DeleteList(appUserModelId.c_str());
        destinations->Release();
    }
}

bool UpdatePinnedLayeredWindow(
    HWND window, PinnedScreenshotData* data,
    int contentScreenX, int contentScreenY) {
    if (!window || !data || !data->bitmap) {
        return false;
    }

    BITMAP sourceInfo{};
    if (GetObjectW(data->bitmap, sizeof(sourceInfo), &sourceInfo) == 0 ||
        sourceInfo.bmWidth <= 0 || sourceInfo.bmHeight <= 0) {
        return false;
    }
    data->contentSize = SIZE{sourceInfo.bmWidth, sourceInfo.bmHeight};
    const int extent = std::max(1, data->shadowExtent);
    const int surfaceWidth = sourceInfo.bmWidth + extent * 2;
    const int surfaceHeight = sourceInfo.bmHeight + extent * 2;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = surfaceWidth;
    bitmapInfo.bmiHeader.biHeight = -surfaceHeight;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* surfaceBits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC surfaceDc = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP surface = screen ? CreateDIBSection(
        screen, &bitmapInfo, DIB_RGB_COLORS, &surfaceBits, nullptr, 0) : nullptr;
    if (!screen || !surfaceDc || !surface || !surfaceBits) {
        if (surface) DeleteObject(surface);
        if (surfaceDc) DeleteDC(surfaceDc);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }

    auto* pixels = static_cast<std::uint32_t*>(surfaceBits);
    std::fill_n(pixels,
        static_cast<std::size_t>(surfaceWidth) * surfaceHeight, 0U);

    // Shadow, border and screenshot live in one ARGB surface, so moving this
    // HWND always moves the complete pinned image as a single visual object.
    const int contentLeft = extent;
    const int contentTop = extent;
    const int contentRight = contentLeft + sourceInfo.bmWidth;
    const int contentBottom = contentTop + sourceInfo.bmHeight;
    for (int y = 0; y < surfaceHeight; ++y) {
        for (int x = 0; x < surfaceWidth; ++x) {
            if (x >= contentLeft && x < contentRight &&
                y >= contentTop && y < contentBottom) {
                continue;
            }
            const int dx = x < contentLeft
                ? contentLeft - x
                : (x >= contentRight ? x - contentRight + 1 : 0);
            const int dy = y < contentTop
                ? contentTop - y
                : (y >= contentBottom ? y - contentBottom + 1 : 0);
            const double distance = std::sqrt(
                static_cast<double>(dx * dx + dy * dy));
            if (distance > extent) {
                continue;
            }
            const double strength = 1.0 - distance /
                static_cast<double>(extent + 1);
            const BYTE alpha = static_cast<BYTE>(std::clamp(
                static_cast<int>(58.0 * strength * strength), 0, 58));
            pixels[static_cast<std::size_t>(y) * surfaceWidth + x] =
                static_cast<std::uint32_t>(alpha) << 24;
        }
    }

    const BYTE borderAlpha = 112;
    const BYTE borderChannel = static_cast<BYTE>(
        145 * static_cast<int>(borderAlpha) / 255);
    const std::uint32_t borderPixel =
        (static_cast<std::uint32_t>(borderAlpha) << 24) |
        (static_cast<std::uint32_t>(borderChannel) << 16) |
        (static_cast<std::uint32_t>(borderChannel) << 8) |
        borderChannel;
    for (int x = contentLeft - 1; x <= contentRight; ++x) {
        pixels[static_cast<std::size_t>(contentTop - 1) * surfaceWidth + x] =
            borderPixel;
        pixels[static_cast<std::size_t>(contentBottom) * surfaceWidth + x] =
            borderPixel;
    }
    for (int y = contentTop; y < contentBottom; ++y) {
        pixels[static_cast<std::size_t>(y) * surfaceWidth + contentLeft - 1] =
            borderPixel;
        pixels[static_cast<std::size_t>(y) * surfaceWidth + contentRight] =
            borderPixel;
    }

    BITMAPINFO sourceBitmapInfo{};
    sourceBitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    sourceBitmapInfo.bmiHeader.biWidth = sourceInfo.bmWidth;
    sourceBitmapInfo.bmiHeader.biHeight = -sourceInfo.bmHeight;
    sourceBitmapInfo.bmiHeader.biPlanes = 1;
    sourceBitmapInfo.bmiHeader.biBitCount = 32;
    sourceBitmapInfo.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> sourcePixels(
        static_cast<std::size_t>(sourceInfo.bmWidth) * sourceInfo.bmHeight);
    const int copiedLines = GetDIBits(screen, data->bitmap, 0,
        static_cast<UINT>(sourceInfo.bmHeight), sourcePixels.data(),
        &sourceBitmapInfo, DIB_RGB_COLORS);
    if (copiedLines != sourceInfo.bmHeight) {
        DeleteObject(surface);
        DeleteDC(surfaceDc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    for (int y = 0; y < sourceInfo.bmHeight; ++y) {
        auto* destination = pixels +
            static_cast<std::size_t>(y + contentTop) * surfaceWidth + contentLeft;
        const auto* sourceRow = sourcePixels.data() +
            static_cast<std::size_t>(y) * sourceInfo.bmWidth;
        for (int x = 0; x < sourceInfo.bmWidth; ++x) {
            destination[x] = sourceRow[x] | 0xFF000000U;
        }
    }

    HGDIOBJ previousSurface = SelectObject(surfaceDc, surface);
    POINT windowPosition{
        contentScreenX - extent, contentScreenY - extent};
    SIZE windowSize{surfaceWidth, surfaceHeight};
    POINT sourcePosition{};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(window, screen,
        &windowPosition, &windowSize, surfaceDc, &sourcePosition,
        0, &blend, ULW_ALPHA);
    SelectObject(surfaceDc, previousSurface);
    DeleteObject(surface);
    DeleteDC(surfaceDc);
    ReleaseDC(nullptr, screen);
    return updated != FALSE;
}

bool RefreshPinnedLayeredWindow(HWND window, PinnedScreenshotData* data) {
    RECT windowRect{};
    return window && data && GetWindowRect(window, &windowRect) &&
        UpdatePinnedLayeredWindow(window, data,
            windowRect.left + data->shadowExtent,
            windowRect.top + data->shadowExtent);
}

void MovePinnedWindowToContent(HWND window, int contentX, int contentY) {
    auto* data = reinterpret_cast<PinnedScreenshotData*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!data) {
        return;
    }
    // While the editor is active its clipped overlay must stay above the
    // pinned image so later drags continue to enter the editor's drag path.
    // Raising the pin to HWND_TOPMOST here puts it above the overlay after
    // the first drag, causing every subsequent drag to bypass toolbar hiding.
    SetWindowPos(window, nullptr,
        contentX - data->shadowExtent, contentY - data->shadowExtent,
        0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

void ConfigurePinnedWindowSystemMenu(HWND window) {
    HMENU menu = GetSystemMenu(window, FALSE);
    if (!menu) {
        return;
    }
    for (int position = GetMenuItemCount(menu) - 1;
         position >= 0; --position) {
        DeleteMenu(menu, position, MF_BYPOSITION);
    }
    AppendMenuW(menu, MF_STRING,
        kCenterPinnedWindowCommand, L"居中显示");
    AppendMenuW(menu, MF_STRING, SC_CLOSE, L"关闭窗口");
}

void CenterPinnedWindow(HWND window, PinnedScreenshotData* data) {
    RECT windowRect{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(
        window, MONITOR_DEFAULTTONEAREST);
    if (!GetWindowRect(window, &windowRect) || !monitor ||
        !GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }

    const int width = RectWidth(windowRect);
    const int height = RectHeight(windowRect);
    const RECT& workArea = monitorInfo.rcWork;
    const int x = workArea.left + (RectWidth(workArea) - width) / 2;
    const int y = workArea.top + (RectHeight(workArea) - height) / 2;
    if (data && data->owner) {
        data->owner->HidePinnedToolbar(window);
    }
    SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK PinnedScreenshotWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<PinnedScreenshotData*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SYSCOMMAND:
        switch (static_cast<UINT>(wParam) & 0xFFF0U) {
        case kCenterPinnedWindowCommand:
            CenterPinnedWindow(window, data);
            return 0;
        case SC_CLOSE:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_LBUTTONDOWN:
        if (data) {
            ActivatePinnedWindowForInteraction(window);
            if (data->owner) {
                data->owner->SwitchPinnedEditingForPointerDown(window);
            }
            POINT cursor{};
            RECT windowRect{};
            GetCursorPos(&cursor);
            GetWindowRect(window, &windowRect);
            data->dragOffset = POINT{
                cursor.x - windowRect.left, cursor.y - windowRect.top};
            data->pointerDown = cursor;
            data->dragging = true;
            data->moved = false;
            data->editingDrag = data->owner &&
                data->owner->IsEditingPinnedWindow(window);
            SetCapture(window);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (data && data->dragging && (wParam & MK_LBUTTON) != 0) {
            POINT cursor{};
            GetCursorPos(&cursor);
            if (!data->moved) {
                data->moved =
                    std::abs(cursor.x - data->pointerDown.x) >=
                        GetSystemMetrics(SM_CXDRAG) ||
                    std::abs(cursor.y - data->pointerDown.y) >=
                        GetSystemMetrics(SM_CYDRAG);
            }
            if (data->moved) {
                const int x = cursor.x - data->dragOffset.x;
                const int y = cursor.y - data->dragOffset.y;
                SetWindowPos(window,
                    data->editingDrag ? nullptr : HWND_TOPMOST, x, y,
                    0, 0, SWP_NOSIZE | SWP_NOACTIVATE |
                        (data->editingDrag ? SWP_NOZORDER : 0));
                if (data->editingDrag && data->owner) {
                    data->owner->SyncPinnedWindowPosition(window,
                        x + data->shadowExtent,
                        y + data->shadowExtent, false);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        const bool openEditor = data && data->dragging &&
            !data->editingDrag;
        const bool finishEditingPointer = data && data->dragging &&
            data->editingDrag;
        if (data) {
            data->dragging = false;
        }
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        if (openEditor && data->owner) {
            data->owner->StartPinnedEditing(window);
        }
        if (finishEditingPointer && data->owner) {
            RECT windowRect{};
            if (GetWindowRect(window, &windowRect)) {
                data->owner->SyncPinnedWindowPosition(window,
                    windowRect.left + data->shadowExtent,
                    windowRect.top + data->shadowExtent, true);
            }
        }
        if (data) {
            data->editingDrag = false;
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (data) {
            data->dragging = false;
            data->editingDrag = false;
        }
        return 0;
    case WM_RBUTTONUP:
        if (data && data->owner) {
            if (data->owner->IsEditingPinnedWindow(window)) {
                RECT windowRect{};
                if (GetWindowRect(window, &windowRect)) {
                    data->owner->SyncPinnedWindowPosition(window,
                        windowRect.left + data->shadowExtent,
                        windowRect.top + data->shadowExtent, true);
                }
            } else {
                data->owner->StartPinnedEditing(window);
            }
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return TRUE;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (data) {
            DeletePinnedWindowJumpList(data->appUserModelId);
            if (data->owner) {
                data->owner->ForgetPinnedWindow(window);
            }
            if (data->bitmap) {
                DeleteObject(data->bitmap);
            }
            if (data->baseBitmap && data->baseBitmap != data->bitmap) {
                DeleteObject(data->baseBitmap);
            }
            delete data;
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
}  // namespace

ScreenshotOverlay::ScreenshotOverlay(HINSTANCE instance) noexcept
    : instance_(instance) {
    // The first palette entry is the application default.  This value is
    // changed only by an explicit palette click and then remains selected for
    // the lifetime of this ScreenshotOverlay instance.
    activeColor_ = kAnnotationColors.front();
    Gdiplus::GdiplusStartupInput startupInput;
    if (Gdiplus::GdiplusStartup(
            &gdiplusToken_, &startupInput, nullptr) != Gdiplus::Ok) {
        gdiplusToken_ = 0;
    }
}

ScreenshotOverlay::~ScreenshotOverlay() {
    Cancel();
    if (gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
}

bool ScreenshotOverlay::RegisterWindowClass() const {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    // This top-level window is clipped into disjoint selection/toolbar
    // regions while editing a pinned image. CS_DROPSHADOW makes Windows draw
    // stray shadow strips around that compound region, so only the pinned
    // layered window owns and renders a shadow.
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    windowClass.lpszClassName = kScreenshotWindowClass;
    if (RegisterClassExW(&windowClass)) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ScreenshotOverlay::Start() {
    if (captureTools_ && captureTools_->FocusActive()) return true;
    if (IsActive()) {
        if (!editingPinned_) {
            return true;
        }

        // A pin editor is not an unfinished capture. Commit its current state
        // and remove only the editor toolbar so the pin itself becomes part of
        // the next desktop capture.
        CompletePinnedEditing(true);
        if (IsActive()) {
            return false;
        }
        DwmFlush();
    }
    Cancel();
    if (!RegisterWindowClass()) {
        ReleaseResources();
        return false;
    }
    if (!CaptureDesktop()) {
        ReleaseResources();
        return false;
    }
    // Capture pins into the frozen desktop first, then hide their live HWNDs
    // so the screenshot overlay does not display a second copy on top.
    HidePinnedWindowsForCapture();

    POINT cursor{};
    GetCursorPos(&cursor);
    RECT initialScreenRect{};
    if (!FindWindowAtScreenPoint(cursor, initialScreenRect)) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        initialScreenRect = monitorInfo.rcMonitor;
    }
    selection_ = ClampRectToClient(OffsetRectValue(
        initialScreenRect, -virtualLeft_, -virtualTop_));
    selectionCommitted_ = false;
    toolbarMoved_ = false;
    annotations_.clear();
    activeTool_ = Tool::None;
    hoveredToolbarCommand_ = ToolbarCommand::None;
    pressedToolbarCommand_ = ToolbarCommand::None;
    toolbarSuppressed_ = false;
    toolbarVisibleBeforePinnedDrag_ = false;
    hoveredFontSizePoints_ = -1;
    fontSizeMenuOpen_ = false;

    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kScreenshotWindowClass, L"PcTool Screenshot",
        WS_POPUP | WS_CLIPCHILDREN,
        virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
        nullptr, nullptr, instance_, this);
    if (!window_) {
        ReleaseResources();
        return false;
    }

    dpi_ = GetDpiForWindow(window_);
    if (dpi_ == 0) {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    toolbarFont_ = capture::CreateToolbarFont(dpi_);
    UpdateToolbarPosition(true);
    CreateToolbarTooltip();
    colorPickerHotkeyRegistered_ = RegisterHotKey(
        window_, kColorPickerHotkeyId, MOD_NOREPEAT, 'C') != FALSE;

    ShowWindow(window_, SW_SHOW);
    SetWindowPos(window_, HWND_TOPMOST, virtualLeft_, virtualTop_,
        virtualWidth_, virtualHeight_, SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    SetFocus(window_);
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool ScreenshotOverlay::StartPinnedEditing(HWND pinnedWindow) {
    if (!pinnedWindow || !IsWindow(pinnedWindow)) {
        return false;
    }
    auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
        GetWindowLongPtrW(pinnedWindow, GWLP_USERDATA));
    BITMAP bitmapInfo{};
    RECT pinnedRect{};
    if (!pinnedData || !pinnedData->bitmap ||
        GetObjectW(pinnedData->bitmap, sizeof(bitmapInfo), &bitmapInfo) == 0 ||
        bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight <= 0 ||
        !GetWindowRect(pinnedWindow, &pinnedRect)) {
        return false;
    }

    bool reuseEditorWindow = false;
    if (window_) {
        if (IsEditingPinnedWindow(pinnedWindow)) {
            SyncPinnedWindowPosition(pinnedWindow,
                pinnedRect.left + pinnedData->shadowExtent,
                pinnedRect.top + pinnedData->shadowExtent, true);
            return true;
        }
        if (!editingPinned_) {
            return false;
        }
        // Keep the active editor alive while switching pins. Destroying and
        // recreating it briefly reactivates the previous desktop window.
        CompletePinnedEditing(true, true);
        if (window_) {
            reuseEditorWindow = true;
        } else {
            return false;
        }
    }

    // Keep the pinned window visible while the editor is opened. The overlay
    // initially paints the same pixels over it, avoiding a desktop-colored
    // frame between the two windows.
    editingPinned_ = true;
    pinnedWindowToUpdate_ = pinnedWindow;
    if (!reuseEditorWindow &&
        (!RegisterWindowClass() || !CaptureDesktop())) {
        ReleaseResources();
        return false;
    }

    selection_ = RECT{
        pinnedRect.left + pinnedData->shadowExtent - virtualLeft_,
        pinnedRect.top + pinnedData->shadowExtent - virtualTop_,
        pinnedRect.left + pinnedData->shadowExtent - virtualLeft_ + bitmapInfo.bmWidth,
        pinnedRect.top + pinnedData->shadowExtent - virtualTop_ + bitmapInfo.bmHeight};

    const bool editorHiddenForSwitch = reuseEditorWindow &&
        SetLayeredWindowAttributes(window_, 0, 0, LWA_ALPHA) != FALSE;
    if (editorHiddenForSwitch) {
        DwmFlush();
    }

    // The active pin must sit above every other pin. The editor is raised
    // immediately afterwards, so its transparent drag handoff reveals the
    // same pin instead of an overlapping sibling.
    SetWindowPos(pinnedWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    selectionCommitted_ = true;
    toolbarMoved_ = false;
    LoadPinnedAnnotations(pinnedWindow);
    workingAnnotation_ = {};
    editingAnnotation_ = {};
    annotationAtDragStart_ = {};
    movingAnnotationIndex_ = -1;
    editingAnnotationIndex_ = -1;
    interaction_ = Interaction::None;
    toolbarDragStarted_ = false;
    annotationDragStarted_ = false;
    selectionResizeStarted_ = false;
    activeTool_ = Tool::None;
    hoveredToolbarCommand_ = ToolbarCommand::None;
    pressedToolbarCommand_ = ToolbarCommand::None;
    toolbarSuppressed_ = false;
    toolbarVisibleBeforePinnedDrag_ = false;
    hoveredFontSizePoints_ = -1;
    fontSizeMenuOpen_ = false;
    if (reuseEditorWindow) {
        SetWindowLongPtrW(window_, GWLP_HWNDPARENT,
            reinterpret_cast<LONG_PTR>(pinnedWindow));
        UpdateToolbarPosition(true);
        UpdateToolbarTooltips();
        UpdatePinnedEditingRegion();
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE |
                RDW_ALLCHILDREN);
        SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
        if (editorHiddenForSwitch) {
            DwmFlush();
        }
        SetFocus(window_);
        return true;
    }

    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kScreenshotWindowClass, L"PcTool 固定截图编辑",
        WS_POPUP | WS_CLIPCHILDREN,
        virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
        pinnedWindow, nullptr, instance_, this);
    if (!window_) {
        ReleaseResources();
        return false;
    }
    if (!SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA)) {
        DestroyWindow(window_);
        return false;
    }
    dpi_ = GetDpiForWindow(window_);
    if (dpi_ == 0) {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    toolbarFont_ = capture::CreateToolbarFont(dpi_);
    UpdateToolbarPosition(true);
    CreateToolbarTooltip();

    // Apply the small visible region before the top-level overlay is shown.
    // Showing the full virtual-screen popup first produces a one-frame flash.
    UpdatePinnedEditingRegion();
    ShowWindow(window_, SW_SHOW);
    SetWindowPos(window_, HWND_TOPMOST, virtualLeft_, virtualTop_,
        virtualWidth_, virtualHeight_, SWP_SHOWWINDOW);
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    SetForegroundWindow(window_);
    SetFocus(window_);
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool ScreenshotOverlay::IsEditingPinnedWindow(
    HWND pinnedWindow) const noexcept {
    return editingPinned_ && pinnedWindowToUpdate_ == pinnedWindow &&
        window_ && IsWindow(window_);
}

bool ScreenshotOverlay::SwitchPinnedEditingForPointerDown(
    HWND pinnedWindow) {
    return editingPinned_ && pinnedWindowToUpdate_ != pinnedWindow &&
        StartPinnedEditing(pinnedWindow);
}

void ScreenshotOverlay::HidePinnedToolbar(HWND pinnedWindow) {
    if (IsEditingPinnedWindow(pinnedWindow)) {
        CompletePinnedEditing(true);
    }
}

void ScreenshotOverlay::SyncPinnedWindowPosition(HWND pinnedWindow,
    int contentScreenX, int contentScreenY, bool dragFinished) {
    if (!IsEditingPinnedWindow(pinnedWindow)) {
        return;
    }

    const RECT previousSelection = selection_;
    const int width = RectWidth(selection_);
    const int height = RectHeight(selection_);
    selection_ = RECT{
        contentScreenX - virtualLeft_,
        contentScreenY - virtualTop_,
        contentScreenX - virtualLeft_ + width,
        contentScreenY - virtualTop_ + height};

    const int deltaX = selection_.left - previousSelection.left;
    const int deltaY = selection_.top - previousSelection.top;
    if (textEditor_ && (deltaX != 0 || deltaY != 0)) {
        RECT editorRect{};
        GetWindowRect(textEditor_, &editorRect);
        MapWindowPoints(nullptr, window_,
            reinterpret_cast<POINT*>(&editorRect), 2);
        SetWindowPos(textEditor_, HWND_TOP,
            editorRect.left + deltaX, editorRect.top + deltaY,
            0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        UpdateTextImePosition();
    }

    toolbarSuppressed_ = false;
    toolbarMoved_ = false;
    UpdateToolbarPosition(true);
    UpdatePinnedEditingRegion();
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | (dragFinished ? RDW_UPDATENOW : 0) |
        RDW_NOERASE | RDW_ALLCHILDREN);
}

void ScreenshotOverlay::Cancel() {
    if (editingPinned_) {
        CompletePinnedEditing(false);
        return;
    }
    DestroyOverlayWindow();
}

bool ScreenshotOverlay::CaptureDesktop() {
    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 0 || virtualHeight <= 0) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP desktopBitmap = screen
        ? CreateCompatibleBitmap(screen, virtualWidth, virtualHeight)
        : nullptr;
    HBITMAP backBufferBitmap = screen
        ? CreateCompatibleBitmap(screen, virtualWidth, virtualHeight)
        : nullptr;
    if (!screen || !memory || !desktopBitmap || !backBufferBitmap) {
        if (desktopBitmap) DeleteObject(desktopBitmap);
        if (backBufferBitmap) DeleteObject(backBufferBitmap);
        if (memory) {
            DeleteDC(memory);
        }
        if (screen) {
            ReleaseDC(nullptr, screen);
        }
        return false;
    }

    HGDIOBJ previous = SelectObject(memory, desktopBitmap);
    const BOOL copied = BitBlt(memory, 0, 0, virtualWidth, virtualHeight,
        screen, virtualLeft, virtualTop, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, backBufferBitmap);
    const BOOL backBufferCopied = BitBlt(memory, 0, 0,
        virtualWidth, virtualHeight, screen, virtualLeft, virtualTop,
        SRCCOPY | CAPTUREBLT);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!copied || !backBufferCopied) {
        DeleteObject(desktopBitmap);
        DeleteObject(backBufferBitmap);
        return false;
    }

    if (desktopBitmap_) DeleteObject(desktopBitmap_);
    if (backBufferBitmap_) DeleteObject(backBufferBitmap_);
    desktopBitmap_ = desktopBitmap;
    backBufferBitmap_ = backBufferBitmap;
    virtualLeft_ = virtualLeft;
    virtualTop_ = virtualTop;
    virtualWidth_ = virtualWidth;
    virtualHeight_ = virtualHeight;
    return true;
}

void ScreenshotOverlay::ReleaseResources() {
    HWND pinnedWindowToRestore = editingPinned_ ? pinnedWindowToUpdate_ : nullptr;
    RestorePinnedDragPreview();
    if (toolbarTooltip_) {
        if (IsWindow(toolbarTooltip_)) {
            DestroyWindow(toolbarTooltip_);
        }
        toolbarTooltip_ = nullptr;
    }
    textEditor_ = nullptr;
    originalTextEditorProc_ = nullptr;
    colorCopied_ = false;
    if (desktopBitmap_) {
        DeleteObject(desktopBitmap_);
        desktopBitmap_ = nullptr;
    }
    if (backBufferBitmap_) {
        DeleteObject(backBufferBitmap_);
        backBufferBitmap_ = nullptr;
    }
    if (toolbarFont_) {
        DeleteObject(toolbarFont_);
        toolbarFont_ = nullptr;
    }
    if (textEditorFont_) {
        DeleteObject(textEditorFont_);
        textEditorFont_ = nullptr;
    }
    if (textEditorBackgroundBrush_) {
        DeleteObject(textEditorBackgroundBrush_);
        textEditorBackgroundBrush_ = nullptr;
    }
    ReleaseNumberEditorCaretBitmap();
    annotations_.clear();
    workingAnnotation_ = {};
    annotationAtDragStart_ = {};
    movingAnnotationIndex_ = -1;
    editingAnnotationIndex_ = -1;
    textEditorWheelRemainder_ = 0;
    textEditorLayoutRefreshPending_ = false;
    suppressTextEditorChange_ = false;
    interaction_ = Interaction::None;
    toolbarDragStarted_ = false;
    annotationDragStarted_ = false;
    selectionResizeStarted_ = false;
    selectionCommitted_ = false;
    hoveredToolbarCommand_ = ToolbarCommand::None;
    pressedToolbarCommand_ = ToolbarCommand::None;
    editingPinned_ = false;
    toolbarSuppressed_ = false;
    toolbarVisibleBeforePinnedDrag_ = false;
    hoveredFontSizePoints_ = -1;
    fontSizeMenuOpen_ = false;
    pinnedWindowToUpdate_ = nullptr;
    if (pinnedWindowToRestore && IsWindow(pinnedWindowToRestore)) {
        ShowWindow(pinnedWindowToRestore, SW_SHOWNOACTIVATE);
        SetWindowPos(pinnedWindowToRestore, HWND_TOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(pinnedWindowToRestore, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    RestorePinnedWindowsAfterCapture();
}

LRESULT CALLBACK ScreenshotOverlay::WindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ScreenshotOverlay* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ScreenshotOverlay*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ScreenshotOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ScreenshotOverlay::TextEditorProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ScreenshotOverlay*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self || !self->originalTextEditorProc_) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (message == kRefreshTextEditorLayoutMessage) {
        self->textEditorLayoutRefreshPending_ = false;
        if (window == self->textEditor_ &&
            self->editingAnnotation_.tool == Tool::Number) {
            self->UpdateTextEditorLayout();
            self->NormalizeNumberEditorViewport();
            self->UpdateTextImePosition();
        }
        return 0;
    }

    if (message == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            self->CommitTextEditing(false);
            return 0;
        }
        if (wParam == VK_DELETE &&
            self->editingAnnotation_.tool == Tool::Number) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                ScreenToClient(self->window_, &cursor);
                if (self->HitTestEditingNumberBadge(cursor)) {
                    const int annotationIndex =
                        self->editingAnnotationIndex_;
                    if (annotationIndex >= 0) {
                        self->DeleteAnnotation(annotationIndex);
                    }
                    self->CommitTextEditing(false);
                    return 0;
                }
            }
        }
    }

    if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return TRUE;
    }

    if (message == WM_MOUSEWHEEL) {
        self->ScrollTextEditor(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }

    if (message == WM_IME_STARTCOMPOSITION) {
        self->UpdateTextImePosition();
    }

    const WNDPROC original = self->originalTextEditorProc_;
    if (message == WM_NCDESTROY) {
        const LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
        self->ReleaseNumberEditorCaretBitmap();
        self->textEditor_ = nullptr;
        self->originalTextEditorProc_ = nullptr;
        self->textEditorLayoutRefreshPending_ = false;
        return result;
    }

    const LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
    if (message == WM_SETFOCUS &&
        self->editingAnnotation_.tool == Tool::Number) {
        self->InstallNumberEditorCaret();
    } else if (message == WM_KILLFOCUS) {
        // The native EDIT procedure has destroyed the caret by this point.
        // Only the bitmap remains owned by ScreenshotOverlay.
        self->ReleaseNumberEditorCaretBitmap();
    }
    const bool draggingSelection =
        message == WM_MOUSEMOVE && (wParam & MK_LBUTTON) != 0;
    if (message == WM_SETFOCUS || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONUP || message == WM_KEYUP ||
        message == WM_CHAR || draggingSelection ||
        message == WM_IME_STARTCOMPOSITION ||
        message == WM_IME_COMPOSITION ||
        message == WM_IME_ENDCOMPOSITION) {
        self->NormalizeNumberEditorViewport();
    }
    const bool deferEditorLayoutRefresh = message == WM_KEYUP ||
        message == WM_CHAR || message == WM_IME_COMPOSITION ||
        message == WM_IME_ENDCOMPOSITION || message == WM_PASTE ||
        message == WM_CUT || message == WM_CLEAR || message == WM_UNDO;
    if (deferEditorLayoutRefresh) {
        if (!self->textEditorLayoutRefreshPending_) {
            self->textEditorLayoutRefreshPending_ = PostMessageW(
                window, kRefreshTextEditorLayoutMessage, 0, 0) != FALSE;
            if (!self->textEditorLayoutRefreshPending_) {
                self->UpdateTextEditorLayout();
                self->NormalizeNumberEditorViewport();
                self->UpdateTextImePosition();
            }
        }
    } else if (message == WM_SETFOCUS || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONUP || message == WM_IME_STARTCOMPOSITION) {
        self->UpdateTextImePosition();
    }
    return result;
}

void ScreenshotOverlay::HandleActivation(WPARAM wParam, LPARAM lParam) {
    if (LOWORD(wParam) == WA_INACTIVE && fontSizeMenuOpen_) {
        CloseFontSizeMenu();
        return;
    }
    if (!editingPinned_ || LOWORD(wParam) != WA_INACTIVE) {
        return;
    }
    const HWND activatedWindow = reinterpret_cast<HWND>(lParam);
    const HWND rootOwner = activatedWindow
        ? GetAncestor(activatedWindow, GA_ROOTOWNER)
        : nullptr;
    const bool activatedPinnedWindow =
        IsPinnedScreenshotWindow(activatedWindow) ||
        IsPinnedScreenshotWindow(rootOwner);
    if (!activatedPinnedWindow &&
        (!activatedWindow || rootOwner != window_)) {
        CompletePinnedEditing(true);
    }
}

void ScreenshotOverlay::HandleLeftButtonDown(POINT point) {
    ActivatePinnedWindowForInteraction(window_);
    if (!selectionCommitted_ && colorPickerHotkeyRegistered_) {
        UnregisterHotKey(window_, kColorPickerHotkeyId);
        colorPickerHotkeyRegistered_ = false;
    }
    const ToolbarCommand toolbarCommand = HitTestToolbar(point);
    if (textEditor_ && editingAnnotation_.tool == Tool::Number &&
        toolbarCommand == ToolbarCommand::None &&
        HitTestEditingNumberBadge(point)) {
        interaction_ = Interaction::MovingEditingAnnotation;
        annotationDragStarted_ = false;
        pointerStart_ = point;
        annotationAtDragStart_ = editingAnnotation_;
        SetCapture(window_);
        SetFocus(window_);
        UpdateCursor(point);
        return;
    }
    if (textEditor_ && toolbarCommand != ToolbarCommand::Undo) {
        const bool editingNumber = editingAnnotation_.tool == Tool::Number;
        CommitTextEditing(true);
        const bool settingsPanelHit = SettingsPanelVisible() &&
            ContainsPoint(settingsPanel_, point);
        const bool fontPopupHit = fontSizeMenuOpen_ &&
            ContainsPoint(FontSizePopupRect(), point);
        if (editingNumber && toolbarCommand == ToolbarCommand::None &&
            ContainsPointInclusive(selection_, point) &&
            !settingsPanelHit && !fontPopupHit) {
            return;
        }
    }
    SetFocus(window_);
    BeginPointerAction(point);
    if (!selectionCommitted_) {
        // Clear preselection now so the first drag move stays inexpensive.
        RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}

void ScreenshotOverlay::HandleMouseMove(POINT point) {
    if (fontSizeMenuOpen_) {
        int hoveredSize = -1;
        const RECT popup = FontSizePopupRect();
        if (ContainsPoint(popup, point) &&
            point.y >= popup.top + 1 && point.y < popup.bottom - 1) {
            const int itemHeight = std::max(
                1, Scale(kFontSizePopupItemHeightDip));
            const int index = (point.y - popup.top - 1) / itemHeight;
            if (index >= 0 && index <=
                kMaximumFontSizePoints - kMinimumFontSizePoints) {
                hoveredSize = kMinimumFontSizePoints + index;
            }
        }
        if (hoveredSize != hoveredFontSizePoints_) {
            hoveredFontSizePoints_ = hoveredSize;
            InvalidateRect(window_, &popup, FALSE);
        }
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window_;
        TrackMouseEvent(&tracking);
    }

    const ToolbarCommand hovered = HitTestToolbar(point);
    if (hovered != hoveredToolbarCommand_) {
        hoveredToolbarCommand_ = hovered;
        InvalidateRect(window_, &toolbar_, FALSE);
    }
    if (interaction_ == Interaction::None && !selectionCommitted_) {
        colorCopied_ = false;
        UpdateHoveredWindow(point);
        InvalidateRect(window_, nullptr, FALSE);
    } else if (interaction_ != Interaction::None) {
        UpdatePointerAction(point);
    }
    UpdateCursor(point);
}

void ScreenshotOverlay::HandleLeftButtonDoubleClick(POINT point) {
    const bool fontTool = activeTool_ == Tool::Text ||
        activeTool_ == Tool::Number;
    if (fontTool && SettingsPanelVisible() &&
        ContainsPoint(FontSizeComboRect(), point)) {
        HandleSettingsPanelClick(point);
        return;
    }
    if (textEditor_) {
        return;
    }
    const int annotationIndex = HitTestAnnotation(point);
    if (annotationIndex < 0 ||
        annotationIndex >= static_cast<int>(annotations_.size())) {
        return;
    }
    const Tool annotationTool =
        annotations_[static_cast<std::size_t>(annotationIndex)].tool;
    if (annotationTool != Tool::Text && annotationTool != Tool::Number) {
        return;
    }
    SetFocus(window_);
    BeginTextEditing(
        annotations_[static_cast<std::size_t>(annotationIndex)],
        annotationIndex, point);
}

void ScreenshotOverlay::HandleKeyDown(WPARAM key) {
    if (key == VK_ESCAPE && interaction_ != Interaction::None) {
        HandleCaptureChanged(nullptr);
        if (GetCapture() == window_) ReleaseCapture();
        return;
    }
    if (fontSizeMenuOpen_ && key == VK_ESCAPE) {
        CloseFontSizeMenu();
    } else if (!selectionCommitted_ && key == 'C') {
        colorCopied_ = CopyCurrentColorToClipboard();
        InvalidateRect(window_, nullptr, FALSE);
    } else if (!selectionCommitted_ && key == VK_ESCAPE) {
        Cancel();
    } else if (key == VK_DELETE) {
        POINT cursor{};
        int annotationIndex = -1;
        if (GetCursorPos(&cursor)) {
            ScreenToClient(window_, &cursor);
            annotationIndex = HitTestAnnotation(cursor, true, true);
        }
        if (annotationIndex >= 0) {
            DeleteAnnotation(annotationIndex);
            InvalidateRect(window_, nullptr, FALSE);
        }
    } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == 'Z' &&
               !annotations_.empty()) {
        DeleteAnnotation(static_cast<int>(annotations_.size()) - 1);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ScreenshotOverlay::HandleCaptureChanged(HWND newCaptureWindow) {
    if (newCaptureWindow == window_) {
        return;
    }
    const bool wasMovingPinned = editingPinned_ &&
        interaction_ == Interaction::MovingPinned;
    const bool wasMovingTextEditor = interaction_ == Interaction::MovingEditingAnnotation;
    const bool restorePinnedRegion = editingPinned_ &&
        (interaction_ == Interaction::MovingToolbar ||
         interaction_ == Interaction::MovingPinned);
    interaction_ = Interaction::None;
    toolbarDragStarted_ = false;
    annotationDragStarted_ = false;
    selectionResizeStarted_ = false;
    pressedToolbarCommand_ = ToolbarCommand::None;
    annotationAtDragStart_ = {};
    movingAnnotationIndex_ = -1;
    if (restorePinnedRegion) {
        FinishPinnedPointerAction(wasMovingPinned);
    }
    workingAnnotation_ = {};
    if (wasMovingTextEditor && textEditor_) {
        UpdateTextEditorLayout();
        ShowWindow(textEditor_, SW_SHOW);
    }
    if (window_) RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
}

LRESULT ScreenshotOverlay::HandleMessage(
    UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == textEditor_) {
            const HDC editorContext = reinterpret_cast<HDC>(wParam);
            if (editingAnnotation_.tool == Tool::Number) {
                SetTextColor(editorContext, RGB(255, 255, 255));
                SetBkColor(editorContext, kNumberLabelBackgroundColor);
                return reinterpret_cast<LRESULT>(textEditorBackgroundBrush_
                    ? textEditorBackgroundBrush_
                    : GetStockObject(GRAY_BRUSH));
            }
            SetTextColor(editorContext, editingAnnotation_.color);
            SetBkColor(editorContext, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        return DefWindowProcW(window_, message, wParam, lParam);

    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == textEditor_ &&
            LOWORD(wParam) == kTextEditorId && HIWORD(wParam) == EN_CHANGE) {
            if (!suppressTextEditorChange_) {
                UpdateTextEditorLayout();
            }
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);

    case WM_MOUSEWHEEL:
        if (textEditor_) {
            ScrollTextEditor(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);

    case capture::OcrFinishedMessage:
        toolbarSuppressed_=false;
        if(wParam) Cancel(); else InvalidateRect(window_,nullptr,FALSE);
        return 0;

    case WM_ACTIVATE:
        HandleActivation(wParam, lParam);
        return 0;

    case WM_LBUTTONDOWN:
        HandleLeftButtonDown(
            POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_MOUSEMOVE:
        HandleMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_MOUSELEAVE:
        if (hoveredFontSizePoints_ >= 0) {
            hoveredFontSizePoints_ = -1;
            const RECT popup = FontSizePopupRect();
            InvalidateRect(window_, &popup, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        EndPointerAction(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_LBUTTONDBLCLK:
        HandleLeftButtonDoubleClick(
            POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;

    case WM_RBUTTONUP:
        if (fontSizeMenuOpen_) {
            CloseFontSizeMenu();
            return 0;
        }
        if (!selectionCommitted_) {
            Cancel();
        }
        return 0;

    case WM_KEYDOWN:
        HandleKeyDown(wParam);
        return 0;

    case WM_HOTKEY:
        if (wParam == kColorPickerHotkeyId && !selectionCommitted_) {
            colorCopied_ = CopyCurrentColorToClipboard();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);

    case WM_CAPTURECHANGED:
        HandleCaptureChanged(reinterpret_cast<HWND>(lParam));
        return 0;

    case WM_CANCELMODE:
        HandleCaptureChanged(nullptr);
        if (GetCapture() == window_) ReleaseCapture();
        return 0;

    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window_, &point);
        UpdateCursor(point);
        return TRUE;
    }

    case WM_NCDESTROY:
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
        if (colorPickerHotkeyRegistered_) {
            UnregisterHotKey(window_, kColorPickerHotkeyId);
            colorPickerHotkeyRegistered_ = false;
        }
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        window_ = nullptr;
        ReleaseResources();
        return 0;

    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

void ScreenshotOverlay::Paint() {
    PAINTSTRUCT paint{};
    HDC deviceContext = BeginPaint(window_, &paint);
    HDC target = CreateCompatibleDC(deviceContext);
    HDC source = CreateCompatibleDC(deviceContext);
    if (!target || !source) {
        if (source) DeleteDC(source);
        if (target) DeleteDC(target);
        EndPaint(window_, &paint);
        return;
    }
    HGDIOBJ previousTargetBitmap = SelectObject(target, backBufferBitmap_);
    HGDIOBJ previousSourceBitmap = SelectObject(source, desktopBitmap_);
    const int targetPaintState = SaveDC(target);
    IntersectClipRect(target, paint.rcPaint.left, paint.rcPaint.top,
        paint.rcPaint.right, paint.rcPaint.bottom);
    BitBlt(target, 0, 0, virtualWidth_, virtualHeight_, source, 0, 0, SRCCOPY);

    if (editingPinned_ && pinnedWindowToUpdate_ &&
        IsWindow(pinnedWindowToUpdate_)) {
        auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
            GetWindowLongPtrW(pinnedWindowToUpdate_, GWLP_USERDATA));
        if (pinnedData && pinnedData->bitmap) {
            HDC pinnedSource = CreateCompatibleDC(deviceContext);
            if (pinnedSource) {
                HBITMAP editingBitmap = pinnedData->baseBitmap
                    ? pinnedData->baseBitmap : pinnedData->bitmap;
                HGDIOBJ previousPinnedBitmap =
                    SelectObject(pinnedSource, editingBitmap);
                BitBlt(target, selection_.left, selection_.top,
                    RectWidth(selection_), RectHeight(selection_),
                    pinnedSource, 0, 0, SRCCOPY);
                SelectObject(pinnedSource, previousPinnedBitmap);
                DeleteDC(pinnedSource);
            }
        }
    }

    const bool hasUsableSelection = HasUsableSelection();
    const bool startingManualSelection =
        !editingPinned_ && interaction_ == Interaction::Selecting;
    if (hasUsableSelection || startingManualSelection) {
        if (!editingPinned_) {
            PaintDimmedOutside(target);
        }
        if (hasUsableSelection) {
            const int saved = SaveDC(target);
            IntersectClipRect(target, selection_.left, selection_.top,
                selection_.right, selection_.bottom);
            // The drag preview carries annotations while the editor is fully
            // transparent. Paint them into the editor as well so its first
            // opaque handoff frame is already complete.
            PaintAnnotations(target, selection_.left, selection_.top);
            if (textEditor_ && editingAnnotation_.tool == Tool::Number) {
                Annotation editingPreview = editingAnnotation_;
                const bool movingEditor =
                    interaction_ == Interaction::MovingEditingAnnotation &&
                    annotationDragStarted_;
                if (!movingEditor) {
                    editingPreview.text.clear();
                }
                PaintAnnotation(target, editingPreview,
                    selection_.left, selection_.top, movingEditor);
            }
            if (interaction_ == Interaction::Drawing) {
                PaintAnnotation(target, workingAnnotation_,
                    selection_.left, selection_.top);
            }
            RestoreDC(target, saved);
            if (!editingPinned_) {
                PaintSelection(target);
            }
            if (ToolbarVisible()) {
                PaintToolbar(target);
                if (SettingsPanelVisible()) {
                    PaintSettingsPanel(target);
                }
            }
        }
    }

    if (!selectionCommitted_ && interaction_ == Interaction::None) {
        PaintColorPicker(target, source);
    }

    BitBlt(deviceContext, paint.rcPaint.left, paint.rcPaint.top,
        paint.rcPaint.right - paint.rcPaint.left,
        paint.rcPaint.bottom - paint.rcPaint.top,
        target, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    RestoreDC(target, targetPaintState);
    SelectObject(source, previousSourceBitmap);
    SelectObject(target, previousTargetBitmap);
    DeleteDC(source);
    DeleteDC(target);
    EndPaint(window_, &paint);
}

void ScreenshotOverlay::PaintDimmedOutside(HDC deviceContext) const {
    HDC source = CreateCompatibleDC(deviceContext);
    HBITMAP pixel = CreateCompatibleBitmap(deviceContext, 1, 1);
    HGDIOBJ previous = SelectObject(source, pixel);
    RECT one{0, 0, 1, 1};
    FillSolidRect(source, one, RGB(0, 0, 0));
    BLENDFUNCTION blend{AC_SRC_OVER, 0, kOutsideDimAlpha, 0};

    const RECT client = ClientBounds();
    const std::array<RECT, 4> dimmed{{
        {client.left, client.top, client.right, selection_.top},
        {client.left, selection_.bottom, client.right, client.bottom},
        {client.left, selection_.top, selection_.left, selection_.bottom},
        {selection_.right, selection_.top, client.right, selection_.bottom},
    }};
    for (const RECT& rect : dimmed) {
        if (RectWidth(rect) > 0 && RectHeight(rect) > 0) {
            AlphaBlend(deviceContext, rect.left, rect.top,
                RectWidth(rect), RectHeight(rect), source, 0, 0, 1, 1, blend);
        }
    }
    SelectObject(source, previous);
    DeleteObject(pixel);
    DeleteDC(source);
}

void ScreenshotOverlay::PaintSelection(HDC deviceContext) const {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), kSelectionColor);
    HGDIOBJ oldPen = SelectObject(deviceContext, pen);
    HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
    Rectangle(deviceContext, selection_.left, selection_.top,
        selection_.right, selection_.bottom);
    SelectObject(deviceContext, oldBrush);
    SelectObject(deviceContext, oldPen);
    DeleteObject(pen);

    if (selectionCommitted_) {
        const int radius = Scale(kHandleRadiusDip);
        const int centerX = (selection_.left + selection_.right) / 2;
        const int centerY = (selection_.top + selection_.bottom) / 2;
        const std::array<POINT, 8> handles{{
            {selection_.left, selection_.top}, {centerX, selection_.top},
            {selection_.right, selection_.top}, {selection_.left, centerY},
            {selection_.right, centerY}, {selection_.left, selection_.bottom},
            {centerX, selection_.bottom}, {selection_.right, selection_.bottom},
        }};
        HBRUSH handleBrush = CreateSolidBrush(kSelectionColor);
        for (const POINT& point : handles) {
            RECT handle{point.x - radius, point.y - radius,
                point.x + radius + 1, point.y + radius + 1};
            FillRect(deviceContext, &handle, handleBrush);
        }
        DeleteObject(handleBrush);
    }

    wchar_t dimensions[64]{};
    swprintf_s(dimensions, L"%d × %d", RectWidth(selection_), RectHeight(selection_));
    const HFONT oldFont = static_cast<HFONT>(SelectObject(deviceContext, toolbarFont_));
    SIZE textSize{};
    GetTextExtentPoint32W(deviceContext, dimensions,
        static_cast<int>(wcslen(dimensions)), &textSize);
    int labelY = selection_.top - textSize.cy - Scale(6);
    if (labelY < 0) {
        labelY = selection_.top + Scale(5);
    }
    RECT label{
        selection_.left, labelY,
        selection_.left + textSize.cx + Scale(10), labelY + textSize.cy + Scale(4)};
    FillSolidRect(deviceContext, label, RGB(28, 31, 36));
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(255, 255, 255));
    RECT textRect = label;
    textRect.left += Scale(5);
    DrawTextW(deviceContext, dimensions, -1, &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(deviceContext, oldFont);
}

void ScreenshotOverlay::PaintColorPicker(
    HDC deviceContext, HDC desktopContext) const {
    if (!window_ || !desktopContext) {
        return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }
    const POINT screenPoint = cursor;
    ScreenToClient(window_, &cursor);
    cursor.x = std::clamp<LONG>(cursor.x, 0, virtualWidth_ - 1);
    cursor.y = std::clamp<LONG>(cursor.y, 0, virtualHeight_ - 1);

    // Reserve enough room for the widest RGB and hexadecimal color values.
    const int panelWidth = Scale(204);
    const int previewHeight = Scale(104);
    const int informationHeight = Scale(70);
    const int panelHeight = previewHeight + informationHeight;
    int panelX = cursor.x + Scale(22);
    int panelY = cursor.y + Scale(22);
    if (panelX + panelWidth > virtualWidth_ - Scale(8)) {
        panelX = cursor.x - panelWidth - Scale(22);
    }
    if (panelY + panelHeight > virtualHeight_ - Scale(8)) {
        panelY = cursor.y - panelHeight - Scale(22);
    }
    panelX = std::clamp(panelX, 0, std::max(0, virtualWidth_ - panelWidth));
    panelY = std::clamp(panelY, 0, std::max(0, virtualHeight_ - panelHeight));

    RECT panel{panelX, panelY, panelX + panelWidth, panelY + panelHeight};
    RECT shadow = OffsetRectValue(panel, Scale(2), Scale(2));
    FillSolidRect(deviceContext, shadow, RGB(120, 120, 120));
    FillSolidRect(deviceContext, panel, RGB(250, 250, 250));
    RECT preview{panel.left + Scale(2), panel.top + Scale(2),
        panel.right - Scale(2), panel.top + previewHeight};
    FillSolidRect(deviceContext, preview, RGB(245, 245, 245));

    constexpr int sampleColumns = 17;
    constexpr int sampleRows = 11;
    constexpr int centerColumn = sampleColumns / 2;
    constexpr int centerRow = sampleRows / 2;
    const int requestedLeft = cursor.x - centerColumn;
    const int requestedTop = cursor.y - centerRow;
    const int sourceLeft = std::max(0, requestedLeft);
    const int sourceTop = std::max(0, requestedTop);
    const int sourceRight = std::min(virtualWidth_, requestedLeft + sampleColumns);
    const int sourceBottom = std::min(virtualHeight_, requestedTop + sampleRows);
    const int previewWidth = RectWidth(preview);
    const int previewPixelsHeight = RectHeight(preview);
    const int destinationLeft = preview.left + MulDiv(
        sourceLeft - requestedLeft, previewWidth, sampleColumns);
    const int destinationTop = preview.top + MulDiv(
        sourceTop - requestedTop, previewPixelsHeight, sampleRows);
    const int destinationRight = preview.left + MulDiv(
        sourceRight - requestedLeft, previewWidth, sampleColumns);
    const int destinationBottom = preview.top + MulDiv(
        sourceBottom - requestedTop, previewPixelsHeight, sampleRows);
    const int previousStretchMode = SetStretchBltMode(
        deviceContext, COLORONCOLOR);
    if (sourceRight > sourceLeft && sourceBottom > sourceTop) {
        StretchBlt(deviceContext, destinationLeft, destinationTop,
            destinationRight - destinationLeft,
            destinationBottom - destinationTop,
            desktopContext, sourceLeft, sourceTop,
            sourceRight - sourceLeft, sourceBottom - sourceTop, SRCCOPY);
    }
    SetStretchBltMode(deviceContext, previousStretchMode);

    const int crossX = preview.left + MulDiv(
        centerColumn * 2 + 1, previewWidth, sampleColumns * 2);
    const int crossY = preview.top + MulDiv(
        centerRow * 2 + 1, previewPixelsHeight, sampleRows * 2);
    HPEN crossPen = CreatePen(PS_SOLID, std::max(1, Scale(2)), kSelectionColor);
    HGDIOBJ previousPen = SelectObject(deviceContext, crossPen);
    MoveToEx(deviceContext, preview.left, crossY, nullptr);
    LineTo(deviceContext, preview.right, crossY);
    MoveToEx(deviceContext, crossX, preview.top, nullptr);
    LineTo(deviceContext, crossX, preview.bottom);
    SelectObject(deviceContext, previousPen);
    DeleteObject(crossPen);

    RECT information{panel.left, panel.top + previewHeight,
        panel.right, panel.bottom};
    FillSolidRect(deviceContext, information, RGB(48, 48, 48));
    COLORREF color = GetPixel(desktopContext, cursor.x, cursor.y);
    if (color == CLR_INVALID) color = RGB(0, 0, 0);
    wchar_t details[160]{};
    swprintf_s(details,
        L"%ld × %ld\nRGB(%u,%u,%u)  #%02X%02X%02X\n%ls",
        screenPoint.x, screenPoint.y,
        GetRValue(color), GetGValue(color), GetBValue(color),
        GetRValue(color), GetGValue(color), GetBValue(color),
        colorCopied_ ? L"已复制色号" : L"按 C 复制色号");
    HGDIOBJ previousFont = SelectObject(deviceContext, toolbarFont_);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(255, 255, 255));
    RECT textRect = information;
    textRect.left += Scale(8);
    textRect.right -= Scale(5);
    textRect.top += Scale(3);
    DrawTextW(deviceContext, details, -1, &textRect,
        DT_LEFT | DT_TOP | DT_NOPREFIX);
    SelectObject(deviceContext, previousFont);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(45, 45, 45));
    previousPen = SelectObject(deviceContext, borderPen);
    HGDIOBJ previousBrush = SelectObject(
        deviceContext, GetStockObject(HOLLOW_BRUSH));
    Rectangle(deviceContext, panel.left, panel.top, panel.right, panel.bottom);
    SelectObject(deviceContext, previousBrush);
    SelectObject(deviceContext, previousPen);
    DeleteObject(borderPen);
}

COLORREF ScreenshotOverlay::SampleDesktopColor(POINT clientPoint) const {
    if (!desktopBitmap_ || virtualWidth_ <= 0 || virtualHeight_ <= 0) {
        return CLR_INVALID;
    }
    clientPoint.x = std::clamp<LONG>(clientPoint.x, 0, virtualWidth_ - 1);
    clientPoint.y = std::clamp<LONG>(clientPoint.y, 0, virtualHeight_ - 1);
    HDC screen = GetDC(nullptr);
    HDC source = screen ? CreateCompatibleDC(screen) : nullptr;
    if (!screen || !source) {
        if (source) DeleteDC(source);
        if (screen) ReleaseDC(nullptr, screen);
        return CLR_INVALID;
    }
    HGDIOBJ previousBitmap = SelectObject(source, desktopBitmap_);
    const COLORREF color = GetPixel(source, clientPoint.x, clientPoint.y);
    SelectObject(source, previousBitmap);
    DeleteDC(source);
    ReleaseDC(nullptr, screen);
    return color;
}

bool ScreenshotOverlay::CopyCurrentColorToClipboard() const {
    if (!window_ || selectionCommitted_) {
        return false;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return false;
    }
    ScreenToClient(window_, &cursor);
    const COLORREF color = SampleDesktopColor(cursor);
    if (color == CLR_INVALID) {
        return false;
    }
    wchar_t colorText[16]{};
    swprintf_s(colorText, L"#%02X%02X%02X",
        GetRValue(color), GetGValue(color), GetBValue(color));
    const SIZE_T byteCount = (wcslen(colorText) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!memory) {
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    CopyMemory(destination, colorText, byteCount);
    GlobalUnlock(memory);
    if (!OpenClipboard(window_)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const HANDLE result = SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!result) {
        GlobalFree(memory);
        return false;
    }
    return true;
}

void ScreenshotOverlay::PaintAnnotations(
    HDC deviceContext, int offsetX, int offsetY) const {
    for (std::size_t index = 0; index < annotations_.size(); ++index) {
        if (static_cast<int>(index) == editingAnnotationIndex_) {
            continue;
        }
        PaintAnnotation(deviceContext, annotations_[index], offsetX, offsetY);
    }
}

void ScreenshotOverlay::PaintAnnotation(HDC dc, const Annotation& annotation, int x, int y, bool empty) const {
    if (annotation.tool == Tool::Mosaic) PaintMosaicAnnotation(dc, annotation, x, y);
    else capture::AnnotationPainter(dpi_).PaintAnnotation(dc, annotation, x, y, empty);
}

void ScreenshotOverlay::PaintMosaicAnnotation(
    HDC deviceContext, const Annotation& annotation,
    int offsetX, int offsetY) const {
    capture::PaintMosaic(deviceContext,annotation,dpi_,offsetX,offsetY);
}
std::vector<ScreenshotOverlay::ToolbarButton> ScreenshotOverlay::ToolbarButtons() const {
    const int gripWidth = Scale(kToolbarGripWidthDip);
    const int buttonWidth = Scale(kToolbarButtonWidthDip);
    int x = toolbar_.left;
    std::vector<ToolbarButton> buttons;
    buttons.reserve(17);
    x += gripWidth;
    const std::array<std::pair<ToolbarCommand, const wchar_t*>, 17> definitions{{
        {ToolbarCommand::Rectangle, L"矩形"},
        {ToolbarCommand::Ellipse, L"椭圆"},
        {ToolbarCommand::Arrow, L"箭头"},
        {ToolbarCommand::Pen, L"画笔"},
        {ToolbarCommand::Mosaic, L"马赛克"},
        {ToolbarCommand::Text, L"文本"},
        {ToolbarCommand::Number, L"序号标注"},
        {ToolbarCommand::Undo, L"撤销"},
        {ToolbarCommand::Clear, L"清除标注"},
        {ToolbarCommand::LongCapture, L"长截图"},
        {ToolbarCommand::Ocr, L"文字识别"},
        {ToolbarCommand::Gif, L"GIF 录制"},
        {ToolbarCommand::Record, L"屏幕录制"},
        {ToolbarCommand::Pin, L"钉在桌面"},
        {ToolbarCommand::Save, L"保存"},
        {ToolbarCommand::Cancel, L"取消"},
        {ToolbarCommand::Finish, L"完成"},
    }};
    for (const auto& definition : definitions) {
        const int currentWidth = definition.first == ToolbarCommand::Finish
            ? Scale(kFinishButtonWidthDip)
            : buttonWidth;
        buttons.push_back({definition.first,
            RECT{x, toolbar_.top, x + currentWidth, toolbar_.bottom}, definition.second});
        x += currentWidth;
    }
    return buttons;
}

void ScreenshotOverlay::PaintToolbar(HDC deviceContext) const {
    FillSolidRect(deviceContext, toolbar_, kToolbarBackgroundColor);

    for (const ToolbarButton& button : ToolbarButtons()) {
        const bool enabled = IsToolbarCommandEnabled(button.command);
        bool selected = false;
        switch (button.command) {
        case ToolbarCommand::Rectangle: selected = activeTool_ == Tool::Rectangle; break;
        case ToolbarCommand::Ellipse: selected = activeTool_ == Tool::Ellipse; break;
        case ToolbarCommand::Arrow: selected = activeTool_ == Tool::Arrow; break;
        case ToolbarCommand::Pen: selected = activeTool_ == Tool::Pen; break;
        case ToolbarCommand::Mosaic: selected = activeTool_ == Tool::Mosaic; break;
        case ToolbarCommand::Text: selected = activeTool_ == Tool::Text; break;
        case ToolbarCommand::Number: selected = activeTool_ == Tool::Number; break;
        default: break;
        }
        if (button.command != ToolbarCommand::Grip)
            capture::DrawToolbarButtonState(deviceContext, button.rect, dpi_, enabled, selected,
                button.command == hoveredToolbarCommand_, button.command == pressedToolbarCommand_);
        PaintToolbarIcon(deviceContext, button.command, button.rect);
        const bool groupEnd = button.command == ToolbarCommand::Grip ||
            button.command == ToolbarCommand::Number ||
            button.command == ToolbarCommand::Save;
        if (groupEnd) {
            HPEN divider = CreatePen(PS_SOLID, 1, kToolbarDividerColor);
            HGDIOBJ oldDivider = SelectObject(deviceContext, divider);
            MoveToEx(deviceContext, button.rect.right - 1,
                button.rect.top + Scale(8), nullptr);
            LineTo(deviceContext, button.rect.right - 1,
                button.rect.bottom - Scale(8));
            SelectObject(deviceContext, oldDivider);
            DeleteObject(divider);
        }
    }

    HPEN border = CreatePen(PS_SOLID, 1, kToolbarBorderColor);
    HGDIOBJ oldPen = SelectObject(deviceContext, border);
    HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
    Rectangle(deviceContext, toolbar_.left, toolbar_.top, toolbar_.right, toolbar_.bottom);
    SelectObject(deviceContext, oldBrush);
    SelectObject(deviceContext, oldPen);
    DeleteObject(border);
}

void ScreenshotOverlay::PaintToolbarIcon(HDC dc,ToolbarCommand command,const RECT& rect) const {
    const COLORREF color=IsToolbarCommandEnabled(command)?kToolbarIconColor:kToolbarDisabledColor;
    using capture::ToolbarIcon;
    switch(command) {
    case ToolbarCommand::None: capture::DrawToolbarIcon(dc,ToolbarIcon::None,rect,dpi_,color); break;
    case ToolbarCommand::Grip: capture::DrawToolbarIcon(dc,ToolbarIcon::Grip,rect,dpi_,color); break;
    case ToolbarCommand::Rectangle: capture::DrawToolbarIcon(dc,ToolbarIcon::Rectangle,rect,dpi_,color); break;
    case ToolbarCommand::Ellipse: capture::DrawToolbarIcon(dc,ToolbarIcon::Ellipse,rect,dpi_,color); break;
    case ToolbarCommand::Arrow: capture::DrawToolbarIcon(dc,ToolbarIcon::Arrow,rect,dpi_,color); break;
    case ToolbarCommand::Pen: capture::DrawToolbarIcon(dc,ToolbarIcon::Pen,rect,dpi_,color); break;
    case ToolbarCommand::Mosaic: capture::DrawToolbarIcon(dc,ToolbarIcon::Mosaic,rect,dpi_,color); break;
    case ToolbarCommand::Text: capture::DrawToolbarIcon(dc,ToolbarIcon::Text,rect,dpi_,color); break;
    case ToolbarCommand::Number: capture::DrawToolbarIcon(dc,ToolbarIcon::Number,rect,dpi_,color); break;
    case ToolbarCommand::Undo: capture::DrawToolbarIcon(dc,ToolbarIcon::Undo,rect,dpi_,color); break;
    case ToolbarCommand::Clear: capture::DrawToolbarIcon(dc,ToolbarIcon::Clear,rect,dpi_,color); break;
    case ToolbarCommand::LongCapture: capture::DrawToolbarIcon(dc,ToolbarIcon::LongCapture,rect,dpi_,color); break;
    case ToolbarCommand::Ocr: capture::DrawToolbarIcon(dc,ToolbarIcon::Ocr,rect,dpi_,color); break;
    case ToolbarCommand::Gif: capture::DrawToolbarIcon(dc,ToolbarIcon::Gif,rect,dpi_,color); break;
    case ToolbarCommand::Record: capture::DrawToolbarIcon(dc,ToolbarIcon::Record,rect,dpi_,color); break;
    case ToolbarCommand::Pin: capture::DrawToolbarIcon(dc,ToolbarIcon::Pin,rect,dpi_,color); break;
    case ToolbarCommand::Save: capture::DrawToolbarIcon(dc,ToolbarIcon::Save,rect,dpi_,color); break;
    case ToolbarCommand::Cancel: capture::DrawToolbarIcon(dc,ToolbarIcon::Cancel,rect,dpi_,color); break;
    case ToolbarCommand::Finish: capture::DrawToolbarIcon(dc,ToolbarIcon::Finish,rect,dpi_,color); break;
    }
}

void ScreenshotOverlay::PaintSettingsPanel(HDC deviceContext) const {
    FillSolidRect(deviceContext, settingsPanel_, kToolbarBackgroundColor);
    HPEN border = CreatePen(PS_SOLID, 1, kToolbarBorderColor);
    HGDIOBJ previousPen = SelectObject(deviceContext, border);
    HGDIOBJ previousBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
    Rectangle(deviceContext, settingsPanel_.left, settingsPanel_.top,
        settingsPanel_.right, settingsPanel_.bottom);

    const bool fontTool = activeTool_ == Tool::Text ||
        activeTool_ == Tool::Number;
    const int sizeAreaWidth = Scale(
        fontTool ? kFontSizeControlWidthDip : 104);
    const int slotWidth = Scale(32);
    const int centerY = (settingsPanel_.top + settingsPanel_.bottom) / 2;
    Gdiplus::Graphics vectorGraphics(deviceContext);
    vectorGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    vectorGraphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    vectorGraphics.SetCompositingQuality(
        Gdiplus::CompositingQualityHighQuality);
    if (fontTool) {
        const RECT iconRect{
            settingsPanel_.left + Scale(4), settingsPanel_.top,
            settingsPanel_.left + Scale(34), settingsPanel_.bottom};
        capture::DrawToolbarIcon(deviceContext,capture::ToolbarIcon::Text,iconRect,dpi_,RGB(38,46,57),22.0F);

        capture::DrawToolbarFontCombo(deviceContext,FontSizeComboRect(),dpi_,fontSizePoints_);
    } else {
        for (int index = 0; index < 3; ++index) {
            const int centerX = settingsPanel_.left + Scale(8) +
                index * slotWidth + slotWidth / 2;
            if ((activeTool_ == Tool::Mosaic
                    ? mosaicSizeIndex_ : activeSizeIndex_) == index) {
                RECT selected{centerX - Scale(13), centerY - Scale(15),
                    centerX + Scale(13), centerY + Scale(15)};
                FillSolidRect(deviceContext, selected, kToolbarSelectedColor);
            }
            const int diameter = activeTool_ == Tool::Mosaic
                ? Scale(std::array<int, 3>{6, 11, 16}[index])
                : Scale(std::array<int, 3>{4, 7, 11}[index]);
            const COLORREF dotColor = activeTool_ == Tool::Mosaic
                ? RGB(55, 120, 210) : activeColor_;
            Gdiplus::SolidBrush dotBrush(Gdiplus::Color(255,
                GetRValue(dotColor), GetGValue(dotColor),
                GetBValue(dotColor)));
            vectorGraphics.FillEllipse(&dotBrush,
                static_cast<float>(centerX) - diameter / 2.0F,
                static_cast<float>(centerY) - diameter / 2.0F,
                static_cast<float>(diameter), static_cast<float>(diameter));
        }
    }

    HPEN divider = CreatePen(PS_SOLID, 1, kToolbarDividerColor);
    HGDIOBJ oldDivider = SelectObject(deviceContext, divider);
    MoveToEx(deviceContext, settingsPanel_.left + sizeAreaWidth,
        settingsPanel_.top + Scale(7), nullptr);
    LineTo(deviceContext, settingsPanel_.left + sizeAreaWidth,
        settingsPanel_.bottom - Scale(7));
    SelectObject(deviceContext, oldDivider);
    DeleteObject(divider);

    if (activeTool_ == Tool::Mosaic) {
        const HFONT previousFont = static_cast<HFONT>(
            SelectObject(deviceContext, toolbarFont_));
        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, kToolbarIconColor);
        RECT label{settingsPanel_.left + Scale(112), settingsPanel_.top,
            settingsPanel_.left + Scale(165), settingsPanel_.bottom};
        DrawTextW(deviceContext, L"模糊度", -1, &label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        const int trackLeft = settingsPanel_.left + Scale(166);
        const int trackRight = settingsPanel_.right - Scale(14);
        const int trackY = centerY;
        HPEN trackPen = CreatePen(PS_SOLID, Scale(3), RGB(195, 199, 205));
        HGDIOBJ oldTrackPen = SelectObject(deviceContext, trackPen);
        MoveToEx(deviceContext, trackLeft, trackY, nullptr);
        LineTo(deviceContext, trackRight, trackY);
        SelectObject(deviceContext, oldTrackPen);
        DeleteObject(trackPen);
        const int thumbX = trackLeft + MulDiv(trackRight - trackLeft,
            mosaicBlurLevel_ - 1, 4);
        Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 39, 145, 230));
        const float thumbRadius = static_cast<float>(Scale(6));
        vectorGraphics.FillEllipse(&thumbBrush,
            thumbX - thumbRadius, trackY - thumbRadius,
            thumbRadius * 2.0F, thumbRadius * 2.0F);
        SelectObject(deviceContext, previousFont);
    } else {
        const auto layout=capture::MakeStylePanelLayout({settingsPanel_.left,settingsPanel_.top},dpi_,fontTool,false);
        for (std::size_t index = 0; index < kAnnotationColors.size(); ++index) {
            RECT swatch=layout.colors[index];            FillSolidRect(deviceContext, swatch, kAnnotationColors[index]);
            if (kAnnotationColors[index] == activeColor_) {
                HPEN selectedPen = CreatePen(PS_SOLID, Scale(2), RGB(20, 20, 20));
                HGDIOBJ oldSelectedPen = SelectObject(deviceContext, selectedPen);
                HGDIOBJ oldSelectedBrush = SelectObject(
                    deviceContext, GetStockObject(HOLLOW_BRUSH));
                Rectangle(deviceContext, swatch.left - Scale(2), swatch.top - Scale(2),
                    swatch.right + Scale(2), swatch.bottom + Scale(2));
                SelectObject(deviceContext, oldSelectedBrush);
                SelectObject(deviceContext, oldSelectedPen);
                DeleteObject(selectedPen);
            }
        }
    }

    if (fontTool && fontSizeMenuOpen_) {
        const RECT popup = FontSizePopupRect();
        FillSolidRect(deviceContext, popup, RGB(255, 255, 255));
        Rectangle(deviceContext, popup.left, popup.top,
            popup.right, popup.bottom);
        const HFONT oldFont = static_cast<HFONT>(
            SelectObject(deviceContext, toolbarFont_));
        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, kToolbarIconColor);
        const int itemHeight = Scale(kFontSizePopupItemHeightDip);
        const int highlightedSize = hoveredFontSizePoints_ >=
                kMinimumFontSizePoints && hoveredFontSizePoints_ <=
                kMaximumFontSizePoints
            ? hoveredFontSizePoints_
            : fontSizePoints_;
        for (int size = kMinimumFontSizePoints;
             size <= kMaximumFontSizePoints; ++size) {
            const int index = size - kMinimumFontSizePoints;
            RECT item{popup.left + 1,
                popup.top + 1 + index * itemHeight,
                popup.right - 1,
                popup.top + 1 + (index + 1) * itemHeight};
            if (size == highlightedSize) {
                FillSolidRect(deviceContext, item, kToolbarSelectedColor);
            }
            wchar_t label[8]{};
            swprintf_s(label, L"%d", size);
            DrawTextW(deviceContext, label, -1, &item,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(deviceContext, oldFont);
    }

    SelectObject(deviceContext, previousBrush);
    SelectObject(deviceContext, previousPen);
    DeleteObject(border);
}

BOOL CALLBACK ScreenshotOverlay::FindWindowCallback(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowSearchContext*>(parameter);
    if (!context || !IsWindowVisible(window) || IsIconic(window) ||
        GetAncestor(window, GA_ROOT) != window) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    // Skip our capture surfaces and decorative helpers, not application windows.
    const auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (window == context->self->window_ ||
        (processId == GetCurrentProcessId() &&
         (extendedStyle & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)) != 0 &&
         (extendedStyle & WS_EX_APPWINDOW) == 0)) {
        return TRUE;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(
            window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }

    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(
            window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        GetWindowRect(window, &rect);
    }
    if (RectWidth(rect) <= 1 || RectHeight(rect) <= 1 ||
        !ContainsPoint(rect, context->screenPoint)) {
        return TRUE;
    }
    context->result = window;
    context->resultRect = rect;
    return FALSE;
}

bool ScreenshotOverlay::FindWindowAtScreenPoint(
    POINT screenPoint, RECT& screenRect) const {
    WindowSearchContext context{};
    context.self = const_cast<ScreenshotOverlay*>(this);
    context.screenPoint = screenPoint;
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&context));
    if (!context.result) {
        return false;
    }
    screenRect = context.resultRect;
    return true;
}

void ScreenshotOverlay::UpdateHoveredWindow(POINT clientPoint) {
    POINT screenPoint{clientPoint.x + virtualLeft_, clientPoint.y + virtualTop_};
    RECT screenRect{};
    if (!FindWindowAtScreenPoint(screenPoint, screenRect)) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        screenRect = monitorInfo.rcMonitor;
    }
    const RECT candidate = ClampRectToClient(
        OffsetRectValue(screenRect, -virtualLeft_, -virtualTop_));
    if (!EqualRect(&candidate, &selection_)) {
        selection_ = candidate;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ScreenshotOverlay::BeginPointerAction(POINT point) {
    if (HandleSettingsPanelClick(point)) {
        return;
    }
    const ToolbarCommand toolbarCommand = HitTestToolbar(point);
    if (toolbarCommand != ToolbarCommand::None) {
        if (!IsToolbarCommandEnabled(toolbarCommand)) {
            return;
        }
        if (toolbarCommand == ToolbarCommand::Grip || IsToolbarDragEdge(point)) {
            interaction_ = Interaction::MovingToolbar;
            pointerStart_ = point;
            toolbarDragOffset_ = POINT{point.x - toolbar_.left, point.y - toolbar_.top};
            toolbarMoved_ = true;
            toolbarDragStarted_ = false;
            SetCapture(window_);
        } else {
            interaction_ = Interaction::PressingToolbar;
            pressedToolbarCommand_ = toolbarCommand;
            SetCapture(window_);
            InvalidateRect(window_, &toolbar_, FALSE);
        }
        return;
    }

    const ResizeHandle handle = HitTestResizeHandle(point);
    if (handle != ResizeHandle::None) {
        interaction_ = Interaction::Resizing;
        resizeHandle_ = handle;
        pointerStart_ = point;
        selectionAtDragStart_ = selection_;
        selectionResizeStarted_ = false;
        SetCapture(window_);
        return;
    }

    if (const int annotationIndex = HitTestAnnotation(point);
        annotationIndex >= 0) {
        interaction_ = Interaction::MovingAnnotation;
        annotationDragStarted_ = false;
        movingAnnotationIndex_ = annotationIndex;
        annotationAtDragStart_ =
            annotations_[static_cast<std::size_t>(annotationIndex)];
        pointerStart_ = point;
        SetCapture(window_);
        return;
    }

    if (activeTool_ == Tool::None) {
        const int mosaicIndex = HitTestAnnotation(point, true);
        if (mosaicIndex >= 0) {
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
    }

    if (editingPinned_ && activeTool_ == Tool::None &&
        ContainsPointInclusive(selection_, point)) {
        toolbarVisibleBeforePinnedDrag_ = ToolbarVisible();
        interaction_ = Interaction::MovingPinned;
        annotationDragStarted_ = false;
        pointerStart_ = point;
        selectionAtDragStart_ = selection_;
        toolbarAtDragStart_ = toolbar_;
        SetCapture(window_);
        return;
    }

    if (!editingPinned_ && selectionCommitted_ && activeTool_ == Tool::None &&
        ContainsPointInclusive(selection_, point)) {
        interaction_ = Interaction::MovingSelection;
        pointerStart_ = point;
        selectionAtDragStart_ = selection_;
        toolbarAtDragStart_ = toolbar_;
        SetCapture(window_);
        return;
    }

    if (activeTool_ != Tool::None && ContainsPoint(selection_, point)) {
        interaction_ = Interaction::Drawing;
        workingAnnotation_ = {};
        workingAnnotation_.tool = activeTool_;
        workingAnnotation_.start = ToSelectionPoint(point);
        workingAnnotation_.end = workingAnnotation_.start;
        workingAnnotation_.color = activeColor_;
        workingAnnotation_.size = CurrentToolSize();
        workingAnnotation_.blurLevel = mosaicBlurLevel_;
        if (activeTool_ == Tool::Number) {
            workingAnnotation_.number = NextNumberAfterAnnotations();
        }
        if (activeTool_ == Tool::Pen || activeTool_ == Tool::Mosaic) {
            workingAnnotation_.points.push_back(workingAnnotation_.start);
        }
        SetCapture(window_);
        return;
    }

    if (editingPinned_) {
        return;
    }

    interaction_ = Interaction::PendingSelection;
    pointerStart_ = point;
    selectionAtDragStart_ = selection_;
    SetCapture(window_);
}

void ScreenshotOverlay::RedrawSelectionTransition(
    const RECT& previousSelection,
    const std::array<RECT, 4>& extraDirtyRects) {
    const RECT client = ClientBounds();
    HRGN dirtyRegion = CreateRectRgn(0, 0, 0, 0);
    if (!dirtyRegion) {
        RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        return;
    }

    const auto addRect = [&](RECT rect) {
        if (!IntersectRect(&rect, &rect, &client) ||
            RectWidth(rect) <= 0 || RectHeight(rect) <= 0) {
            return;
        }
        HRGN part = CreateRectRgnIndirect(&rect);
        if (part) {
            CombineRgn(dirtyRegion, dirtyRegion, part, RGN_OR);
            DeleteObject(part);
        }
    };

    // Only pixels entering or leaving the selection change their dim state.
    HRGN previousRegion = CreateRectRgnIndirect(&previousSelection);
    HRGN currentRegion = CreateRectRgnIndirect(&selection_);
    if (previousRegion && currentRegion) {
        HRGN changedFill = CreateRectRgn(0, 0, 0, 0);
        if (changedFill) {
            CombineRgn(changedFill, previousRegion, currentRegion, RGN_XOR);
            CombineRgn(dirtyRegion, dirtyRegion, changedFill, RGN_OR);
            DeleteObject(changedFill);
        }
    }
    if (previousRegion) {
        DeleteObject(previousRegion);
    }
    if (currentRegion) {
        DeleteObject(currentRegion);
    }

    const auto addSelectionChrome = [&](const RECT& rect) {
        RECT outline = rect;
        InflateRect(&outline, Scale(4), Scale(4));
        addRect(outline);

        // Cover both possible size-label positions around the top edge.
        addRect(RECT{rect.left - Scale(2), rect.top - Scale(34),
            rect.left + Scale(150), rect.top + Scale(34)});
    };
    addSelectionChrome(previousSelection);
    addSelectionChrome(selection_);

    for (const Annotation& annotation : annotations_) {
        RECT annotationBounds = AnnotationBounds(annotation);
        InflateRect(&annotationBounds,
            std::max(Scale(6), annotation.size + Scale(2)),
            std::max(Scale(6), annotation.size + Scale(2)));
        RECT previousBounds = annotationBounds;
        OffsetRect(&previousBounds,
            previousSelection.left, previousSelection.top);
        addRect(previousBounds);
        OffsetRect(&annotationBounds, selection_.left, selection_.top);
        addRect(annotationBounds);
    }
    for (const RECT& rect : extraDirtyRects) {
        addRect(rect);
    }

    RedrawWindow(window_, nullptr, dirtyRegion,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    DeleteObject(dirtyRegion);
}

void ScreenshotOverlay::UpdateMovingAnnotation(POINT point) {
    if (movingAnnotationIndex_ < 0 ||
        movingAnnotationIndex_ >= static_cast<int>(annotations_.size())) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (!annotationDragStarted_) {
        if (!HasExceededSystemDragThreshold(point)) {
            return;
        }
        annotationDragStarted_ = true;
    }

    Annotation& annotation =
        annotations_[static_cast<std::size_t>(movingAnnotationIndex_)];
    if(annotation.fragmentGroup) {
        RECT bounds=AnnotationBounds(annotation);
        for(const auto& part:annotations_)if(part.fragmentGroup==annotation.fragmentGroup){auto r=AnnotationBounds(part);UnionRect(&bounds,&bounds,&r);}
        int dx=annotationAtDragStart_.start.x+point.x-pointerStart_.x-annotation.start.x;
        int dy=annotationAtDragStart_.start.y+point.y-pointerStart_.y-annotation.start.y;
        dx=std::clamp(dx,-int(bounds.left),std::max(-int(bounds.left),RectWidth(selection_)-int(bounds.right)));
        dy=std::clamp(dy,-int(bounds.top),std::max(-int(bounds.top),RectHeight(selection_)-int(bounds.bottom)));
        for(auto& part:annotations_)if(part.fragmentGroup==annotation.fragmentGroup)OffsetAnnotation(part,dx,dy);
        InvalidateRect(window_,nullptr,FALSE);return;
    }
    RECT previousBounds = AnnotationBounds(annotation);
    OffsetRect(&previousBounds, selection_.left, selection_.top);
    int deltaX = point.x - pointerStart_.x;
    int deltaY = point.y - pointerStart_.y;
    const int selectionWidth = RectWidth(selection_);
    const int selectionHeight = RectHeight(selection_);
    annotation = annotationAtDragStart_;
    const bool liveTextReflow =
        annotation.tool == Tool::Text || annotation.tool == Tool::Number;
    if (liveTextReflow) {
        // Reflow text against the space available at its current anchor.
        OffsetAnnotation(annotation, deltaX, deltaY);
        annotation.minimumTextCharacters = 3;
        AdaptTextAnnotationLayout(annotation);
        const RECT adjustedBounds = AnnotationBounds(annotation);
        const int correctionX = adjustedBounds.left < 0
            ? -adjustedBounds.left
            : (adjustedBounds.right > selectionWidth
                ? selectionWidth - adjustedBounds.right : 0);
        // Number-label height changes must not move the badge anchor.
        const int correctionY = annotation.tool == Tool::Number
            ? 0
            : (adjustedBounds.top < 0
                ? -adjustedBounds.top
                : (adjustedBounds.bottom > selectionHeight
                    ? selectionHeight - adjustedBounds.bottom : 0));
        OffsetAnnotation(annotation, correctionX, correctionY);
        if (correctionX != 0) {
            AdaptTextAnnotationLayout(annotation);
        }
    } else {
        const RECT bounds = AnnotationBounds(annotationAtDragStart_);
        const int minimumDeltaX = -bounds.left;
        const int maximumDeltaX = selectionWidth - bounds.right;
        const int minimumDeltaY = -bounds.top;
        const int maximumDeltaY = selectionHeight - bounds.bottom;
        deltaX = minimumDeltaX <= maximumDeltaX
            ? std::clamp(deltaX, minimumDeltaX, maximumDeltaX) : 0;
        deltaY = minimumDeltaY <= maximumDeltaY
            ? std::clamp(deltaY, minimumDeltaY, maximumDeltaY) : 0;
        OffsetAnnotation(annotation, deltaX, deltaY);
    }

    RECT currentBounds = AnnotationBounds(annotation);
    OffsetRect(&currentBounds, selection_.left, selection_.top);
    RECT dirty = previousBounds;
    UnionRect(&dirty, &dirty, &currentBounds);
    const int padding = std::max(Scale(6), annotation.size + Scale(2));
    InflateRect(&dirty, padding, padding);
    if (IntersectRect(&dirty, &dirty, &selection_)) {
        // Let Windows merge rapid pointer updates into one back-buffered
        // paint. Forcing and flushing every intermediate frame blocks input
        // on DWM and can expose partially advanced drag frames.
        RedrawWindow(window_, &dirty, nullptr,
            RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    }
}

void ScreenshotOverlay::UpdateMovingEditingAnnotation(POINT point) {
    if (!textEditor_ || editingAnnotation_.tool != Tool::Number) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (!annotationDragStarted_) {
        if (!HasExceededSystemDragThreshold(point)) {
            return;
        }
        annotationDragStarted_ = true;
    }

    const Annotation previousEditingAnnotation = editingAnnotation_;
    RECT previousEditorRect{};
    GetWindowRect(textEditor_, &previousEditorRect);
    MapWindowPoints(nullptr, window_,
        reinterpret_cast<POINT*>(&previousEditorRect), 2);
    editingAnnotation_ = annotationAtDragStart_;
    OffsetAnnotation(editingAnnotation_,
        point.x - pointerStart_.x, point.y - pointerStart_.y);
    editingAnnotation_.text = ReadWindowText(textEditor_);
    editingAnnotation_.minimumTextCharacters = 3;
    if (IsWindowVisible(textEditor_)) {
        ShowWindow(textEditor_, SW_HIDE);
    }
    // Reflow the hidden editor while the parent paints the drag preview.
    UpdateTextEditorLayout();
    RECT currentEditorRect{};
    GetWindowRect(textEditor_, &currentEditorRect);
    MapWindowPoints(nullptr, window_,
        reinterpret_cast<POINT*>(&currentEditorRect), 2);

    RECT previousBounds = AnnotationBounds(previousEditingAnnotation);
    RECT currentBounds = AnnotationBounds(editingAnnotation_);
    OffsetRect(&previousBounds, selection_.left, selection_.top);
    OffsetRect(&currentBounds, selection_.left, selection_.top);
    RECT dirty = previousBounds;
    UnionRect(&dirty, &dirty, &currentBounds);
    // Empty number labels are absent from AnnotationBounds, so include the
    // child editor rectangles explicitly.
    UnionRect(&dirty, &dirty, &previousEditorRect);
    UnionRect(&dirty, &dirty, &currentEditorRect);
    InflateRect(&dirty, Scale(3), Scale(3));
    if (IntersectRect(&dirty, &dirty, &selection_)) {
        RedrawWindow(window_, &dirty, nullptr,
            RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    }
}

void ScreenshotOverlay::UpdateMovingPinned(POINT point) {
    if (!annotationDragStarted_) {
        if (!HasExceededSystemDragThreshold(point)) {
            return;
        }
        annotationDragStarted_ = true;
        if (toolbarTooltip_ && IsWindow(toolbarTooltip_)) {
            SendMessageW(toolbarTooltip_, TTM_POP, 0, 0);
        }
        if (!PreparePinnedDragPreview()) {
            annotationDragStarted_ = false;
            return;
        }
        toolbarSuppressed_ = true;
        // Hiding via per-window alpha preserves the editor and mouse capture.
        if (!SetLayeredWindowAttributes(window_, 0, 0, LWA_ALPHA)) {
            toolbarSuppressed_ = !toolbarVisibleBeforePinnedDrag_;
            RestorePinnedDragPreview();
            annotationDragStarted_ = false;
            return;
        }
        DwmFlush();
    }

    const POINT offset{
        point.x - pointerStart_.x,
        point.y - pointerStart_.y};
    selection_ = selectionAtDragStart_;
    OffsetRect(&selection_, offset.x, offset.y);
    if (pinnedWindowToUpdate_ && IsWindow(pinnedWindowToUpdate_)) {
        MovePinnedWindowToContent(pinnedWindowToUpdate_,
            virtualLeft_ + selection_.left,
            virtualTop_ + selection_.top);
    }
}

void ScreenshotOverlay::UpdateMovingToolbar(POINT point) {
    if (!toolbarDragStarted_) {
        if (!HasExceededSystemDragThreshold(point)) {
            return;
        }
        toolbarDragStarted_ = true;
    }

    const RECT previousToolbar = toolbar_;
    const RECT previousSettingsPanel = settingsPanel_;
    const bool previousSettingsVisible = SettingsPanelVisible();
    const int width = RectWidth(toolbar_);
    const int height = RectHeight(toolbar_);
    toolbar_.left = point.x - toolbarDragOffset_.x;
    toolbar_.top = point.y - toolbarDragOffset_.y;
    toolbar_.right = toolbar_.left + width;
    toolbar_.bottom = toolbar_.top + height;
    ClampToolbarToClient();
    RECT dirty = previousToolbar;
    UnionRect(&dirty, &dirty, &toolbar_);
    if (previousSettingsVisible) {
        UnionRect(&dirty, &dirty, &previousSettingsPanel);
    }
    if (SettingsPanelVisible()) {
        UnionRect(&dirty, &dirty, &settingsPanel_);
    }
    InflateRect(&dirty, Scale(3), Scale(3));
    RedrawWindow(window_, &dirty, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
}

void ScreenshotOverlay::UpdatePendingSelection(POINT point) {
    if (!HasExceededSystemDragThreshold(point)) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    interaction_ = Interaction::Selecting;
    selectionCommitted_ = false;
    toolbarMoved_ = false;
    activeTool_ = Tool::None;
    annotations_.clear();
    selection_ = ClampRectToClient(NormalizeRect(pointerStart_, point));
    // Rebuild the one-time transition from automatic window preselection to
    // manual selection. Later Selecting updates redraw only the changed area.
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
}

void ScreenshotOverlay::UpdateSelectionResize(POINT point) {
    if (!selectionResizeStarted_) {
        if (!HasExceededSystemDragThreshold(point)) {
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        selectionResizeStarted_ = true;
    }

    const RECT previousSelection = selection_;
    const RECT previousToolbar = toolbar_;
    const RECT previousSettingsPanel = settingsPanel_;
    selection_ = ResizeSelection(point);
    const int annotationOffsetX = previousSelection.left - selection_.left;
    const int annotationOffsetY = previousSelection.top - selection_.top;
    if (annotationOffsetX != 0 || annotationOffsetY != 0) {
        for (Annotation& annotation : annotations_) {
            OffsetAnnotation(annotation, annotationOffsetX, annotationOffsetY);
        }
    }
    UpdateToolbarPosition(!toolbarMoved_);

    RECT dirty = previousSelection;
    UnionRect(&dirty, &dirty, &selection_);
    InflateRect(&dirty, Scale(10), Scale(30));
    UnionRect(&dirty, &dirty, &previousToolbar);
    UnionRect(&dirty, &dirty, &toolbar_);
    if (RectWidth(previousSettingsPanel) > 0 &&
        RectHeight(previousSettingsPanel) > 0) {
        UnionRect(&dirty, &dirty, &previousSettingsPanel);
    }
    if (SettingsPanelVisible()) {
        UnionRect(&dirty, &dirty, &settingsPanel_);
    }
    const RECT client = ClientBounds();
    IntersectRect(&dirty, &dirty, &client);
    RedrawWindow(window_, &dirty, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
}

void ScreenshotOverlay::UpdateDrawingAnnotation(POINT point) {
    point.x = std::clamp(point.x, selection_.left, selection_.right);
    point.y = std::clamp(point.y, selection_.top, selection_.bottom);
    workingAnnotation_.end = ToSelectionPoint(point);
    if (workingAnnotation_.tool == Tool::Pen ||
        workingAnnotation_.tool == Tool::Mosaic) {
        const POINT last = workingAnnotation_.points.back();
        if (last.x != workingAnnotation_.end.x ||
            last.y != workingAnnotation_.end.y) {
            workingAnnotation_.points.push_back(workingAnnotation_.end);
        }
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void ScreenshotOverlay::UpdatePointerAction(POINT point) {
    point.x = std::clamp<LONG>(point.x, 0, static_cast<LONG>(virtualWidth_));
    point.y = std::clamp<LONG>(point.y, 0, static_cast<LONG>(virtualHeight_));
    switch (interaction_) {
    case Interaction::PendingSelection:
        UpdatePendingSelection(point);
        return;
    case Interaction::Selecting: {
        const RECT previousSelection = selection_;
        selection_ = ClampRectToClient(NormalizeRect(pointerStart_, point));
        RedrawSelectionTransition(previousSelection, {});
        return;
    }
    case Interaction::Resizing:
        UpdateSelectionResize(point);
        return;
    case Interaction::MovingSelection: {
        const RECT previousSelection = selection_;
        const RECT previousToolbar = toolbar_;
        const RECT previousSettingsPanel = settingsPanel_;
        const POINT offset = ClampedSelectionDragOffset(point);
        selection_ = selectionAtDragStart_;
        OffsetRect(&selection_, offset.x, offset.y);
        toolbar_ = toolbarAtDragStart_;
        OffsetRect(&toolbar_, offset.x, offset.y);
        ClampToolbarToClient();
        RedrawSelectionTransition(previousSelection,
            std::array<RECT, 4>{
                previousToolbar, toolbar_, previousSettingsPanel,
                settingsPanel_});
        return;
    }
    case Interaction::Drawing:
        UpdateDrawingAnnotation(point);
        return;
    case Interaction::MovingAnnotation:
        UpdateMovingAnnotation(point);
        return;
    case Interaction::MovingEditingAnnotation:
        UpdateMovingEditingAnnotation(point);
        return;
    case Interaction::MovingPinned:
        UpdateMovingPinned(point);
        return;
    case Interaction::MovingToolbar:
        UpdateMovingToolbar(point);
        return;
    case Interaction::PressingToolbar:
        hoveredToolbarCommand_ = HitTestToolbar(point);
        break;
    case Interaction::None:
        break;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void ScreenshotOverlay::EndPointerAction(POINT point) {
    if (interaction_ == Interaction::None) {
        return;
    }
    const bool finishingToolbarMove =
        interaction_ == Interaction::MovingToolbar;
    const bool finishingPinnedMove =
        interaction_ == Interaction::MovingPinned;
    const bool finishingSelectionResize =
        interaction_ == Interaction::Resizing;
    UpdatePointerAction(point);
    ToolbarCommand commandToExecute = ToolbarCommand::None;
    Annotation annotationToEdit{};
    int annotationIndexToEdit = -1;
    bool startTextEditor = false;
    bool restoreEditingNumberEditor = false;
    if (interaction_ == Interaction::PendingSelection) {
        if (!selectionCommitted_ && HasUsableSelection()) {
            selectionCommitted_ = true;
            toolbarMoved_ = false;
            UpdateToolbarPosition(true);
        }
    } else if (interaction_ == Interaction::Selecting) {
        if (RectWidth(selection_) < Scale(kMinimumSelectionDip) ||
            RectHeight(selection_) < Scale(kMinimumSelectionDip)) {
            selection_ = selectionAtDragStart_;
        }
        selectionCommitted_ = HasUsableSelection();
        UpdateToolbarPosition(true);
    } else if (interaction_ == Interaction::Resizing &&
               selectionResizeStarted_) {
        if (!HasUsableSelection()) {
            selection_ = selectionAtDragStart_;
        }
        selectionCommitted_ = true;
        toolbarMoved_ = false;
        UpdateToolbarPosition(true);
    } else if (interaction_ == Interaction::Drawing) {
        const bool usableStroke =
            (workingAnnotation_.tool == Tool::Pen &&
             workingAnnotation_.points.size() >= 2) ||
            (workingAnnotation_.tool == Tool::Mosaic &&
             !workingAnnotation_.points.empty());
        const bool isTextTool = workingAnnotation_.tool == Tool::Text ||
            workingAnnotation_.tool == Tool::Number;
        const bool usableShape = !isTextTool &&
            workingAnnotation_.tool != Tool::Pen &&
            workingAnnotation_.tool != Tool::Mosaic &&
            (std::abs(workingAnnotation_.end.x - workingAnnotation_.start.x) >= 2 ||
             std::abs(workingAnnotation_.end.y - workingAnnotation_.start.y) >= 2);
        if (isTextTool) {
            annotationToEdit = std::move(workingAnnotation_);
            startTextEditor = true;
        } else if (usableStroke || usableShape) {
            annotations_.push_back(std::move(workingAnnotation_));
        }
        workingAnnotation_ = {};
    } else if (interaction_ == Interaction::MovingAnnotation) {
        const bool isClick = !annotationDragStarted_;
        if (isClick && movingAnnotationIndex_ >= 0 &&
            movingAnnotationIndex_ < static_cast<int>(annotations_.size()) &&
            annotations_[static_cast<std::size_t>(
                movingAnnotationIndex_)].tool == Tool::Number) {
            annotationToEdit = annotations_[static_cast<std::size_t>(
                movingAnnotationIndex_)];
            annotationIndexToEdit = movingAnnotationIndex_;
            startTextEditor = true;
        } else if (!isClick && movingAnnotationIndex_ >= 0 &&
            movingAnnotationIndex_ < static_cast<int>(annotations_.size())) {
            Annotation& moved =
                annotations_[static_cast<std::size_t>(movingAnnotationIndex_)];
            if (moved.tool == Tool::Text || moved.tool == Tool::Number) {
                // Keep the existing box unchanged during the drag for a
                // stable, cheap frame.  Reflow once at the final position.
                moved.minimumTextCharacters = 3;
                AdaptTextAnnotationLayout(moved);
                RECT bounds = AnnotationBounds(moved);
                const int correctionX = bounds.left < 0
                    ? -bounds.left
                    : (bounds.right > RectWidth(selection_)
                        ? RectWidth(selection_) - bounds.right : 0);
                const int correctionY = moved.tool == Tool::Number
                    ? 0
                    : (bounds.top < 0
                        ? -bounds.top
                        : (bounds.bottom > RectHeight(selection_)
                            ? RectHeight(selection_) - bounds.bottom : 0));
                OffsetAnnotation(moved, correctionX, correctionY);
                AdaptTextAnnotationLayout(moved);
            }
        }
    } else if (interaction_ == Interaction::MovingEditingAnnotation &&
               annotationDragStarted_) {
        // Publish the final native layout once before restoring the editor.
        editingAnnotation_.minimumTextCharacters = 3;
        UpdateTextEditorLayout();
        restoreEditingNumberEditor = true;
    } else if (interaction_ == Interaction::PressingToolbar) {
        if (HitTestToolbar(point) == pressedToolbarCommand_) {
            commandToExecute = pressedToolbarCommand_;
        }
    }
    interaction_ = Interaction::None;
    if (finishingSelectionResize) {
        // Resizing hides the toolbar, which intentionally clears the settings
        // panel rectangle. Recalculate it after the toolbar becomes visible
        // again so the panel is never painted from an empty (0, 0) origin.
        if (selectionResizeStarted_) {
            toolbarMoved_ = false;
            UpdateToolbarPosition(true);
        } else {
            UpdateToolbarPosition(!toolbarMoved_);
        }
    }
    if ((finishingToolbarMove || finishingPinnedMove) && editingPinned_) {
        FinishPinnedPointerAction(finishingPinnedMove);
    }
    toolbarDragStarted_ = false;
    annotationDragStarted_ = false;
    selectionResizeStarted_ = false;
    resizeHandle_ = ResizeHandle::None;
    pressedToolbarCommand_ = ToolbarCommand::None;
    movingAnnotationIndex_ = -1;
    annotationAtDragStart_ = {};
    if (GetCapture() == window_) {
        ReleaseCapture();
    }
    if (restoreEditingNumberEditor && textEditor_) {
        SetWindowPos(textEditor_, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetFocus(textEditor_);
        NormalizeNumberEditorViewport();
        UpdateTextImePosition();
        RedrawWindow(textEditor_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    InvalidateRect(window_, nullptr, FALSE);
    if (commandToExecute != ToolbarCommand::None) {
        ExecuteToolbarCommand(commandToExecute);
    }
    if (startTextEditor && window_) {
        BeginTextEditing(std::move(annotationToEdit), annotationIndexToEdit,
            point);
    }
}

void ScreenshotOverlay::UpdateCursor(POINT point) const {
    HCURSOR cursor = LoadCursorW(nullptr,
        editingPinned_ ? IDC_SIZEALL : IDC_CROSS);
    if (fontSizeMenuOpen_ && ContainsPoint(FontSizePopupRect(), point)) {
        cursor = LoadCursorW(nullptr, IDC_HAND);
    } else if (SettingsPanelVisible() && ContainsPoint(settingsPanel_, point)) {
        bool settingsInteractive = true;
        const bool fontTool = activeTool_ == Tool::Text ||
            activeTool_ == Tool::Number;
        if (fontTool) {
            const int localX = point.x - settingsPanel_.left;
            const int localY = point.y - settingsPanel_.top;
            const int paletteX = localX - Scale(104);
            const int paletteY = localY - Scale(4);
            const int cell = std::max(1, Scale(20));
            const bool paletteHit = paletteX >= 0 && paletteY >= 0 &&
                paletteX / cell < 8 && paletteY / cell < 2;
            settingsInteractive = ContainsPoint(FontSizeComboRect(), point) ||
                paletteHit;
        }
        cursor = LoadCursorW(nullptr,
            settingsInteractive ? IDC_HAND : IDC_ARROW);
    } else if (const ToolbarCommand command = HitTestToolbar(point);
               command != ToolbarCommand::None) {
        cursor = LoadCursorW(nullptr, !IsToolbarCommandEnabled(command)
            ? IDC_ARROW
            : (IsToolbarDragEdge(point) ? IDC_SIZEALL : IDC_HAND));
    } else if (interaction_ == Interaction::MovingEditingAnnotation ||
               HitTestEditingNumberBadge(point)) {
        cursor = LoadCursorW(nullptr, IDC_SIZEALL);
    } else {
        switch (HitTestResizeHandle(point)) {
        case ResizeHandle::Left:
        case ResizeHandle::Right: cursor = LoadCursorW(nullptr, IDC_SIZEWE); break;
        case ResizeHandle::Top:
        case ResizeHandle::Bottom: cursor = LoadCursorW(nullptr, IDC_SIZENS); break;
        case ResizeHandle::TopLeft:
        case ResizeHandle::BottomRight: cursor = LoadCursorW(nullptr, IDC_SIZENWSE); break;
        case ResizeHandle::TopRight:
        case ResizeHandle::BottomLeft: cursor = LoadCursorW(nullptr, IDC_SIZENESW); break;
        case ResizeHandle::None:
            if (HitTestAnnotation(point) >= 0) {
                cursor = LoadCursorW(nullptr, IDC_SIZEALL);
            } else if (activeTool_ != Tool::None && ContainsPoint(selection_, point)) {
                cursor = LoadCursorW(nullptr, IDC_CROSS);
            } else if (!editingPinned_ && selectionCommitted_ &&
                       ContainsPointInclusive(selection_, point)) {
                cursor = LoadCursorW(nullptr, IDC_SIZEALL);
            } else if (editingPinned_ && ContainsPointInclusive(selection_, point)) {
                cursor = LoadCursorW(nullptr, IDC_SIZEALL);
            }
            break;
        }
    }
    SetCursor(cursor);
}

void ScreenshotOverlay::ExecuteToolbarCommand(ToolbarCommand command) {
    if (!IsToolbarCommandEnabled(command)) return;
    if (textEditor_ && command != ToolbarCommand::None) {
        if (command == ToolbarCommand::Undo) {
            const bool emptyTextDraft = editingAnnotation_.tool == Tool::Text &&
                !HasMeaningfulText(ReadWindowText(textEditor_));
            CommitTextEditing(false);
            if (!emptyTextDraft) {
                return;
            }
        } else {
            CommitTextEditing(true);
        }
    }
    switch (command) {
    case ToolbarCommand::LongCapture:
    case ToolbarCommand::Ocr:
    case ToolbarCommand::Gif:
    case ToolbarCommand::Record: {
        if (!captureTools_) return;
        try {
            if (command == ToolbarCommand::Ocr) {
                HBITMAP bitmap = CreateOutputBitmap(true);
                if (!bitmap) throw std::bad_alloc();
                capture::Image image;
                try { image = capture::FromBitmap(bitmap); }
                catch (...) { DeleteObject(bitmap); throw; }
                DeleteObject(bitmap);
                capture::OcrRequest request; request.image=std::move(image); request.owner=window_;
                request.imageBounds=selection_; request.busyRegion=selection_;
                OffsetRect(&request.busyRegion,virtualLeft_,virtualTop_);
                if(!editingPinned_) request.background=capture::FromBitmap(desktopBitmap_);
                toolbarSuppressed_=true; InvalidateRect(window_,nullptr,FALSE); UpdateWindow(window_);
                try { captureTools_->Ocr(std::move(request)); }
                catch(...) { toolbarSuppressed_=false; InvalidateRect(window_,nullptr,FALSE); throw; }
                return;
            }
            RECT screenRegion = selection_;
            OffsetRect(&screenRegion, virtualLeft_, virtualTop_);
            std::wstring error;
            if (!capture::CheckCaptureRegion(screenRegion, error)) {
                MessageBoxW(window_, error.c_str(), L"PcTool", MB_OK | MB_ICONINFORMATION);
                return;
            }
            const HWND sourceWindow = window_;
            auto completion = [this, sourceWindow](bool success) {
                if (window_ != sourceWindow || !IsWindow(sourceWindow)) return;
                if (success) Cancel();
                else { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); }
            };
            if (command == ToolbarCommand::LongCapture) {
                auto document = std::make_shared<capture::ImageDocument>();
                document->annotations = annotations_;
                for(auto& annotation:document->annotations)OffsetAnnotation(annotation,-selection_.left,-selection_.top);
                document->dpi = dpi_;
                document->image=capture::FromBitmap(desktopBitmap_).Crop(selection_.left,selection_.top,selection_.right-selection_.left,selection_.bottom-selection_.top);
                ShowWindow(window_,SW_HIDE);DwmFlush();
                try {captureTools_->LongCapture(screenRegion,std::move(document),[this,sourceWindow](bool){if(window_==sourceWindow&&IsWindow(sourceWindow))Cancel();});}
                catch(...){ShowWindow(window_,SW_SHOW);throw;}
            } else {
                captureTools_->Record(screenRegion, [this, sourceWindow](bool) {
                    if (window_ == sourceWindow && IsWindow(sourceWindow)) Cancel();
                },command == ToolbarCommand::Gif);
            }
            ShowWindow(window_, SW_HIDE);
        } catch (...) {
            MessageBoxW(window_, capture::CurrentError(L"无法启动采集工具").c_str(), L"PcTool", MB_OK | MB_ICONERROR);
        }
        return;
    }
    case ToolbarCommand::Rectangle:
        activeTool_ = activeTool_ == Tool::Rectangle ? Tool::None : Tool::Rectangle;
        break;
    case ToolbarCommand::Ellipse:
        activeTool_ = activeTool_ == Tool::Ellipse ? Tool::None : Tool::Ellipse;
        break;
    case ToolbarCommand::Arrow:
        activeTool_ = activeTool_ == Tool::Arrow ? Tool::None : Tool::Arrow;
        break;
    case ToolbarCommand::Pen:
        activeTool_ = activeTool_ == Tool::Pen ? Tool::None : Tool::Pen;
        break;
    case ToolbarCommand::Mosaic:
        activeTool_ = activeTool_ == Tool::Mosaic ? Tool::None : Tool::Mosaic;
        break;
    case ToolbarCommand::Text:
        activeTool_ = activeTool_ == Tool::Text ? Tool::None : Tool::Text;
        break;
    case ToolbarCommand::Number:
        activeTool_ = activeTool_ == Tool::Number ? Tool::None : Tool::Number;
        break;
    case ToolbarCommand::Clear:
        annotations_.clear();
        movingAnnotationIndex_ = -1;
        break;
    case ToolbarCommand::Undo:
        if (!annotations_.empty()) {
            DeleteAnnotation(static_cast<int>(annotations_.size()) - 1);
        }
        break;
    case ToolbarCommand::Pin:
        if (editingPinned_) {
            CompletePinnedEditing(true);
        } else if (PinSelectionToDesktop()) {
            Cancel();
        } else {
            MessageBoxW(window_, L"无法将截图固定在桌面。", L"PcTool",
                MB_OK | MB_ICONWARNING);
        }
        return;
    case ToolbarCommand::Save:
        if (SaveSelectionToPng()) {
            if (editingPinned_) {
                CompletePinnedEditing(true);
            } else {
                Cancel();
            }
        }
        return;
    case ToolbarCommand::Cancel:
        if (editingPinned_) {
            ClosePinnedScreenshot();
        } else {
            Cancel();
        }
        return;
    case ToolbarCommand::Finish:
        Finish(true);
        return;
    case ToolbarCommand::None:
    case ToolbarCommand::Grip:
        return;
    }
    UpdateSettingsPanelPosition();
    UpdatePinnedEditingRegion();
    InvalidateRect(window_, nullptr, FALSE);
}

bool ScreenshotOverlay::HandleSettingsPanelClick(POINT point) {
    if (!SettingsPanelVisible()) {
        return false;
    }
    const bool fontTool = activeTool_ == Tool::Text ||
        activeTool_ == Tool::Number;
    const RECT fontCombo = FontSizeComboRect();
    if (fontTool && fontSizeMenuOpen_) {
        const RECT popup = FontSizePopupRect();
        if (ContainsPoint(popup, point)) {
            const int itemHeight = Scale(kFontSizePopupItemHeightDip);
            const int index = std::clamp(
                static_cast<int>(point.y - popup.top - 1) /
                    std::max(1, itemHeight),
                0, kMaximumFontSizePoints - kMinimumFontSizePoints);
            fontSizePoints_ = kMinimumFontSizePoints + index;
            CloseFontSizeMenu();
            return true;
        }
        if (!ContainsPoint(fontCombo, point)) {
            CloseFontSizeMenu();
            return true;
        }
    }
    if (!ContainsPoint(settingsPanel_, point)) {
        return false;
    }
    const int localX = point.x - settingsPanel_.left;
    if (fontTool && ContainsPoint(fontCombo, point)) {
        fontSizeMenuOpen_ = !fontSizeMenuOpen_;
        hoveredFontSizePoints_ = -1;
        UpdatePinnedEditingRegion();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    } else if (!fontTool && localX < Scale(104)) {
        const int index = std::clamp((localX - Scale(8)) / std::max(1, Scale(32)), 0, 2);
        if (activeTool_ == Tool::Mosaic) {
            mosaicSizeIndex_ = index;
        } else {
            const int hit=capture::MakeStylePanelLayout({settingsPanel_.left,settingsPanel_.top},dpi_,false,false).Hit(point,false);
            if(hit>=1 && hit<=3) activeSizeIndex_=hit-1;
        }
    } else if (activeTool_ == Tool::Mosaic) {
        const int trackLeft = Scale(166);
        const int trackRight = RectWidth(settingsPanel_) - Scale(14);
        if (localX >= trackLeft) {
            const int position = std::clamp(localX, trackLeft, trackRight) - trackLeft;
            mosaicBlurLevel_ = 1 + std::clamp(
                MulDiv(position, 4, std::max(1, trackRight - trackLeft)), 0, 4);
        }
    } else {
        const auto layout=capture::MakeStylePanelLayout({settingsPanel_.left,settingsPanel_.top},dpi_,fontTool,false);
        const int hit=layout.Hit(point,fontTool);
        if(hit>=100 && hit<116) activeColor_=kAnnotationColors[hit-100];
    }    InvalidateRect(window_, &settingsPanel_, FALSE);
    return true;
}

void ScreenshotOverlay::BeginTextEditing(
    Annotation annotation, int existingAnnotationIndex,
    POINT activationPoint) {
    if (!window_ || textEditor_) {
        return;
    }
    textEditorWheelRemainder_ = 0;

    if (existingAnnotationIndex < 0 ||
        existingAnnotationIndex >= static_cast<int>(annotations_.size()) ||
        annotations_[static_cast<std::size_t>(existingAnnotationIndex)].tool != annotation.tool) {
        existingAnnotationIndex = -1;
    }
    const bool preserveExistingNumberLayout =
        existingAnnotationIndex >= 0 && annotation.tool == Tool::Number &&
        annotation.textBoxSize.cx > 0 && annotation.textBoxSize.cy > 0;

    if (annotation.tool == Tool::Text) {
        RECT relativeRect = NormalizeRect(annotation.start, annotation.end);
        if (RectHeight(relativeRect) < annotation.size + Scale(8)) {
            const int editorHeight = annotation.size + Scale(12);
            const int clickY = annotation.start.y;
            relativeRect.top = clickY - editorHeight / 2;
            relativeRect.bottom = relativeRect.top + editorHeight;
        }
        annotation.start = POINT{relativeRect.left, relativeRect.top};
        annotation.end = POINT{relativeRect.right, relativeRect.bottom};
    }
    if (!preserveExistingNumberLayout) {
        AdaptTextAnnotationLayout(annotation);
    }

    RECT editorRect{};
    if (annotation.tool == Tool::Number) {
        editorRect = NumberEditorRect(annotation,
            selection_.left, selection_.top);
    } else {
        editorRect = RECT{
            selection_.left + annotation.start.x,
            selection_.top + annotation.start.y,
            selection_.left + annotation.end.x,
            selection_.top + annotation.end.y};
    }

    if (annotation.tool != Tool::Number) {
        const int desiredWidth = std::min(
            RectWidth(editorRect), RectWidth(selection_));
        const int desiredHeight = std::min(
            RectHeight(editorRect), RectHeight(selection_));
        editorRect.left = std::clamp<LONG>(editorRect.left,
            selection_.left,
            std::max(selection_.left, selection_.right - desiredWidth));
        editorRect.top = std::clamp<LONG>(editorRect.top,
            selection_.top,
            std::max(selection_.top, selection_.bottom - desiredHeight));
        editorRect.right = editorRect.left + desiredWidth;
        editorRect.bottom = editorRect.top + desiredHeight;
        annotation.start = POINT{
            editorRect.left - selection_.left,
            editorRect.top - selection_.top};
        annotation.end = POINT{
            editorRect.right - selection_.left,
            editorRect.bottom - selection_.top};
    }
    editingAnnotation_ = std::move(annotation);
    editingAnnotationIndex_ = existingAnnotationIndex;

    RECT editingDirty = AnnotationBounds(editingAnnotation_);
    OffsetRect(&editingDirty, selection_.left, selection_.top);
    InflateRect(&editingDirty, Scale(6), Scale(6));
    if (editingAnnotationIndex_ >= 0) {
        // Remove the committed copy before the child editor is shown. This
        // prevents a fast second click from exposing two text layers while
        // the editor is being initialized.
        RedrawWindow(window_, &editingDirty, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    const bool numberEditor = editingAnnotation_.tool == Tool::Number;
    if (numberEditor && !textEditorBackgroundBrush_) {
        textEditorBackgroundBrush_ =
            CreateSolidBrush(kNumberLabelBackgroundColor);
    }
    const DWORD editorExStyle = numberEditor
        ? 0 : WS_EX_CLIENTEDGE;
    const DWORD editorStyle = WS_CHILD | ES_LEFT |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;

    textEditor_ = CreateWindowExW(
        editorExStyle, L"EDIT", L"", editorStyle,
        editorRect.left, editorRect.top, RectWidth(editorRect), RectHeight(editorRect),
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTextEditorId)),
        instance_, nullptr);
    if (!textEditor_) {
        editingAnnotation_ = {};
        editingAnnotationIndex_ = -1;
        return;
    }
    if (numberEditor) {
        SendMessageW(textEditor_, EM_SETWORDBREAKPROC, 0,
            reinterpret_cast<LPARAM>(&CharacterWordBreakProc));
    }

    if (numberEditor) ApplyNumberEditorRegion();

    if (textEditorFont_) {
        DeleteObject(textEditorFont_);
    }
    const int editorFontSize = std::max(1, editingAnnotation_.size);
    textEditorFont_ = CreateFontW(-editorFontSize,
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kAnnotationTextQuality,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    SendMessageW(textEditor_, WM_SETFONT,
        reinterpret_cast<WPARAM>(textEditorFont_), TRUE);
    suppressTextEditorChange_ = true;
    SetWindowTextW(textEditor_, editingAnnotation_.text.c_str());
    suppressTextEditorChange_ = false;
    SetWindowLongPtrW(textEditor_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    originalTextEditorProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        textEditor_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TextEditorProc)));
    if (numberEditor) {
        // WM_SETFONT may rebuild the native EDIT's formatting rectangle.
        // Reapply the callout's text area after the final input font exists.
        ApplyNumberEditorRegion();
    }
    SetWindowPos(textEditor_, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    SetFocus(textEditor_);
    // Existing labels can carry geometry and scroll state from an earlier
    // available width. Normalize every editor from its current text so a
    // reopened one never starts with a blank oversized body.
    UpdateTextEditorLayout();

    int insertionIndex = GetWindowTextLengthW(textEditor_);
    POINT editorPoint = activationPoint;
    if (activationPoint.x >= 0 && activationPoint.y >= 0) {
        MapWindowPoints(window_, textEditor_, &editorPoint, 1);
        RECT editorClient{};
        GetClientRect(textEditor_, &editorClient);
        if (PtInRect(&editorClient, editorPoint)) {
            const LRESULT character = SendMessageW(textEditor_, EM_CHARFROMPOS,
                0, MAKELPARAM(editorPoint.x, editorPoint.y));
            insertionIndex = std::clamp<int>(LOWORD(character), 0,
                GetWindowTextLengthW(textEditor_));
        }
    }
    SendMessageW(textEditor_, EM_SETSEL, insertionIndex, insertionIndex);
    SendMessageW(textEditor_, EM_SCROLLCARET, 0, 0);
    NormalizeNumberEditorViewport();
    UpdateTextImePosition();
    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
    RedrawWindow(textEditor_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    RedrawWindow(window_, &editingDirty, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW |
        RDW_NOERASE | RDW_ALLCHILDREN);
}

void ScreenshotOverlay::NormalizeNumberEditorViewport() {
    if (!textEditor_ || editingAnnotation_.tool != Tool::Number) {
        return;
    }
    RECT client{};
    GetClientRect(textEditor_, &client);
    int lineHeight = std::max(1, editingAnnotation_.size);
    HDC context = GetDC(textEditor_);
    if (context) {
        HGDIOBJ previousFont = textEditorFont_
            ? SelectObject(context, textEditorFont_) : nullptr;
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(context, &metrics)) {
            lineHeight = std::max(1, static_cast<int>(metrics.tmHeight));
        }
        if (previousFont) SelectObject(context, previousFont);
        ReleaseDC(textEditor_, context);
    }
    const int lineCount = std::max<int>(1,
        static_cast<int>(SendMessageW(textEditor_, EM_GETLINECOUNT, 0, 0)));
    const bool allLinesFit = lineCount * lineHeight <=
        RectHeight(client) + Scale(2);
    if (allLinesFit) {
        const int firstVisibleLine = std::max<int>(0,
            static_cast<int>(SendMessageW(
                textEditor_, EM_GETFIRSTVISIBLELINE, 0, 0)));
        if (firstVisibleLine > 0) {
            SendMessageW(textEditor_, EM_LINESCROLL,
                0, -firstVisibleLine);
        }
    }
}

void ScreenshotOverlay::ReleaseNumberEditorCaretBitmap() {
    if (!numberEditorCaretBitmap_) {
        return;
    }
    DeleteObject(numberEditorCaretBitmap_);
    numberEditorCaretBitmap_ = nullptr;
}

void ScreenshotOverlay::InstallNumberEditorCaret() {
    if (!textEditor_ || editingAnnotation_.tool != Tool::Number ||
        GetFocus() != textEditor_) {
        return;
    }

    POINT caretPosition{};
    const bool hasCaretPosition = GetCaretPos(&caretPosition) != FALSE;
    GUITHREADINFO threadInfo{};
    threadInfo.cbSize = sizeof(threadInfo);
    const bool hasNativeCaret =
        GetGUIThreadInfo(GetCurrentThreadId(), &threadInfo) != FALSE &&
        threadInfo.hwndCaret == textEditor_;
    int caretWidth = hasNativeCaret
        ? std::max(1, RectWidth(threadInfo.rcCaret))
        : std::max(1, Scale(1));
    int caretHeight = hasNativeCaret
        ? std::max(1, RectHeight(threadInfo.rcCaret))
        : std::max(1, editingAnnotation_.size);

    DestroyCaret();
    ReleaseNumberEditorCaretBitmap();

    HDC editorContext = GetDC(textEditor_);
    HDC bitmapContext = editorContext
        ? CreateCompatibleDC(editorContext) : nullptr;
    HBITMAP caretBitmap = editorContext
        ? CreateCompatibleBitmap(editorContext, caretWidth, caretHeight)
        : nullptr;
    HGDIOBJ previousBitmap = nullptr;
    if (bitmapContext && caretBitmap) {
        previousBitmap = SelectObject(bitmapContext, caretBitmap);
        // Caret bitmaps are XORed onto the control. Pre-XOR the fixed gray
        // background so the visible caret becomes pure white.
        const COLORREF caretXorColor =
            kNumberLabelBackgroundColor ^ RGB(255, 255, 255);
        const RECT bitmapRect{0, 0, caretWidth, caretHeight};
        FillSolidRect(bitmapContext, bitmapRect, caretXorColor);
        SelectObject(bitmapContext, previousBitmap);
    }
    if (bitmapContext) {
        DeleteDC(bitmapContext);
    }
    if (editorContext) {
        ReleaseDC(textEditor_, editorContext);
    }

    bool created = caretBitmap &&
        CreateCaret(textEditor_, caretBitmap, 0, 0) != FALSE;
    if (created) {
        numberEditorCaretBitmap_ = caretBitmap;
    } else {
        if (caretBitmap) {
            DeleteObject(caretBitmap);
        }
        created = CreateCaret(textEditor_, nullptr,
            caretWidth, caretHeight) != FALSE;
    }
    if (!created) {
        return;
    }
    if (hasCaretPosition) {
        SetCaretPos(caretPosition.x, caretPosition.y);
    }
    ShowCaret(textEditor_);
}

void ScreenshotOverlay::UpdateTextImePosition() {
    if (!textEditor_ || !IsWindow(textEditor_) ||
        GetFocus() != textEditor_ || updatingTextImePosition_) {
        return;
    }
    updatingTextImePosition_ = true;

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(textEditor_, EM_GETSEL,
        reinterpret_cast<WPARAM>(&selectionStart),
        reinterpret_cast<LPARAM>(&selectionEnd));
    POINT caret{};
    int lineHeight = std::max(Scale(14), editingAnnotation_.size + Scale(6));

    if (editingAnnotation_.tool == Tool::Number) {
        RECT client{};
        GetClientRect(textEditor_, &client);
        const int width = RectWidth(client);
        const int height = RectHeight(client);
        const int pointerWidth = std::min(Scale(8), width / 3);
        const int horizontalPadding = Scale(8);
        const int verticalPadding = Scale(5);
        const bool onLeft = editingAnnotation_.numberTextOnLeft;
        const int textLeft = onLeft
            ? horizontalPadding
            : pointerWidth + horizontalPadding;
        const int textRight = onLeft
            ? std::max(textLeft + 1,
                width - pointerWidth - horizontalPadding)
            : std::max(textLeft + 1, width - horizontalPadding);
        const int lineIndex = std::max<int>(0,
            static_cast<int>(SendMessageW(
                textEditor_, EM_LINEFROMCHAR, selectionStart, 0)));
        const int firstVisibleLine = std::max<int>(0,
            static_cast<int>(SendMessageW(
                textEditor_, EM_GETFIRSTVISIBLELINE, 0, 0)));
        LRESULT lineStartResult = SendMessageW(
            textEditor_, EM_LINEINDEX, lineIndex, 0);
        if (lineStartResult < 0) lineStartResult = 0;
        const std::wstring value = ReadWindowText(textEditor_);
        const std::size_t caretCharacter = std::min<std::size_t>(
            selectionStart, value.size());
        const std::size_t lineStart = std::min<std::size_t>(
            static_cast<std::size_t>(lineStartResult), caretCharacter);
        SIZE prefixExtent{};
        HDC context = GetDC(textEditor_);
        if (context) {
            HGDIOBJ previousFont = textEditorFont_
                ? SelectObject(context, textEditorFont_)
                : nullptr;
            const int prefixLength = static_cast<int>(
                caretCharacter - lineStart);
            if (prefixLength > 0) {
                GetTextExtentPoint32W(context,
                    value.data() + lineStart, prefixLength, &prefixExtent);
            }
            TEXTMETRICW metrics{};
            if (GetTextMetricsW(context, &metrics)) {
                lineHeight = std::max(lineHeight,
                    static_cast<int>(metrics.tmHeight +
                        metrics.tmExternalLeading));
            }
            if (previousFont) SelectObject(context, previousFont);
            ReleaseDC(textEditor_, context);
        }
        const int fontSize = std::max(1, editingAnnotation_.size);
        lineHeight = std::max(lineHeight, fontSize + Scale(3));
        caret.x = std::clamp(textLeft + static_cast<int>(prefixExtent.cx),
            textLeft, std::max(textLeft, textRight - Scale(1)));
        caret.y = std::clamp(verticalPadding +
                (lineIndex - firstVisibleLine) * lineHeight,
            verticalPadding,
            std::max(verticalPadding, height - lineHeight));
    } else {
        const LRESULT position = SendMessageW(
            textEditor_, EM_POSFROMCHAR, selectionStart, 0);
        if (position != -1) {
            caret.x = static_cast<short>(LOWORD(position));
            caret.y = static_cast<short>(HIWORD(position));
        } else if (!GetCaretPos(&caret)) {
            caret = POINT{Scale(4), Scale(4)};
        }
    }

    HIMC inputContext = ImmGetContext(textEditor_);
    if (!inputContext) {
        updatingTextImePosition_ = false;
        return;
    }
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = caret;
    ImmSetCompositionWindow(inputContext, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = POINT{caret.x, caret.y + lineHeight};
    candidate.rcArea = RECT{
        caret.x, caret.y,
        caret.x + std::max(Scale(2), 1), caret.y + lineHeight};
    ImmSetCandidateWindow(inputContext, &candidate);
    ImmReleaseContext(textEditor_, inputContext);
    updatingTextImePosition_ = false;
}

void ScreenshotOverlay::ScrollTextEditor(int wheelDelta) {
    if (!textEditor_ || !IsWindow(textEditor_) || wheelDelta == 0) {
        return;
    }
    textEditorWheelRemainder_ += wheelDelta;
    const int wheelSteps = textEditorWheelRemainder_ / WHEEL_DELTA;
    textEditorWheelRemainder_ %= WHEEL_DELTA;
    if (wheelSteps == 0) {
        return;
    }
    UINT linesPerStep = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0,
        &linesPerStep, 0);
    if (linesPerStep == WHEEL_PAGESCROLL) {
        RECT client{};
        GetClientRect(textEditor_, &client);
        const int fontSize = std::max(1, editingAnnotation_.size);
        linesPerStep = static_cast<UINT>(std::max(1,
            RectHeight(client) / std::max(1, fontSize + Scale(4))));
    }
    const int scrollLines = -wheelSteps *
        static_cast<int>(std::max<UINT>(1, linesPerStep));
    SendMessageW(textEditor_, EM_LINESCROLL, 0, scrollLines);
    RedrawWindow(textEditor_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW);
    UpdateTextImePosition();
}

void ScreenshotOverlay::ApplyNumberEditorRegion() {
    if (!textEditor_ || editingAnnotation_.tool != Tool::Number) {
        return;
    }
    RECT client{};
    GetClientRect(textEditor_, &client);
    const int editorWidth = RectWidth(client);
    const int editorHeight = RectHeight(client);
    if (editorWidth <= 0 || editorHeight <= 0) {
        return;
    }
    SetWindowRgn(textEditor_, nullptr, FALSE);
    SendMessageW(textEditor_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    RECT formattingRect{0, 0, editorWidth, editorHeight};
    SendMessageW(textEditor_, EM_SETRECT, 0,
        reinterpret_cast<LPARAM>(&formattingRect));
}

int ScreenshotOverlay::TextEditorLineHeight() const {
    int height = std::max(1, editingAnnotation_.size) +
        (editingAnnotation_.tool == Tool::Number ? Scale(3) : Scale(6));
    HDC context = GetDC(textEditor_);
    if (!context) {
        return height;
    }
    HGDIOBJ previousFont = textEditorFont_
        ? SelectObject(context, textEditorFont_)
        : nullptr;
    TEXTMETRICW metrics{};
    if (GetTextMetricsW(context, &metrics)) {
        height = std::max(height,
            static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
    }
    if (previousFont) {
        SelectObject(context, previousFont);
    }
    ReleaseDC(textEditor_, context);
    return height;
}

void ScreenshotOverlay::RestoreTextEditorScroll(
    int lineHeight, bool preserveVisibleText,
    LRESULT preservedVisibleCharacter) {
    const int lineCount = std::max<int>(1,
        static_cast<int>(SendMessageW(textEditor_, EM_GETLINECOUNT, 0, 0)));
    RECT formattingRect{};
    SendMessageW(textEditor_, EM_GETRECT, 0,
        reinterpret_cast<LPARAM>(&formattingRect));
    const int visibleLineCapacity = std::max(1,
        RectHeight(formattingRect) / std::max(1, lineHeight));
    const int firstVisibleLine = std::max<int>(0,
        static_cast<int>(SendMessageW(
            textEditor_, EM_GETFIRSTVISIBLELINE, 0, 0)));
    if (lineCount <= visibleLineCapacity) {
        if (firstVisibleLine > 0) {
            SendMessageW(textEditor_, EM_LINESCROLL, 0, -firstVisibleLine);
        }
        return;
    }
    if (preserveVisibleText) {
        const int targetLine = std::max<int>(0,
            static_cast<int>(SendMessageW(textEditor_, EM_LINEFROMCHAR,
                preservedVisibleCharacter, 0)));
        if (targetLine != firstVisibleLine) {
            SendMessageW(textEditor_, EM_LINESCROLL,
                0, targetLine - firstVisibleLine);
        }
    } else {
        SendMessageW(textEditor_, EM_SCROLLCARET, 0, 0);
    }
}

void ScreenshotOverlay::UpdateNumberTextEditorLayout(
    Annotation layout, bool preserveVisibleText,
    LRESULT preservedVisibleCharacter) {
    const int numberDiameter = std::max(
        Scale(18), layout.size + Scale(8));
    int desiredWidth = layout.textBoxSize.cx;
    int desiredHeight = layout.textBoxSize.cy;
    int left = selection_.left + layout.start.x;
    int longestLogicalLineWidth = 0;
    HDC context = GetDC(textEditor_);
    if (context) {
        HGDIOBJ previousFont = textEditorFont_
            ? SelectObject(context, textEditorFont_)
            : nullptr;
        std::size_t lineStart = 0;
        while (lineStart <= layout.text.size()) {
            const std::size_t lineEnd = layout.text.find(L'\n', lineStart);
            std::size_t lineLength =
                (lineEnd == std::wstring::npos
                    ? layout.text.size() : lineEnd) - lineStart;
            if (lineLength > 0 &&
                layout.text[lineStart + lineLength - 1] == L'\r') {
                --lineLength;
            }
            SIZE extent{};
            if (lineLength > 0 && GetTextExtentPoint32W(context,
                layout.text.data() + lineStart,
                static_cast<int>(lineLength), &extent)) {
                longestLogicalLineWidth = std::max(
                    longestLogicalLineWidth, static_cast<int>(extent.cx));
            }
            if (lineEnd == std::wstring::npos) {
                break;
            }
            lineStart = lineEnd + 1;
        }
        if (previousFont) {
            SelectObject(context, previousFont);
        }
        ReleaseDC(textEditor_, context);
    }

    const int radius = (numberDiameter + 1) / 2;
    const int pointerGap = Scale(3);
    const int pointerWidth = Scale(8);
    const int horizontalPadding = Scale(16);
    const int rightContentEdge = std::max(
        1, RectWidth(selection_) - Scale(2));
    const int availableWidth = layout.numberTextOnLeft
        ? std::max(1, static_cast<int>(layout.start.x) -
            radius - pointerGap)
        : std::max(1, rightContentEdge -
            static_cast<int>(layout.start.x) - radius - pointerGap);
    const int naturalWidth = pointerWidth + horizontalPadding +
        longestLogicalLineWidth + Scale(2);
    desiredWidth = std::min(availableWidth,
        std::max(Scale(kDefaultTextEditorWidthDip), naturalWidth));
    layout.textBoxSize.cx = desiredWidth;
    const int pointerTip = left +
        (layout.numberTextOnLeft ? -1 : 1) *
            (numberDiameter / 2 + Scale(3));
    left = layout.numberTextOnLeft
        ? pointerTip - desiredWidth
        : pointerTip;
    int top = selection_.top + (layout.textBoxSize.cy > 0
        ? layout.end.y
        : layout.start.y - desiredHeight / 2);

    RECT previousEditorRect{};
    GetWindowRect(textEditor_, &previousEditorRect);
    MapWindowPoints(nullptr, window_,
        reinterpret_cast<POINT*>(&previousEditorRect), 2);
    const RECT previousCalloutRect = NumberCalloutRect(
        editingAnnotation_, selection_.left, selection_.top);
    bool redrawSuppressed = false;
    RECT appliedEditorRect = previousEditorRect;
    const auto applyEditorGeometry = [&](int editorLeft, int editorTop,
        int editorWidth, int editorHeight) {
        const RECT requested{editorLeft, editorTop,
            editorLeft + editorWidth, editorTop + editorHeight};
        if (EqualRect(&appliedEditorRect, &requested)) {
            return;
        }
        const bool sizeChanged =
            RectWidth(appliedEditorRect) != editorWidth ||
            RectHeight(appliedEditorRect) != editorHeight;
        if (!redrawSuppressed) {
            // EN_CHANGE runs inside EDIT; publish only the final geometry.
            SendMessageW(textEditor_, WM_SETREDRAW, FALSE, 0);
            redrawSuppressed = true;
        }
        SetWindowPos(textEditor_, HWND_TOP, editorLeft, editorTop,
            editorWidth, editorHeight, SWP_NOACTIVATE | SWP_NOREDRAW);
        if (sizeChanged) {
            ApplyNumberEditorRegion();
        }
        appliedEditorRect = requested;
    };

    editingAnnotation_.numberTextOnLeft = layout.numberTextOnLeft;
    RECT editorRect = NumberEditorRect(layout,
        selection_.left, selection_.top);
    applyEditorGeometry(editorRect.left, editorRect.top,
        RectWidth(editorRect), RectHeight(editorRect));

    const int lineHeight = TextEditorLineHeight();
    const int nativeLineCount = std::max<int>(1,
        static_cast<int>(SendMessageW(
            textEditor_, EM_GETLINECOUNT, 0, 0)));
    const int verticalPadding = Scale(5) * 2;
    const int selectionHeight = RectHeight(selection_);
    const int availableLineCount = std::max(0,
        (selectionHeight - verticalPadding) / lineHeight);
    const int visibleLineCount = std::min(
        nativeLineCount, availableLineCount);
    desiredHeight = std::min(selectionHeight,
        std::max(numberDiameter,
            verticalPadding + visibleLineCount * lineHeight));
    top = std::clamp<int>(
        selection_.top + layout.start.y - desiredHeight / 2,
        selection_.top,
        std::max<int>(selection_.top, selection_.bottom - desiredHeight));
    // The badge remains the only position anchor while the label grows.
    layout.end = POINT{left - selection_.left, top - selection_.top};
    layout.textBoxSize = SIZE{desiredWidth, desiredHeight};
    editorRect = NumberEditorRect(layout, selection_.left, selection_.top);
    applyEditorGeometry(editorRect.left, editorRect.top,
        RectWidth(editorRect), RectHeight(editorRect));

    // A width change can update EDIT's soft-wrap indexes asynchronously.
    std::vector<std::wstring> visualLines =
        ReadEditVisualLines(textEditor_, layout.text);
    const int finalLineCount = std::max<int>(1,
        static_cast<int>(visualLines.size()));
    const int finalVisibleLineCount = std::min(
        finalLineCount, availableLineCount);
    const int finalHeight = std::min(selectionHeight,
        std::max(numberDiameter,
            verticalPadding + finalVisibleLineCount * lineHeight));
    if (finalHeight != desiredHeight) {
        desiredHeight = finalHeight;
        top = std::clamp<int>(
            selection_.top + layout.start.y - desiredHeight / 2,
            selection_.top,
            std::max<int>(selection_.top,
                selection_.bottom - desiredHeight));
        layout.end = POINT{left - selection_.left, top - selection_.top};
        layout.textBoxSize = SIZE{desiredWidth, desiredHeight};
        editorRect = NumberEditorRect(layout,
            selection_.left, selection_.top);
        applyEditorGeometry(editorRect.left, editorRect.top,
            RectWidth(editorRect), RectHeight(editorRect));
    }
    editingAnnotation_ = std::move(layout);
    ApplyNumberEditorRegion();
    RestoreTextEditorScroll(lineHeight, preserveVisibleText,
        preservedVisibleCharacter);
    editingAnnotation_.renderedTextLines = std::move(visualLines);
    UpdateTextImePosition();

    const RECT currentEditorRect = editorRect;
    const RECT currentCalloutRect = NumberCalloutRect(
        editingAnnotation_, selection_.left, selection_.top);
    const bool geometryChanged =
        !EqualRect(&previousEditorRect, &currentEditorRect);
    if (redrawSuppressed) {
        SendMessageW(textEditor_, WM_SETREDRAW, TRUE, 0);
    }
    if (preserveVisibleText) {
        return;
    }
    if (geometryChanged) {
        RECT dirty = previousEditorRect;
        UnionRect(&dirty, &dirty, &currentEditorRect);
        UnionRect(&dirty, &dirty, &previousCalloutRect);
        UnionRect(&dirty, &dirty, &currentCalloutRect);
        InflateRect(&dirty, Scale(2), Scale(2));
        const RECT client = ClientBounds();
        IntersectRect(&dirty, &dirty, &client);
        RedrawWindow(window_, &dirty, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW |
            RDW_NOERASE | RDW_NOCHILDREN);
    }
    RedrawWindow(textEditor_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

void ScreenshotOverlay::UpdatePlainTextEditorLayout(
    Annotation layout, bool preserveVisibleText,
    LRESULT preservedVisibleCharacter) {
    const int desiredWidth = std::max(
        1, static_cast<int>(layout.end.x - layout.start.x));
    int desiredHeight = std::max(
        1, static_cast<int>(layout.end.y - layout.start.y));
    const int left = selection_.left + layout.start.x;
    int top = selection_.top + layout.start.y;
    top = std::clamp(top, static_cast<int>(selection_.top),
        std::max(static_cast<int>(selection_.top),
            static_cast<int>(selection_.bottom) - desiredHeight));

    RECT previousEditorRect{};
    GetWindowRect(textEditor_, &previousEditorRect);
    MapWindowPoints(nullptr, window_,
        reinterpret_cast<POINT*>(&previousEditorRect), 2);
    RECT appliedEditorRect = previousEditorRect;
    bool redrawSuppressed = false;
    const auto applyEditorGeometry = [&](int editorLeft, int editorTop,
        int editorWidth, int editorHeight) {
        const RECT requested{editorLeft, editorTop,
            editorLeft + editorWidth, editorTop + editorHeight};
        if (EqualRect(&appliedEditorRect, &requested)) {
            return;
        }
        if (!redrawSuppressed) {
            SendMessageW(textEditor_, WM_SETREDRAW, FALSE, 0);
            redrawSuppressed = true;
        }
        SetWindowPos(textEditor_, HWND_TOP, editorLeft, editorTop,
            editorWidth, editorHeight, SWP_NOACTIVATE | SWP_NOREDRAW);
        appliedEditorRect = requested;
    };

    const int lineHeight = TextEditorLineHeight();
    const int verticalPadding = Scale(8);
    const int selectionHeight = RectHeight(selection_);
    const int minimumCompleteHeight = lineHeight + verticalPadding;
    if (selectionHeight >= minimumCompleteHeight) {
        const int clampedStartY = std::clamp<int>(layout.start.y, 0,
            selectionHeight - minimumCompleteHeight);
        layout.start.y = clampedStartY;
        top = selection_.top + clampedStartY;
    }
    applyEditorGeometry(left, top, desiredWidth, desiredHeight);
    const int lineCount = std::max<int>(1,
        static_cast<int>(SendMessageW(textEditor_, EM_GETLINECOUNT, 0, 0)));
    const int availableHeight = std::max(0,
        static_cast<int>(selection_.bottom) - top);
    const int availableLineCount = std::max(0,
        (availableHeight - verticalPadding) / lineHeight);
    const int visibleLineCount = std::min(lineCount, availableLineCount);
    desiredHeight = std::min(availableHeight,
        verticalPadding + visibleLineCount * lineHeight);
    applyEditorGeometry(left, top, desiredWidth, desiredHeight);

    layout.start.y = top - selection_.top;
    layout.end.y = layout.start.y + desiredHeight;
    editingAnnotation_ = std::move(layout);
    RestoreTextEditorScroll(lineHeight, preserveVisibleText,
        preservedVisibleCharacter);
    UpdateTextImePosition();
    if (redrawSuppressed) {
        SendMessageW(textEditor_, WM_SETREDRAW, TRUE, 0);
    }
    const RECT currentEditorRect{
        left, top, left + desiredWidth, top + desiredHeight};
    if (!EqualRect(&previousEditorRect, &currentEditorRect)) {
        RECT dirty = previousEditorRect;
        UnionRect(&dirty, &dirty, &currentEditorRect);
        InflateRect(&dirty, Scale(2), Scale(2));
        const RECT client = ClientBounds();
        IntersectRect(&dirty, &dirty, &client);
        RedrawWindow(window_, &dirty, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW |
            RDW_NOERASE | RDW_ALLCHILDREN);
    } else {
        RedrawWindow(textEditor_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}

void ScreenshotOverlay::UpdateTextEditorLayout() {
    if (!textEditor_ || !IsWindow(textEditor_)) {
        return;
    }
    const bool preserveVisibleText =
        interaction_ == Interaction::MovingEditingAnnotation;
    LRESULT preservedVisibleCharacter = 0;
    if (preserveVisibleText) {
        const LRESULT firstVisibleLine = SendMessageW(
            textEditor_, EM_GETFIRSTVISIBLELINE, 0, 0);
        preservedVisibleCharacter = SendMessageW(textEditor_, EM_LINEINDEX,
            std::max<LRESULT>(0, firstVisibleLine), 0);
        if (preservedVisibleCharacter < 0) {
            preservedVisibleCharacter = 0;
        }
    }
    Annotation layout = editingAnnotation_;
    layout.text = ReadWindowText(textEditor_);
    AdaptTextAnnotationLayout(layout);

    if (layout.tool == Tool::Number) {
        UpdateNumberTextEditorLayout(std::move(layout), preserveVisibleText,
            preservedVisibleCharacter);
        return;
    }
    UpdatePlainTextEditorLayout(std::move(layout), preserveVisibleText,
        preservedVisibleCharacter);
}

void ScreenshotOverlay::CommitTextEditing(bool keepAnnotation) {
    if (!textEditor_) {
        return;
    }
    HWND editor = textEditor_;
    const std::wstring editorValue = ReadWindowText(editor);
    std::wstring value = TrimTextWhitespace(editorValue);
    const int existingAnnotationIndex = editingAnnotationIndex_;
    RECT annotationDirty = AnnotationBounds(editingAnnotation_);
    if (keepAnnotation && editingAnnotation_.tool == Tool::Number) {
        if (value.empty()) {
            editingAnnotation_.text.clear();
            editingAnnotation_.textBoxSize = {};
            editingAnnotation_.renderedTextLines.clear();
        } else {
            if (value != editorValue) {
                suppressTextEditorChange_ = true;
                SetWindowTextW(editor, value.c_str());
                suppressTextEditorChange_ = false;
                SendMessageW(editor, EM_SETSEL,
                    static_cast<WPARAM>(value.size()),
                    static_cast<LPARAM>(value.size()));
            }
            UpdateTextEditorLayout();
            editingAnnotation_.renderedTextLines =
                ReadEditVisualLines(editor, value);
        }
        const RECT updatedBounds = AnnotationBounds(editingAnnotation_);
        UnionRect(&annotationDirty, &annotationDirty, &updatedBounds);
    }

    RECT editorDirty{};
    GetWindowRect(editor, &editorDirty);
    MapWindowPoints(nullptr, window_,
        reinterpret_cast<POINT*>(&editorDirty), 2);
    OffsetRect(&annotationDirty, selection_.left, selection_.top);
    UnionRect(&editorDirty, &editorDirty, &annotationDirty);
    InflateRect(&editorDirty, Scale(3), Scale(3));

    textEditor_ = nullptr;
    editingAnnotationIndex_ = -1;
    textEditorWheelRemainder_ = 0;
    DestroyWindow(editor);
    if (keepAnnotation &&
        (editingAnnotation_.tool == Tool::Number || HasMeaningfulText(value))) {
        editingAnnotation_.text = std::move(value);
        if (existingAnnotationIndex >= 0 &&
            existingAnnotationIndex < static_cast<int>(annotations_.size())) {
            capture::ReplaceAnnotationGroup(annotations_,static_cast<std::size_t>(existingAnnotationIndex),editingAnnotation_);
            if(editingAnnotation_.fragmentGroup)editorDirty=ClientBounds();
        } else {
            if (editingAnnotation_.tool == Tool::Number) {
                editingAnnotation_.number = NextNumberAfterAnnotations();
            }
            annotations_.push_back(std::move(editingAnnotation_));
        }
    }
    editingAnnotation_ = {};
    if (window_) {
        SetFocus(window_);
        const RECT client = ClientBounds();
        IntersectRect(&editorDirty, &editorDirty, &client);
        RedrawWindow(window_, &editorDirty, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW |
            RDW_NOERASE | RDW_NOCHILDREN);
    }
}

void ScreenshotOverlay::UpdateToolbarPosition(bool resetPosition) {
    const auto buttons = ToolbarButtons();
    const int width = buttons.empty() ? 0 :
        buttons.back().rect.right - toolbar_.left;
    const int height = Scale(kToolbarHeightDip);
    const int placementHeight = std::max(
        1, height - Scale(kToolbarPlacementHeightToleranceDip));
    if (resetPosition || !toolbarMoved_) {
        const RECT client = ClientBounds();
        RECT available = client;
        RECT screenSelection = selection_;
        OffsetRect(&screenSelection, virtualLeft_, virtualTop_);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor = MonitorFromRect(
            &screenSelection, MONITOR_DEFAULTTONEAREST);
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
            available = OffsetRectValue(
                monitorInfo.rcMonitor, -virtualLeft_, -virtualTop_);
            IntersectRect(&available, &available, &client);
        }

        const int gap = Scale(kToolbarGapDip);
        const int maximumX = std::max(available.left, available.right - width);
        const int x = std::clamp<int>(
            selection_.right - width, available.left, maximumX);
        const int spaceBelow = available.bottom - selection_.bottom;
        const int spaceAbove = selection_.top - available.top;
        int y = selection_.bottom;
        if (spaceBelow >= placementHeight) {
            y = std::min<int>(selection_.bottom + gap,
                available.bottom - height);
        } else if (spaceAbove >= placementHeight) {
            y = std::max<int>(selection_.top - height - gap,
                available.top);
        } else {
            // A full-height pin has no external slot. Keep the toolbar at the
            // top of the visible image instead of clamping it near the bottom.
            y = std::max<int>(available.top, selection_.top) + gap;
            y = std::clamp<int>(y, available.top,
                std::max<int>(available.top, available.bottom - height));
        }
        toolbar_ = RECT{x, y, x + width, y + height};
    }
    ClampToolbarToClient();
}

void ScreenshotOverlay::ClampToolbarToClient() {
    const RECT client = ClientBounds();
    const int width = RectWidth(toolbar_);
    const int height = RectHeight(toolbar_);
    toolbar_.left = std::clamp(toolbar_.left, client.left,
        std::max(client.left, client.right - width));
    toolbar_.top = std::clamp(toolbar_.top, client.top,
        std::max(client.top, client.bottom - height));
    toolbar_.right = toolbar_.left + width;
    toolbar_.bottom = toolbar_.top + height;
    UpdateSettingsPanelPosition();
    UpdatePinnedEditingRegion();
    if (interaction_ != Interaction::MovingPinned) {
        UpdateToolbarTooltips();
    }
}

void ScreenshotOverlay::CreateToolbarTooltip() {
    if (!window_ || toolbarTooltip_) {
        return;
    }
    toolbarTooltip_ = capture::CreateToolbarTooltipWindow(window_, dpi_);
    if (!toolbarTooltip_) return;

    for (const ToolbarButton& button : ToolbarButtons()) {
        if (button.command == ToolbarCommand::Grip) {
            continue;
        }
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = static_cast<UINT_PTR>(button.command);
        tool.rect = button.rect;
        tool.lpszText = const_cast<wchar_t*>(button.text);
        SendMessageW(toolbarTooltip_, TTM_ADDTOOLW, 0,
            reinterpret_cast<LPARAM>(&tool));
    }
}

void ScreenshotOverlay::UpdateToolbarTooltips() {
    if (!toolbarTooltip_ || !IsWindow(toolbarTooltip_)) {
        return;
    }
    for (const ToolbarButton& button : ToolbarButtons()) {
        if (button.command == ToolbarCommand::Grip) {
            continue;
        }
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window_;
        tool.uId = static_cast<UINT_PTR>(button.command);
        tool.rect = button.rect;
        SendMessageW(toolbarTooltip_, TTM_NEWTOOLRECTW, 0,
            reinterpret_cast<LPARAM>(&tool));
    }
}

void ScreenshotOverlay::UpdateSettingsPanelPosition() {
    if (!SettingsPanelVisible()) {
        SetRectEmpty(&settingsPanel_);
        return;
    }
    const bool fontTool = activeTool_ == Tool::Text ||
        activeTool_ == Tool::Number;
    const int width = Scale(fontTool ? 272 : 280);
    const int height = Scale(48);
    int x = toolbar_.left;
    int y = toolbar_.bottom + Scale(4);
    if (y + height > virtualHeight_) {
        y = toolbar_.top - height - Scale(4);
    }
    x = std::clamp(x, 0, std::max(0, virtualWidth_ - width));
    y = std::clamp(y, 0, std::max(0, virtualHeight_ - height));
    settingsPanel_ = RECT{x, y, x + width, y + height};
}

void ScreenshotOverlay::UpdatePinnedEditingRegion() {
    if (!editingPinned_ || !window_ || !IsWindow(window_)) {
        return;
    }
    if (interaction_ == Interaction::MovingPinned) {
        return;
    }
    HRGN visibleRegion = CreateRectRgn(
        selection_.left, selection_.top,
        selection_.right, selection_.bottom);
    if (!visibleRegion) {
        return;
    }
    if (ToolbarVisible()) {
        HRGN toolbarRegion = CreateRectRgn(toolbar_.left, toolbar_.top,
            toolbar_.right, toolbar_.bottom);
        if (toolbarRegion) {
            CombineRgn(visibleRegion, visibleRegion, toolbarRegion, RGN_OR);
            DeleteObject(toolbarRegion);
        }
    }
    if (SettingsPanelVisible()) {
        HRGN settingsRegion = CreateRectRgn(settingsPanel_.left,
            settingsPanel_.top, settingsPanel_.right, settingsPanel_.bottom);
        if (settingsRegion) {
            CombineRgn(visibleRegion, visibleRegion, settingsRegion, RGN_OR);
            DeleteObject(settingsRegion);
        }
        if (fontSizeMenuOpen_) {
            const RECT popup = FontSizePopupRect();
            HRGN popupRegion = CreateRectRgn(
                popup.left, popup.top, popup.right, popup.bottom);
            if (popupRegion) {
                CombineRgn(visibleRegion, visibleRegion, popupRegion, RGN_OR);
                DeleteObject(popupRegion);
            }
        }
    }
    SetWindowRgn(window_, visibleRegion, TRUE);
}

void ScreenshotOverlay::CloseFontSizeMenu() {
    hoveredFontSizePoints_ = -1;
    fontSizeMenuOpen_ = false;
    UpdatePinnedEditingRegion();
    InvalidateRect(window_, nullptr, FALSE);
}

RECT ScreenshotOverlay::FontSizeComboRect() const noexcept {
    if(!SettingsPanelVisible()) return RECT{};
    return capture::MakeStylePanelLayout({settingsPanel_.left,settingsPanel_.top},dpi_,true,false).fontCombo;
}
RECT ScreenshotOverlay::FontSizePopupRect() const noexcept {
    const RECT combo = FontSizeComboRect();
    if (RectWidth(combo) <= 0 || RectHeight(combo) <= 0) {
        return RECT{};
    }

    const int height = Scale(kFontSizePopupItemHeightDip) *
        (kMaximumFontSizePoints - kMinimumFontSizePoints + 1) + 2;
    int top = combo.bottom + Scale(2);
    if (top + height > virtualHeight_) {
        top = combo.top - Scale(2) - height;
    }
    top = std::clamp(top, 0, std::max(0, virtualHeight_ - height));
    return RECT{combo.left, top, combo.right, top + height};
}

ScreenshotOverlay::ToolbarCommand ScreenshotOverlay::HitTestToolbar(POINT point) const {
    if (!ToolbarVisible() || !ContainsPoint(toolbar_, point)) {
        return ToolbarCommand::None;
    }
    for (const ToolbarButton& button : ToolbarButtons()) {
        if (ContainsPoint(button.rect, point)) {
            return button.command;
        }
    }
    return ToolbarCommand::Grip;
}

bool ScreenshotOverlay::IsToolbarCommandEnabled(
    ToolbarCommand command) const noexcept {

    if (command == ToolbarCommand::LongCapture || command == ToolbarCommand::Record || command == ToolbarCommand::Gif)
        return captureTools_ && !editingPinned_ && HasUsableSelection();
    if (command == ToolbarCommand::Ocr) return captureTools_ && HasUsableSelection();
    if (command == ToolbarCommand::Undo || command == ToolbarCommand::Clear) {
        return textEditor_ != nullptr || !annotations_.empty();
    }
    if (command == ToolbarCommand::Pin && editingPinned_) {
        return false;
    }
    return command != ToolbarCommand::None;
}

bool ScreenshotOverlay::IsToolbarDragEdge(POINT point) const {
    if (!ToolbarVisible() || !ContainsPoint(toolbar_, point)) {
        return false;
    }
    const int edge = Scale(4);
    return point.x < toolbar_.left + Scale(kToolbarGripWidthDip) ||
        point.x - toolbar_.left <= edge || toolbar_.right - point.x <= edge ||
        point.y - toolbar_.top <= edge || toolbar_.bottom - point.y <= edge;
}

bool ScreenshotOverlay::HasExceededSystemDragThreshold(
    POINT point) const noexcept {
    return std::abs(point.x - pointerStart_.x) >= GetSystemMetrics(SM_CXDRAG) ||
        std::abs(point.y - pointerStart_.y) >= GetSystemMetrics(SM_CYDRAG);
}

POINT ScreenshotOverlay::ClampedSelectionDragOffset(
    POINT point) const noexcept {
    return POINT{
        std::clamp<int>(point.x - pointerStart_.x,
            -static_cast<int>(selectionAtDragStart_.left),
            virtualWidth_ - static_cast<int>(selectionAtDragStart_.right)),
        std::clamp<int>(point.y - pointerStart_.y,
            -static_cast<int>(selectionAtDragStart_.top),
            virtualHeight_ - static_cast<int>(selectionAtDragStart_.bottom))};
}

ScreenshotOverlay::ResizeHandle ScreenshotOverlay::HitTestResizeHandle(POINT point) const {
    if (editingPinned_ || !selectionCommitted_ || !HasUsableSelection()) {
        return ResizeHandle::None;
    }
    const int hit = Scale(7);
    const bool left = std::abs(point.x - selection_.left) <= hit;
    const bool right = std::abs(point.x - selection_.right) <= hit;
    const bool top = std::abs(point.y - selection_.top) <= hit;
    const bool bottom = std::abs(point.y - selection_.bottom) <= hit;
    const bool withinX = point.x >= selection_.left - hit && point.x <= selection_.right + hit;
    const bool withinY = point.y >= selection_.top - hit && point.y <= selection_.bottom + hit;
    if (left && top) return ResizeHandle::TopLeft;
    if (right && top) return ResizeHandle::TopRight;
    if (left && bottom) return ResizeHandle::BottomLeft;
    if (right && bottom) return ResizeHandle::BottomRight;
    if (left && withinY) return ResizeHandle::Left;
    if (right && withinY) return ResizeHandle::Right;
    if (top && withinX) return ResizeHandle::Top;
    if (bottom && withinX) return ResizeHandle::Bottom;
    return ResizeHandle::None;
}

RECT ScreenshotOverlay::ResizeSelection(POINT point) const {
    RECT rect = selectionAtDragStart_;
    RECT protectedBounds{};
    bool hasProtectedBounds = false;
    for (const Annotation& annotation : annotations_) {
        if (annotation.tool == Tool::None) {
            continue;
        }
        RECT bounds = AnnotationBounds(annotation);
        int padding = Scale(2);
        if (annotation.tool == Tool::Pen ||
            annotation.tool == Tool::Mosaic ||
            annotation.tool == Tool::Rectangle ||
            annotation.tool == Tool::Ellipse) {
            padding = std::max(padding, (annotation.size + 1) / 2);
        }
        InflateRect(&bounds, padding, padding);
        OffsetRect(&bounds, selection_.left, selection_.top);
        if (hasProtectedBounds) {
            UnionRect(&protectedBounds, &protectedBounds, &bounds);
        } else {
            protectedBounds = bounds;
            hasProtectedBounds = true;
        }
    }

    const int minimumWidth = Scale(kMinimumSelectionDip);
    const int minimumHeight = Scale(kMinimumSelectionDip);
    const int maximumLeft = std::max(0, std::min<int>(
        rect.right - minimumWidth,
        hasProtectedBounds ? protectedBounds.left : rect.right));
    const int minimumRight = std::min(virtualWidth_, std::max<int>(
        rect.left + minimumWidth,
        hasProtectedBounds ? protectedBounds.right : rect.left));
    const int maximumTop = std::max(0, std::min<int>(
        rect.bottom - minimumHeight,
        hasProtectedBounds ? protectedBounds.top : rect.bottom));
    const int minimumBottom = std::min(virtualHeight_, std::max<int>(
        rect.top + minimumHeight,
        hasProtectedBounds ? protectedBounds.bottom : rect.top));

    switch (resizeHandle_) {
    case ResizeHandle::Left:
        rect.left = std::clamp<int>(point.x, 0, maximumLeft);
        break;
    case ResizeHandle::Top:
        rect.top = std::clamp<int>(point.y, 0, maximumTop);
        break;
    case ResizeHandle::Right:
        rect.right = std::clamp<int>(point.x, minimumRight, virtualWidth_);
        break;
    case ResizeHandle::Bottom:
        rect.bottom = std::clamp<int>(point.y, minimumBottom, virtualHeight_);
        break;
    case ResizeHandle::TopLeft:
        rect.left = std::clamp<int>(point.x, 0, maximumLeft);
        rect.top = std::clamp<int>(point.y, 0, maximumTop);
        break;
    case ResizeHandle::TopRight:
        rect.right = std::clamp<int>(point.x, minimumRight, virtualWidth_);
        rect.top = std::clamp<int>(point.y, 0, maximumTop);
        break;
    case ResizeHandle::BottomLeft:
        rect.left = std::clamp<int>(point.x, 0, maximumLeft);
        rect.bottom = std::clamp<int>(point.y, minimumBottom, virtualHeight_);
        break;
    case ResizeHandle::BottomRight:
        rect.right = std::clamp<int>(point.x, minimumRight, virtualWidth_);
        rect.bottom = std::clamp<int>(point.y, minimumBottom, virtualHeight_);
        break;
    case ResizeHandle::None: break;
    }
    return ClampRectToClient(rect);
}

int ScreenshotOverlay::HitTestAnnotation(
    POINT point, bool includeMosaic, bool visibleContentOnly) const {
    if (!selectionCommitted_ || !ContainsPointInclusive(selection_, point)) {
        return -1;
    }
    const POINT relative = ToSelectionPoint(point);
    for (int index = static_cast<int>(annotations_.size()) - 1;
         index >= 0; --index) {
        const Annotation& annotation =
            annotations_[static_cast<std::size_t>(index)];
        if(annotation.clip&&!PtInRect(&*annotation.clip,relative))continue;
        if (annotation.tool == Tool::None ||
            (annotation.tool == Tool::Mosaic && !includeMosaic)) {
            continue;
        }
        const double tolerance = std::max<double>(
            Scale(6), annotation.size / 2.0 + Scale(3));
        bool hit = false;
        switch (annotation.tool) {
        case Tool::Rectangle: {
            RECT rect = NormalizeRect(annotation.start, annotation.end);
            RECT outer = rect;
            InflateRect(&outer, static_cast<int>(std::ceil(tolerance)),
                static_cast<int>(std::ceil(tolerance)));
            if (ContainsPointInclusive(outer, relative)) {
                const double edgeDistance = std::min({
                    std::abs(static_cast<double>(relative.x - rect.left)),
                    std::abs(static_cast<double>(relative.x - rect.right)),
                    std::abs(static_cast<double>(relative.y - rect.top)),
                    std::abs(static_cast<double>(relative.y - rect.bottom))});
                hit = edgeDistance <= tolerance;
            }
            break;
        }
        case Tool::Ellipse: {
            const RECT rect = NormalizeRect(annotation.start, annotation.end);
            const double radiusX = RectWidth(rect) / 2.0;
            const double radiusY = RectHeight(rect) / 2.0;
            if (radiusX <= 0.0 || radiusY <= 0.0) {
                hit = DistanceToSegment(relative, annotation.start,
                    annotation.end) <= tolerance;
                break;
            }
            const double centerX = (rect.left + rect.right) / 2.0;
            const double centerY = (rect.top + rect.bottom) / 2.0;
            const double normalizedX = (relative.x - centerX) / radiusX;
            const double normalizedY = (relative.y - centerY) / radiusY;
            const double normalizedRadius = std::hypot(normalizedX, normalizedY);
            hit = std::abs(normalizedRadius - 1.0) *
                std::min(radiusX, radiusY) <= tolerance;
            break;
        }
        case Tool::Arrow: {
            hit = DistanceToSegment(relative, annotation.start,
                annotation.end) <= tolerance;
            if (!hit) {
                const double angle = std::atan2(
                    static_cast<double>(annotation.end.y - annotation.start.y),
                    static_cast<double>(annotation.end.x - annotation.start.x));
                const double wing = 0.55;
                const double length = static_cast<double>(
                    std::max(Scale(10), annotation.size * 3));
                const POINT first{
                    static_cast<LONG>(std::lround(annotation.end.x -
                        std::cos(angle - wing) * length)),
                    static_cast<LONG>(std::lround(annotation.end.y -
                        std::sin(angle - wing) * length))};
                const POINT second{
                    static_cast<LONG>(std::lround(annotation.end.x -
                        std::cos(angle + wing) * length)),
                    static_cast<LONG>(std::lround(annotation.end.y -
                        std::sin(angle + wing) * length))};
                hit = DistanceToSegment(relative, annotation.end, first) <= tolerance ||
                    DistanceToSegment(relative, annotation.end, second) <= tolerance;
            }
            break;
        }
        case Tool::Pen:
        case Tool::Mosaic:
            if (annotation.points.size() == 1) {
                hit = std::hypot(
                    static_cast<double>(relative.x - annotation.points.front().x),
                    static_cast<double>(relative.y - annotation.points.front().y)) <= tolerance;
            } else {
                for (std::size_t pointIndex = 1;
                     pointIndex < annotation.points.size(); ++pointIndex) {
                    if (DistanceToSegment(relative,
                            annotation.points[pointIndex - 1],
                            annotation.points[pointIndex]) <= tolerance) {
                        hit = true;
                        break;
                    }
                }
            }
            break;
        case Tool::Text: {
            // Fragments keep the original text layout; the clip test above
            // restricts hits without rewrapping to the fragment width.
            RECT rect = NormalizeRect(annotation.start,annotation.end);
            if (!visibleContentOnly) {
                InflateRect(&rect, Scale(3), Scale(3));
                hit = ContainsPointInclusive(rect, relative);
                break;
            }

            ScopedGdiTextMeasurement measurement(
                std::max(1, annotation.size));
            if (!measurement.IsAvailable()) {
                break;
            }
            TEXTMETRICW metrics{};
            const bool hasMetrics = GetTextMetricsW(
                measurement.Context(), &metrics) != FALSE;
            const int lineHeight = std::max(
                annotation.size + Scale(6),
                hasMetrics
                    ? static_cast<int>(metrics.tmHeight +
                        metrics.tmExternalLeading)
                    : 0);
            const auto lines = WrapTextToGdiPixelWidth(
                measurement.Context(), annotation.text,
                std::max(1, RectWidth(rect)));
            int lineTop = rect.top;
            for (const std::wstring& line : lines) {
                if (lineTop >= rect.bottom) {
                    break;
                }
                if (!line.empty()) {
                    SIZE extent{};
                    if (GetTextExtentPoint32W(measurement.Context(),
                            line.data(), static_cast<int>(line.size()),
                            &extent)) {
                        RECT visibleLine{
                            rect.left,
                            lineTop,
                            std::min<LONG>(rect.right,
                                rect.left + extent.cx),
                            std::min<LONG>(rect.bottom,
                                lineTop + lineHeight)};
                        InflateRect(&visibleLine, Scale(3), Scale(3));
                        if (ContainsPointInclusive(visibleLine, relative)) {
                            hit = true;
                            break;
                        }
                    }
                }
                lineTop += lineHeight;
            }
            break;
        }
        case Tool::Number: {
            const int diameter = std::max(
                Scale(18), annotation.size + Scale(8));
            const double dx = static_cast<double>(relative.x - annotation.start.x);
            const double dy = static_cast<double>(relative.y - annotation.start.y);
            hit = std::hypot(dx, dy) <= diameter / 2.0 + tolerance;
            if (!hit && annotation.textBoxSize.cx > 0) {
                hit = ContainsPointInclusive(AnnotationBounds(annotation), relative);
            }
            break;
        }
        case Tool::None:
            break;
        }
        if (hit) {
            return index;
        }
    }
    return -1;
}

bool ScreenshotOverlay::HitTestEditingNumberBadge(POINT point) const noexcept {
    if (!textEditor_ || editingAnnotation_.tool != Tool::Number ||
        !ContainsPointInclusive(selection_, point)) {
        return false;
    }
    const POINT relative = ToSelectionPoint(point);
    const int diameter = std::max(
        Scale(18), editingAnnotation_.size + Scale(8));
    const double deltaX =
        static_cast<double>(relative.x - editingAnnotation_.start.x);
    const double deltaY =
        static_cast<double>(relative.y - editingAnnotation_.start.y);
    return std::hypot(deltaX, deltaY) <= diameter / 2.0 + Scale(4);
}

RECT ScreenshotOverlay::NumberCalloutRect(
    const Annotation& annotation, int offsetX, int offsetY) const {
    const int diameter = std::max(
        Scale(18), annotation.size + Scale(8));
    const int width = std::max(1, static_cast<int>(annotation.textBoxSize.cx));
    const int height = std::max(
        diameter, static_cast<int>(annotation.textBoxSize.cy));
    const int pointerTip = annotation.start.x + offsetX +
        (annotation.numberTextOnLeft ? -1 : 1) *
            (diameter / 2 + Scale(3));
    const int left = annotation.numberTextOnLeft
        ? pointerTip - width : pointerTip;
    const int top = annotation.end.y + offsetY;
    return RECT{left, top, left + width, top + height};
}

RECT ScreenshotOverlay::NumberEditorRect(
    const Annotation& annotation, int offsetX, int offsetY) const {
    const RECT callout = NumberCalloutRect(annotation, offsetX, offsetY);
    const int pointerWidth = std::min(
        Scale(8), std::max(1, RectWidth(callout) / 3));
    const int horizontalPadding = Scale(8);
    const int verticalPadding = Scale(5);
    const int bodyLeft = annotation.numberTextOnLeft
        ? callout.left : callout.left + pointerWidth;
    const int bodyRight = annotation.numberTextOnLeft
        ? callout.right - pointerWidth : callout.right;
    RECT editor{
        bodyLeft + horizontalPadding,
        callout.top + verticalPadding,
        bodyRight - horizontalPadding,
        callout.bottom - verticalPadding};
    editor.right = std::max(editor.left + 1, editor.right);
    editor.bottom = std::max(editor.top + 1, editor.bottom);
    return editor;
}

RECT ScreenshotOverlay::AnnotationBounds(const Annotation& annotation) const {
    RECT bounds = NormalizeRect(annotation.start, annotation.end);
    switch (annotation.tool) {
    case Tool::Pen:
    case Tool::Mosaic:
        if (!annotation.points.empty()) {
            bounds = RECT{annotation.points.front().x, annotation.points.front().y,
                annotation.points.front().x, annotation.points.front().y};
            for (const POINT& point : annotation.points) {
                bounds.left = std::min(bounds.left, point.x);
                bounds.top = std::min(bounds.top, point.y);
                bounds.right = std::max(bounds.right, point.x);
                bounds.bottom = std::max(bounds.bottom, point.y);
            }
        }
        break;
    case Tool::Arrow: {
        const int wingLength = std::max(Scale(10), annotation.size * 3);
        InflateRect(&bounds, wingLength, wingLength);
        break;
    }
    case Tool::Number: {
        const int diameter = std::max(
            Scale(18), annotation.size + Scale(8));
        const int radius = (diameter + 1) / 2;
        bounds = RECT{annotation.start.x - radius, annotation.start.y - radius,
            annotation.start.x + radius, annotation.start.y + radius};
        if (annotation.textBoxSize.cx > 0) {
            const int fontSize = std::max(1, annotation.size);
            const int labelHeight = annotation.textBoxSize.cy > 0
                ? std::max(diameter,
                    static_cast<int>(annotation.textBoxSize.cy))
                : std::max(diameter, fontSize + Scale(10));
            const int pointerTip = annotation.start.x +
                (annotation.numberTextOnLeft ? -1 : 1) *
                    (diameter / 2 + Scale(3));
            const int estimatedTextWidth = std::max(Scale(10),
                static_cast<int>(annotation.text.size()) * fontSize);
            const int labelWidth = annotation.textBoxSize.cx > 0
                ? annotation.textBoxSize.cx
                : estimatedTextWidth + Scale(24);
            bounds.left = std::min<LONG>(bounds.left,
                annotation.numberTextOnLeft
                    ? pointerTip - labelWidth : pointerTip);
            bounds.top = std::min(bounds.top,
                annotation.textBoxSize.cy > 0
                    ? annotation.end.y
                    : annotation.start.y - labelHeight / 2);
            bounds.right = std::max<LONG>(bounds.right,
                annotation.numberTextOnLeft
                    ? pointerTip : pointerTip + labelWidth);
            bounds.bottom = std::max(bounds.bottom,
                (annotation.textBoxSize.cy > 0
                    ? annotation.end.y
                    : annotation.start.y - labelHeight / 2) + labelHeight);
        }
        break;
    }
    case Tool::Text: {
        bounds = NormalizeRect(annotation.start, annotation.end);
        break;
    }
    case Tool::Rectangle:
    case Tool::Ellipse:
    case Tool::None:
        break;
    }
    if(annotation.clip) {
        // Include stroke coverage before clipping so a fragment on a partition
        // boundary remains paintable and draggable.
        const int padding=std::max(1,(annotation.size+1)/2)+1;
        InflateRect(&bounds,padding,padding);
        RECT clipped{};IntersectRect(&clipped,&bounds,&*annotation.clip);return clipped;
    }
    return bounds;
}

void ScreenshotOverlay::AdaptTextAnnotationLayout(Annotation& annotation) const {
    capture::AnnotationPainter(dpi_).LayoutText(annotation, RectWidth(selection_), RectHeight(selection_));
}

void ScreenshotOverlay::OffsetAnnotation(
    Annotation& annotation, int deltaX, int deltaY) const {
    annotation.start.x += deltaX;
    annotation.start.y += deltaY;
    annotation.end.x += deltaX;
    annotation.end.y += deltaY;
    for (POINT& point : annotation.points) {
        point.x += deltaX;
        point.y += deltaY;
    }
    annotation.mosaicGridOrigin.x+=deltaX;annotation.mosaicGridOrigin.y+=deltaY;
    if(annotation.clip)OffsetRect(&*annotation.clip,deltaX,deltaY);
}

void ScreenshotOverlay::DeleteAnnotation(int index) {
    if (index < 0 || index >= static_cast<int>(annotations_.size())) {
        return;
    }
    if(annotations_[index].fragmentGroup){capture::EraseAnnotationGroup(annotations_,index);movingAnnotationIndex_=-1;return;}
    annotations_.erase(annotations_.begin() + index);
    if (movingAnnotationIndex_ == index) {
        movingAnnotationIndex_ = -1;
    } else if (movingAnnotationIndex_ > index) {
        --movingAnnotationIndex_;
    }
}

int ScreenshotOverlay::NextNumberAfterAnnotations() const noexcept {
    int nextNumber = 1;
    for (const Annotation& annotation : annotations_) {
        if (annotation.tool == Tool::Number) {
            nextNumber = std::max(nextNumber, annotation.number + 1);
        }
    }
    return nextNumber;
}

void ScreenshotOverlay::LoadPinnedAnnotations(HWND pinnedWindow) {
    annotations_.clear();
    const auto record = std::find_if(pinnedAnnotationRecords_.begin(),
        pinnedAnnotationRecords_.end(),
        [pinnedWindow](const PinnedAnnotationRecord& candidate) {
            return candidate.window == pinnedWindow;
        });
    if (record != pinnedAnnotationRecords_.end()) {
        annotations_ = record->annotations;
    }
}

void ScreenshotOverlay::StorePinnedAnnotations(HWND pinnedWindow) {
    auto record = std::find_if(pinnedAnnotationRecords_.begin(),
        pinnedAnnotationRecords_.end(),
        [pinnedWindow](const PinnedAnnotationRecord& candidate) {
            return candidate.window == pinnedWindow;
        });
    if (record == pinnedAnnotationRecords_.end()) {
        pinnedAnnotationRecords_.push_back(
            PinnedAnnotationRecord{pinnedWindow, annotations_});
    } else {
        record->annotations = annotations_;
    }
}

void ScreenshotOverlay::ForgetPinnedWindow(HWND pinnedWindow) {
    const bool closeActiveEditor =
        editingPinned_ && pinnedWindowToUpdate_ == pinnedWindow;
    pinnedAnnotationRecords_.erase(
        std::remove_if(pinnedAnnotationRecords_.begin(),
            pinnedAnnotationRecords_.end(),
            [pinnedWindow](const PinnedAnnotationRecord& record) {
                return record.window == pinnedWindow;
            }),
        pinnedAnnotationRecords_.end());
    hiddenPinnedWindows_.erase(
        std::remove(hiddenPinnedWindows_.begin(), hiddenPinnedWindows_.end(),
            pinnedWindow),
        hiddenPinnedWindows_.end());
    if (closeActiveEditor) {
        editingPinned_ = false;
        pinnedWindowToUpdate_ = nullptr;
        DestroyOverlayWindow();
    }
}

HBITMAP ScreenshotOverlay::CreateOutputBitmap(bool includeAnnotations) const {
    if (!HasUsableSelection() || !desktopBitmap_) {
        return nullptr;
    }
    HDC screen = GetDC(nullptr);
    HDC source = screen ? CreateCompatibleDC(screen) : nullptr;
    HDC output = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen
        ? CreateCompatibleBitmap(screen, RectWidth(selection_), RectHeight(selection_))
        : nullptr;
    if (!screen || !source || !output || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (source) DeleteDC(source);
        if (output) DeleteDC(output);
        if (screen) ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HBITMAP baseBitmap = desktopBitmap_;
    int sourceX = selection_.left;
    int sourceY = selection_.top;
    if (editingPinned_ && pinnedWindowToUpdate_ &&
        IsWindow(pinnedWindowToUpdate_)) {
        auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
            GetWindowLongPtrW(pinnedWindowToUpdate_, GWLP_USERDATA));
        if (pinnedData && pinnedData->bitmap) {
            baseBitmap = pinnedData->baseBitmap
                ? pinnedData->baseBitmap : pinnedData->bitmap;
            sourceX = 0;
            sourceY = 0;
        }
    }
    HGDIOBJ oldSource = SelectObject(source, baseBitmap);
    HGDIOBJ oldOutput = SelectObject(output, bitmap);
    BitBlt(output, 0, 0, RectWidth(selection_), RectHeight(selection_),
        source, sourceX, sourceY, SRCCOPY);
    if (includeAnnotations) {
        PaintAnnotations(output, 0, 0);
    }
    SelectObject(output, oldOutput);
    SelectObject(source, oldSource);
    DeleteDC(output);
    DeleteDC(source);
    ReleaseDC(nullptr, screen);
    return bitmap;
}

void ScreenshotOverlay::HidePinnedWindowsForCapture() {
    RestorePinnedWindowsAfterCapture();
    for (const PinnedAnnotationRecord& record : pinnedAnnotationRecords_) {
        if (record.window && IsWindow(record.window) &&
            IsWindowVisible(record.window)) {
            ShowWindow(record.window, SW_HIDE);
            hiddenPinnedWindows_.push_back(record.window);
        }
    }
    if (!hiddenPinnedWindows_.empty()) {
        DwmFlush();
    }
}

void ScreenshotOverlay::RestorePinnedWindowsAfterCapture() {
    for (HWND pinnedWindow : hiddenPinnedWindows_) {
        if (!pinnedWindow || !IsWindow(pinnedWindow)) {
            continue;
        }
        ShowWindow(pinnedWindow, SW_SHOWNOACTIVATE);
        SetWindowPos(pinnedWindow, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(pinnedWindow, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    hiddenPinnedWindows_.clear();
}

bool ScreenshotOverlay::RegisterPinnedWindowClass() const {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = PinnedScreenshotWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kPinnedScreenshotWindowClass;
    if (RegisterClassExW(&windowClass)) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ScreenshotOverlay::PinSelectionToDesktop() {
    HBITMAP baseBitmap = CreateOutputBitmap(false);
    HBITMAP bitmap = CreateOutputBitmap(true);
    if (!baseBitmap || !bitmap) {
        if (baseBitmap) DeleteObject(baseBitmap);
        if (bitmap) DeleteObject(bitmap);
        return false;
    }
    BITMAP bitmapInfo{};
    if (GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo) == 0 ||
        bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight <= 0 ||
        !RegisterPinnedWindowClass()) {
        DeleteObject(baseBitmap);
        DeleteObject(bitmap);
        return false;
    }

    const int screenX = virtualLeft_ + selection_.left;
    const int screenY = virtualTop_ + selection_.top;
    HWND pinnedWindow = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_APPWINDOW | WS_EX_TOPMOST,
        kPinnedScreenshotWindowClass, L"PcTool 钉图",
        WS_POPUP | WS_SYSMENU,
        screenX, screenY, 0, 0,
        nullptr, nullptr, instance_, nullptr);
    if (!pinnedWindow) {
        DeleteObject(baseBitmap);
        DeleteObject(bitmap);
        return false;
    }

    auto* data = new (std::nothrow) PinnedScreenshotData{};
    if (!data) {
        DestroyWindow(pinnedWindow);
        DeleteObject(baseBitmap);
        DeleteObject(bitmap);
        return false;
    }
    data->bitmap = bitmap;
    data->baseBitmap = baseBitmap;
    data->contentSize = SIZE{bitmapInfo.bmWidth, bitmapInfo.bmHeight};
    data->shadowExtent = std::max(6, MulDiv(9,
        static_cast<int>(dpi_ == 0 ? USER_DEFAULT_SCREEN_DPI : dpi_),
        USER_DEFAULT_SCREEN_DPI));
    data->owner = this;
    SetWindowLongPtrW(pinnedWindow, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(data));
    ConfigurePinnedWindowTaskbar(pinnedWindow, data);
    ConfigurePinnedWindowSystemMenu(pinnedWindow);
    if (!UpdatePinnedLayeredWindow(
            pinnedWindow, data, screenX, screenY)) {
        DestroyWindow(pinnedWindow);
        return false;
    }
    StorePinnedAnnotations(pinnedWindow);
    ShowWindow(pinnedWindow, SW_SHOWNOACTIVATE);
    SetWindowPos(pinnedWindow, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

bool ScreenshotOverlay::PreparePinnedDragPreview() {
    if (pinnedBitmapBeforeDrag_) {
        return true;
    }
    if (!pinnedWindowToUpdate_ || !IsWindow(pinnedWindowToUpdate_)) {
        return false;
    }
    auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
        GetWindowLongPtrW(pinnedWindowToUpdate_, GWLP_USERDATA));
    if (!pinnedData || !pinnedData->bitmap) {
        return false;
    }
    HBITMAP previewBitmap = CreateOutputBitmap();
    if (!previewBitmap) {
        return false;
    }
    pinnedBitmapBeforeDrag_ = pinnedData->bitmap;
    pinnedData->bitmap = previewBitmap;
    if (!RefreshPinnedLayeredWindow(pinnedWindowToUpdate_, pinnedData)) {
        pinnedData->bitmap = pinnedBitmapBeforeDrag_;
        pinnedBitmapBeforeDrag_ = nullptr;
        DeleteObject(previewBitmap);
        return false;
    }
    return true;
}

void ScreenshotOverlay::RestorePinnedDragPreview() {
    if (!pinnedBitmapBeforeDrag_) {
        return;
    }
    if (pinnedWindowToUpdate_ && IsWindow(pinnedWindowToUpdate_)) {
        auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
            GetWindowLongPtrW(pinnedWindowToUpdate_, GWLP_USERDATA));
        if (pinnedData) {
            HBITMAP previewBitmap = pinnedData->bitmap;
            pinnedData->bitmap = pinnedBitmapBeforeDrag_;
            pinnedBitmapBeforeDrag_ = nullptr;
            if (previewBitmap && previewBitmap != pinnedData->bitmap) {
                DeleteObject(previewBitmap);
            }
            RefreshPinnedLayeredWindow(pinnedWindowToUpdate_, pinnedData);
            return;
        }
    }
    DeleteObject(pinnedBitmapBeforeDrag_);
    pinnedBitmapBeforeDrag_ = nullptr;
}

void ScreenshotOverlay::FinishPinnedPointerAction(bool movedPinnedWindow) {
    if (movedPinnedWindow && pinnedWindowToUpdate_ &&
        IsWindow(pinnedWindowToUpdate_)) {
        MovePinnedWindowToContent(pinnedWindowToUpdate_,
            virtualLeft_ + selection_.left,
            virtualTop_ + selection_.top);
    }
    if (movedPinnedWindow) {
        toolbarSuppressed_ = false;
        toolbarMoved_ = false;
        // A click or completed drag on the pin always exposes its editing
        // controls at the standard position.
        UpdateToolbarPosition(true);
    }
    UpdatePinnedEditingRegion();
    UpdateToolbarTooltips();
    if (!movedPinnedWindow) {
        return;
    }
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
    // Present the fully prepared editor in one opacity change, then restore
    // the base pin underneath its opaque area.
    DwmFlush();
    RestorePinnedDragPreview();
}

void ScreenshotOverlay::CompletePinnedEditing(
    bool applyChanges, bool keepEditorWindow) {
    if (!editingPinned_) {
        return;
    }
    if (textEditor_) {
        CommitTextEditing(applyChanges);
    }
    RestorePinnedDragPreview();

    HWND pinnedWindow = pinnedWindowToUpdate_;
    HBITMAP updatedBitmap = applyChanges ? CreateOutputBitmap() : nullptr;
    if (updatedBitmap && pinnedWindow && IsWindow(pinnedWindow)) {
        auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
            GetWindowLongPtrW(pinnedWindow, GWLP_USERDATA));
        if (pinnedData) {
            HBITMAP oldBitmap = pinnedData->bitmap;
            pinnedData->bitmap = updatedBitmap;
            updatedBitmap = nullptr;
            if (oldBitmap) {
                DeleteObject(oldBitmap);
            }
            StorePinnedAnnotations(pinnedWindow);
            RefreshPinnedLayeredWindow(pinnedWindow, pinnedData);
        }
    }
    if (updatedBitmap) {
        DeleteObject(updatedBitmap);
    }

    editingPinned_ = false;
    pinnedWindowToUpdate_ = nullptr;
    if (pinnedWindow && IsWindow(pinnedWindow)) {
        ShowWindow(pinnedWindow, SW_SHOWNOACTIVATE);
        if (!keepEditorWindow) {
            SetWindowPos(pinnedWindow, HWND_TOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        auto* pinnedData = reinterpret_cast<PinnedScreenshotData*>(
            GetWindowLongPtrW(pinnedWindow, GWLP_USERDATA));
        if (pinnedData) {
            RefreshPinnedLayeredWindow(pinnedWindow, pinnedData);
        }
    }
    if (keepEditorWindow && window_ && IsWindow(window_)) {
        return;
    }
    DestroyOverlayWindow();
}

void ScreenshotOverlay::ClosePinnedScreenshot() {
    if (!editingPinned_) {
        return;
    }
    if (textEditor_) {
        CommitTextEditing(false);
    }
    RestorePinnedDragPreview();
    HWND pinnedWindow = pinnedWindowToUpdate_;
    editingPinned_ = false;
    pinnedWindowToUpdate_ = nullptr;
    DestroyOverlayWindow();
    if (pinnedWindow && IsWindow(pinnedWindow)) {
        DestroyWindow(pinnedWindow);
    }
}

void ScreenshotOverlay::DestroyOverlayWindow() {
    if (window_ && IsWindow(window_)) {
        DestroyWindow(window_);
        return;
    }
    window_ = nullptr;
    ReleaseResources();
}

bool ScreenshotOverlay::CopySelectionToClipboard() {
    HBITMAP bitmap=CreateOutputBitmap();if(!bitmap)return false;
    std::filesystem::path path;HGLOBAL files{};
    try{path=app_storage::Unique(L"Screenshots",L".png");
        if(!capture::SavePng(capture::FromBitmap(bitmap),path.wstring()))throw std::runtime_error("PNG encoding failed");
        files=app_storage::FileDrop(path);if(!files)throw std::runtime_error("Clipboard allocation failed");
    }catch(...){DeleteObject(bitmap);if(!path.empty())DeleteFileW(path.c_str());return false;}
    if(!OpenClipboard(window_)){GlobalFree(files);DeleteObject(bitmap);DeleteFileW(path.c_str());return false;}
    bool ok=EmptyClipboard()!=FALSE;
    if(ok){ok=SetClipboardData(CF_HDROP,files)!=nullptr;if(ok)files=nullptr;}
    if(ok){ok=SetClipboardData(CF_BITMAP,bitmap)!=nullptr;if(ok)bitmap=nullptr;}
    if(!ok)EmptyClipboard();
    CloseClipboard();if(files)GlobalFree(files);if(bitmap)DeleteObject(bitmap);
    if(!ok)DeleteFileW(path.c_str());return ok;
}
bool ScreenshotOverlay::SaveSelectionToPng() {
    HBITMAP bitmap = CreateOutputBitmap();
    if (!bitmap) {
        return false;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t fileName[MAX_PATH]{};
    swprintf_s(fileName, L"PcTool_%04u%02u%02u_%02u%02u%02u.png",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    bool saved = false;
    if (GetSaveFileNameW(&dialog)) {
        saved = SaveBitmapAsPng(bitmap, fileName);
        if (!saved) {
            MessageBoxW(window_, L"PNG 图片保存失败。", L"PcTool",
                MB_OK | MB_ICONWARNING);
        }
    }
    DeleteObject(bitmap);
    return saved;
}

bool ScreenshotOverlay::SaveBitmapAsPng(
    HBITMAP bitmap, const std::wstring& path) const {
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* source = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    bool succeeded = false;

    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) {
        result = factory->CreateBitmapFromHBITMAP(
            bitmap, nullptr, WICBitmapIgnoreAlpha, &source);
    }
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(result)) result = frame->Initialize(properties);

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) result = source->GetSize(&width, &height);
    if (SUCCEEDED(result)) result = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result)) result = frame->WriteSource(source, nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    succeeded = SUCCEEDED(result);

    if (properties) properties->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (source) source->Release();
    if (factory) factory->Release();
    return succeeded;
}

void ScreenshotOverlay::Finish(bool copyToClipboard) {
    if (editingPinned_) {
        CompletePinnedEditing(true);
        return;
    }
    if (!copyToClipboard || CopySelectionToClipboard()) {
        Cancel();
    } else {
        MessageBoxW(window_, L"复制截图到剪贴板失败。", L"PcTool",
            MB_OK | MB_ICONWARNING);
    }
}

RECT ScreenshotOverlay::NormalizeRect(POINT first, POINT second) const noexcept {
    return RECT{
        std::min(first.x, second.x), std::min(first.y, second.y),
        std::max(first.x, second.x), std::max(first.y, second.y)};
}

RECT ScreenshotOverlay::ClientBounds() const noexcept {
    return RECT{0, 0, virtualWidth_, virtualHeight_};
}

RECT ScreenshotOverlay::ClampRectToClient(RECT rect) const noexcept {
    const RECT client = ClientBounds();
    rect.left = std::clamp(rect.left, client.left, client.right);
    rect.right = std::clamp(rect.right, client.left, client.right);
    rect.top = std::clamp(rect.top, client.top, client.bottom);
    rect.bottom = std::clamp(rect.bottom, client.top, client.bottom);
    return rect;
}

bool ScreenshotOverlay::HasUsableSelection() const noexcept {
    return RectWidth(selection_) >= Scale(kMinimumSelectionDip) &&
        RectHeight(selection_) >= Scale(kMinimumSelectionDip);
}

bool ScreenshotOverlay::ToolbarVisible() const noexcept {
    return !toolbarSuppressed_ && selectionCommitted_ && HasUsableSelection() &&
        !(interaction_ == Interaction::MovingPinned &&
            annotationDragStarted_) &&
        !(interaction_ == Interaction::Resizing && selectionResizeStarted_);
}

bool ScreenshotOverlay::SettingsPanelVisible() const noexcept {
    return ToolbarVisible() && activeTool_ != Tool::None;
}

POINT ScreenshotOverlay::ToSelectionPoint(POINT point) const noexcept {
    return POINT{point.x - selection_.left, point.y - selection_.top};
}

int ScreenshotOverlay::CurrentToolSize() const noexcept {
    const int index = std::clamp(activeSizeIndex_, 0, 2);
    switch (activeTool_) {
    case Tool::Mosaic:
        return Scale(kMosaicSizesDip[static_cast<std::size_t>(
            std::clamp(mosaicSizeIndex_, 0, 2))]);
    case Tool::Text:
    case Tool::Number:
        return std::max(1, MulDiv(
            std::clamp(fontSizePoints_,
                kMinimumFontSizePoints, kMaximumFontSizePoints),
            static_cast<int>(dpi_), 72));
    case Tool::Rectangle:
    case Tool::Ellipse:
    case Tool::Arrow:
    case Tool::Pen:
        return Scale(kLineSizesDip[static_cast<std::size_t>(index)]);
    case Tool::None:
        return Scale(kLineSizesDip[1]);
    }
    return Scale(kLineSizesDip[1]);
}

int ScreenshotOverlay::Scale(int value) const noexcept {
    return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}







