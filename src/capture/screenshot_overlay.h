#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <vector>
#include "shared/image/image_document.h"

class CaptureTools;
namespace capture { class AnnotationRenderer; class ImageEditor; Image RenderDocument(const ImageDocument& document); }

class ScreenshotOverlay final {
public:
    explicit ScreenshotOverlay(HINSTANCE instance) noexcept;
    ~ScreenshotOverlay();

    ScreenshotOverlay(const ScreenshotOverlay&) = delete;
    ScreenshotOverlay& operator=(const ScreenshotOverlay&) = delete;

    bool Start();
    void SetCaptureTools(CaptureTools* tools) noexcept { captureTools_ = tools; }
    bool StartPinnedEditing(HWND pinnedWindow);
    [[nodiscard]] bool IsEditingPinnedWindow(
        HWND pinnedWindow) const noexcept;
    bool SwitchPinnedEditingForPointerDown(HWND pinnedWindow);
    void HidePinnedToolbar(HWND pinnedWindow);
    void SyncPinnedWindowPosition(HWND pinnedWindow,
        int contentScreenX, int contentScreenY, bool dragFinished);
    void ForgetPinnedWindow(HWND pinnedWindow);
    void Cancel();
    [[nodiscard]] bool IsActive() const noexcept { return window_ != nullptr; }
    bool FocusActive() { if(!window_)return false;SetForegroundWindow(window_);return true; }

private:
    friend struct ScreenshotInteractionTests;
    friend class capture::ImageEditor;
    friend capture::Image capture::RenderDocument(const capture::ImageDocument& document);
    using Tool = capture::Tool;
    using Annotation = capture::Annotation;

    enum class Interaction {
        None,
        PendingSelection,
        Selecting,
        Resizing,
        MovingSelection,
        Drawing,
        MovingAnnotation,
        MovingEditingAnnotation,
        MovingPinned,
        MovingToolbar,
        PressingToolbar,
    };

    enum class ResizeHandle {
        None,
        Left,
        Top,
        Right,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    enum class ToolbarCommand {
        None,
        Grip,
        Rectangle,
        Ellipse,
        Arrow,
        Pen,
        Mosaic,
        Text,
        Number,
        Undo,
        Clear,
        LongCapture,
        Ocr,
        Gif,
        Record,
        Pin,
        Save,
        Cancel,
        Finish,
    };


    struct PinnedAnnotationRecord {
        HWND window{};
        std::vector<Annotation> annotations;
    };

    struct ToolbarButton {
        ToolbarCommand command{ToolbarCommand::None};
        RECT rect{};
        const wchar_t* text{};
    };

    struct WindowSearchContext {
        ScreenshotOverlay* self{};
        POINT screenPoint{};
        HWND result{};
        RECT resultRect{};
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TextEditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void HandleActivation(WPARAM wParam, LPARAM lParam);
    void HandleLeftButtonDown(POINT point);
    void HandleMouseMove(POINT point);
    void HandleLeftButtonDoubleClick(POINT point);
    void HandleKeyDown(WPARAM key);
    void HandleCaptureChanged(HWND newCaptureWindow);

    bool RegisterWindowClass() const;
    bool CaptureDesktop();
    void ReleaseResources();
    void Paint();
    void PaintDimmedOutside(HDC deviceContext) const;
    void PaintSelection(HDC deviceContext) const;
    void PaintAnnotations(HDC deviceContext, int offsetX = 0, int offsetY = 0) const;
    void PaintAnnotation(
        HDC deviceContext, const Annotation& annotation,
        int offsetX, int offsetY,
        bool showEmptyNumberLabel = false) const;
    void PaintToolbar(HDC deviceContext) const;
    void PaintToolbarIcon(
        HDC deviceContext, ToolbarCommand command, const RECT& rect) const;
    void PaintSettingsPanel(HDC deviceContext) const;
    void PaintColorPicker(HDC deviceContext, HDC desktopContext) const;
    void PaintMosaicAnnotation(
        HDC deviceContext, const Annotation& annotation,
        int offsetX, int offsetY) const;

