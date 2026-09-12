#pragma once
#include "shared/image/image_document.h"
#include <memory>

namespace capture {
struct FrozenDesktop { Image image; RECT bounds{}; };
FrozenDesktop FreezeDesktop();
Image FromBitmap(HBITMAP bitmap);
HBITMAP ToBitmap(const Image& image);
bool CopyImage(HWND owner,const Image& image);
bool CopyImageFile(HWND owner,const Image& image);
bool CopyText(HWND owner,const std::wstring& text);
bool SavePng(const Image& image,const std::wstring& path);
std::wstring ChoosePath(HWND owner,bool video);
std::wstring ErrorMessage(const wchar_t* action,long code);
std::wstring CurrentError(const wchar_t* action); // call only inside a catch block
int64_t ClockNow();
bool CheckCaptureRegion(RECT region,std::wstring& error);
class DesktopCapture {
public:
    DesktopCapture(RECT region,bool cursor);
    ~DesktopCapture();
    bool Next(Image& image); // nonblocking; caller owns an MTA apartment
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// DXGI duplication does not create WGC's monitor-wide yellow indicator.
class BorderlessCapture {
public:
    BorderlessCapture(RECT region,bool cursor);
    ~BorderlessCapture();
    bool Next(Image& image);
    void DrawCursor(Image& image); // Composite the latest native shape onto a clean frame.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
