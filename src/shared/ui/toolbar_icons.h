#pragma once
#include <windows.h>
namespace capture {
inline constexpr float ToolbarIconSizeDip=15.0F;
// Callers own GDI+ lifetime and draw into a buffer at the window's physical DPI.
enum class ToolbarIcon { None, Grip, Rectangle, Ellipse, Arrow, Pen, Mosaic, Text, Number, Undo, LongCapture, Ocr, Gif, Record, Pin, Save, Cancel, Finish, System, SystemMuted, Mic, MicMuted, Laser, Clear, Pause, Resume, Style, Cursor, CursorHidden };
void DrawToolbarIcon(HDC dc,ToolbarIcon icon,const RECT& rect,UINT dpi,COLORREF color,float sizeDip=ToolbarIconSizeDip);
// Draw an embedded path fitted to a physical-pixel rectangle; caller owns GDI+.
bool DrawSvgIcon(HDC dc,const char* path,const RECT& rect,COLORREF color);
}