    void UpdateHoveredWindow(POINT clientPoint);
    [[nodiscard]] bool FindWindowAtScreenPoint(POINT screenPoint, RECT& screenRect) const;
    void BeginPointerAction(POINT point);
    void UpdatePointerAction(POINT point);
    void UpdatePendingSelection(POINT point);
    void UpdateSelectionResize(POINT point);
    void UpdateDrawingAnnotation(POINT point);
    void UpdateMovingAnnotation(POINT point);
    void UpdateMovingEditingAnnotation(POINT point);
    void UpdateMovingPinned(POINT point);
    void UpdateMovingToolbar(POINT point);
    void EndPointerAction(POINT point);
    void RedrawSelectionTransition(const RECT& previousSelection,
        const std::array<RECT, 4>& extraDirtyRects);
    void UpdateCursor(POINT point) const;
    void ExecuteToolbarCommand(ToolbarCommand command);
    bool HandleSettingsPanelClick(POINT point);
    void BeginTextEditing(Annotation annotation,
        int existingAnnotationIndex = -1,
        POINT activationPoint = POINT{-1, -1});
    void CommitTextEditing(bool keepAnnotation);
    void UpdateTextEditorLayout();
    void UpdateNumberTextEditorLayout(Annotation layout,
        bool preserveVisibleText, LRESULT preservedVisibleCharacter);
    void UpdatePlainTextEditorLayout(Annotation layout,
        bool preserveVisibleText, LRESULT preservedVisibleCharacter);
    [[nodiscard]] int TextEditorLineHeight() const;
    void RestoreTextEditorScroll(int lineHeight, bool preserveVisibleText,
        LRESULT preservedVisibleCharacter);
    void UpdateTextImePosition();
    void NormalizeNumberEditorViewport();
    void InstallNumberEditorCaret();
    void ReleaseNumberEditorCaretBitmap();
    void ScrollTextEditor(int wheelDelta);
    void ApplyNumberEditorRegion();
    [[nodiscard]] RECT NumberCalloutRect(const Annotation& annotation,
        int offsetX, int offsetY) const;
    [[nodiscard]] RECT NumberEditorRect(const Annotation& annotation,
        int offsetX, int offsetY) const;
    [[nodiscard]] COLORREF SampleDesktopColor(POINT clientPoint) const;
    bool CopyCurrentColorToClipboard() const;

    void UpdateToolbarPosition(bool resetPosition);
    void CreateToolbarTooltip();
    void UpdateToolbarTooltips();
    void UpdateSettingsPanelPosition();
    void UpdatePinnedEditingRegion();
    void CloseFontSizeMenu();
    [[nodiscard]] RECT FontSizeComboRect() const noexcept;
    [[nodiscard]] RECT FontSizePopupRect() const noexcept;
    void ClampToolbarToClient();
    [[nodiscard]] std::vector<ToolbarButton> ToolbarButtons() const;
    [[nodiscard]] ToolbarCommand HitTestToolbar(POINT point) const;
    [[nodiscard]] bool IsToolbarCommandEnabled(ToolbarCommand command) const noexcept;
    [[nodiscard]] bool IsToolbarDragEdge(POINT point) const;
    [[nodiscard]] bool HasExceededSystemDragThreshold(POINT point) const noexcept;
    [[nodiscard]] POINT ClampedSelectionDragOffset(POINT point) const noexcept;
    [[nodiscard]] ResizeHandle HitTestResizeHandle(POINT point) const;
    [[nodiscard]] RECT ResizeSelection(POINT point) const;
    [[nodiscard]] int HitTestAnnotation(
        POINT point, bool includeMosaic = false,
        bool visibleContentOnly = false) const;
    [[nodiscard]] bool HitTestEditingNumberBadge(POINT point) const noexcept;
    [[nodiscard]] RECT AnnotationBounds(const Annotation& annotation) const;
    void AdaptTextAnnotationLayout(Annotation& annotation) const;
    void OffsetAnnotation(Annotation& annotation, int deltaX, int deltaY) const;
    void DeleteAnnotation(int index);
    [[nodiscard]] int NextNumberAfterAnnotations() const noexcept;
    void LoadPinnedAnnotations(HWND pinnedWindow);
    void StorePinnedAnnotations(HWND pinnedWindow);

