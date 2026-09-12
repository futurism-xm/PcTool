#pragma once
#include "shared/ui/capture_ui.h"
#include <array>

namespace capture {
inline constexpr int ToolbarHeightDip=36,ToolbarButtonDip=36;
// Screenshot and recording toolbars use the same point size at their window DPI.
inline constexpr int ToolbarFontPoints=10;
inline constexpr COLORREF ToolbarBackground=RGB(248,249,250),ToolbarBorder=RGB(216,220,225),ToolbarInk=RGB(38,46,57),ToolbarSelected=RGB(218,232,244);
inline constexpr COLORREF ToolbarDisabled=RGB(170,176,184),ToolbarDivider=RGB(222,225,229);
void DrawToolbarButtonState(HDC dc,RECT bounds,UINT dpi,bool enabled,bool selected,bool hovered,bool pressed,COLORREF accent=CLR_INVALID);
HWND CreateToolbarTooltipWindow(HWND owner,UINT dpi,bool excludeFromCapture=false);
HFONT CreateToolbarFont(UINT dpi);
void DrawToolbarFontCombo(HDC dc,RECT bounds,UINT dpi,int points);
inline constexpr std::array<int,3> AnnotationLineSizes{2,4,7};
inline constexpr int MinimumFontPoints=8,MaximumFontPoints=22;
inline constexpr std::array<COLORREF,16> AnnotationColors{
    RGB(255,70,70),RGB(255,140,55),RGB(255,205,30),RGB(113,195,55),
    RGB(38,166,91),RGB(39,145,230),RGB(77,93,225),RGB(145,68,220),
    RGB(235,55,175),RGB(25,25,25),RGB(92,92,92),RGB(150,150,150),
    RGB(255,255,255),RGB(0,188,212),RGB(121,85,72),RGB(255,64,129)};
struct AnnotationStyle { COLORREF color{AnnotationColors.front()}; int lineIndex{},fontPoints{11}; };
struct StylePanelLayout {
    RECT bounds{},fontCombo{},preview{};
    std::array<RECT,3> widths{};
    std::array<RECT,16> colors{};
    // 1..3 widths, 100..115 colors, 200 font dropdown.
    int Hit(POINT point,bool text) const;
};
StylePanelLayout MakeStylePanelLayout(POINT origin,UINT dpi,bool text,bool preview);
class AnnotationStylePanel final : public ToolWindow {
public:
    AnnotationStylePanel(HWND owner,std::function<void(AnnotationStyle)> changed,std::function<void()> escape);
    ~AnnotationStylePanel() override;
    void Update(AnnotationStyle style,bool text,UINT dpi,RECT toolbar,RECT monitor);
    void Hide();
    bool CloseFontDropdown();
private:
    class FontListWindow;
    std::unique_ptr<FontListWindow> fontWindow_;
    void PositionFontList();
    static LRESULT CALLBACK MouseHook(int,WPARAM,LPARAM);
    void WatchOutsideClicks();
    void Position();
    void Paint(HDC dc);
    int Hit(POINT point) const;
    LRESULT Handle(UINT,WPARAM,LPARAM) override;
    AnnotationStyle style_;
    bool text_{},fontOpen_{};
    int hover_{},pressed_{},fontFirst_{MinimumFontPoints},fontRows_{};
    RECT toolbar_{},monitor_{},fontList_{};
    StylePanelLayout layout_;
    HHOOK mouseHook_{};
    inline static thread_local AnnotationStylePanel* openPanel_{};
    std::function<void(AnnotationStyle)> changed_;
    std::function<void()> escape_;
};
}