    [[nodiscard]] HBITMAP CreateOutputBitmap(
        bool includeAnnotations = true) const;
    bool CopySelectionToClipboard();
    bool PinSelectionToDesktop();
    bool RegisterPinnedWindowClass() const;
    void HidePinnedWindowsForCapture();
    void RestorePinnedWindowsAfterCapture();
    bool PreparePinnedDragPreview();
    void RestorePinnedDragPreview();
    void FinishPinnedPointerAction(bool movedPinnedWindow);
    void CompletePinnedEditing(
        bool applyChanges, bool keepEditorWindow = false);
    void ClosePinnedScreenshot();
    void DestroyOverlayWindow();
    bool SaveSelectionToPng();
    bool SaveBitmapAsPng(HBITMAP bitmap, const std::wstring& path) const;
    void Finish(bool copyToClipboard);

    [[nodiscard]] RECT NormalizeRect(POINT first, POINT second) const noexcept;
    [[nodiscard]] RECT ClientBounds() const noexcept;
    [[nodiscard]] RECT ClampRectToClient(RECT rect) const noexcept;
    [[nodiscard]] bool HasUsableSelection() const noexcept;
    [[nodiscard]] bool ToolbarVisible() const noexcept;
    [[nodiscard]] bool SettingsPanelVisible() const noexcept;
    [[nodiscard]] POINT ToSelectionPoint(POINT point) const noexcept;
    [[nodiscard]] int CurrentToolSize() const noexcept;
    [[nodiscard]] int Scale(int value) const noexcept;

    HINSTANCE instance_{};
    CaptureTools* captureTools_{}; // owned by the application controller
    HWND window_{};
    HWND toolbarTooltip_{};
    HBITMAP desktopBitmap_{};
    HBITMAP backBufferBitmap_{};
    HFONT toolbarFont_{};
    HFONT textEditorFont_{};
    ULONG_PTR gdiplusToken_{};
    int virtualLeft_{};
    int virtualTop_{};
    int virtualWidth_{};
    int virtualHeight_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};

    RECT selection_{};
    RECT selectionAtDragStart_{};
    RECT toolbar_{};
    RECT toolbarAtDragStart_{};
    RECT settingsPanel_{};
    POINT pointerStart_{};
    POINT toolbarDragOffset_{};
    Annotation annotationAtDragStart_{};
    bool selectionCommitted_{};
    bool toolbarMoved_{};
    bool toolbarDragStarted_{};
    bool annotationDragStarted_{};
    bool selectionResizeStarted_{};
    bool editingPinned_{};
    bool toolbarSuppressed_{};
    bool toolbarVisibleBeforePinnedDrag_{};
    ResizeHandle resizeHandle_{ResizeHandle::None};
    Tool activeTool_{Tool::None};
    Interaction interaction_{Interaction::None};
    ToolbarCommand hoveredToolbarCommand_{ToolbarCommand::None};
    ToolbarCommand pressedToolbarCommand_{ToolbarCommand::None};
    COLORREF activeColor_{RGB(255, 70, 70)};
    int activeSizeIndex_{0};
    int mosaicSizeIndex_{0};
    int fontSizePoints_{11};
    int hoveredFontSizePoints_{-1};
    bool fontSizeMenuOpen_{};
    int mosaicBlurLevel_{3};
    int movingAnnotationIndex_{-1};
    int editingAnnotationIndex_{-1};
    HWND pinnedWindowToUpdate_{};
    HBITMAP pinnedBitmapBeforeDrag_{};
    HWND textEditor_{};
    HBRUSH textEditorBackgroundBrush_{};
    WNDPROC originalTextEditorProc_{};
    HBITMAP numberEditorCaretBitmap_{};
    bool textEditorLayoutRefreshPending_{};
    bool suppressTextEditorChange_{};
    bool updatingTextImePosition_{};
    int textEditorWheelRemainder_{};
    bool colorPickerHotkeyRegistered_{};
    bool colorCopied_{};
    Annotation editingAnnotation_{};
    std::vector<Annotation> annotations_;
    Annotation workingAnnotation_{};
    std::vector<PinnedAnnotationRecord> pinnedAnnotationRecords_;
    std::vector<HWND> hiddenPinnedWindows_;
};
