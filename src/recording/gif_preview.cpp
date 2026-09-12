#include "recording/gif_recording.h"
#include "shared/annotation/annotation_style.h"
#include "shared/ui/toolbar_icons.h"
#include <objidl.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <winrt/base.h>
#include <filesystem>

namespace capture {
namespace {
class GifPreview final : public RecordingPreview {
    ULONG_PTR gdiplus_{};
    std::unique_ptr<Gdiplus::Image> image_;
    std::wstring path_; RecordingPreviewState state_;
    UINT frame_{},count_{}; std::vector<UINT> delays_; ULONGLONG next_{};
    void Schedule() { next_=GetTickCount64()+delays_[frame_]; SetTimer(window_,1,delays_[frame_],nullptr); }
    void Layout() {
        if(!window_) return; RECT r{}; GetClientRect(window_,&r);
        const int h=Dip(36),y=std::max(0L,r.bottom-Dip(48));
        MoveWindow(GetDlgItem(window_,PreviewPlay),Dip(12),y,Dip(72),h,TRUE);
        MoveWindow(GetDlgItem(window_,PreviewSave),std::max(Dip(96),int(r.right)-Dip(184)),y,Dip(80),h,TRUE);
        MoveWindow(GetDlgItem(window_,PreviewComplete),std::max(Dip(184),int(r.right)-Dip(96)),y,Dip(80),h,TRUE);
        InvalidateRect(window_,nullptr,FALSE);
    }
    void Paint(HDC target) {
        RECT r{}; GetClientRect(window_,&r); if(r.right<=0||r.bottom<=0)return;
        auto dc=CreateCompatibleDC(target); auto bitmap=CreateCompatibleBitmap(target,r.right,r.bottom); auto old=SelectObject(dc,bitmap);
        FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if(image_) {
            Gdiplus::Graphics g(dc); const float available=std::max(1,int(r.bottom)-Dip(60));
            const float scale=std::min(float(r.right)/image_->GetWidth(),available/image_->GetHeight());
            const float w=image_->GetWidth()*scale,h=image_->GetHeight()*scale;
            g.DrawImage(image_.get(),Gdiplus::RectF((r.right-w)/2,(available-h)/2,w,h));
        }
        BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    }
    void Command(int command) {
        if(command==PreviewPlay) { state_.playing=!state_.playing; if(state_.playing)Schedule();else KillTimer(window_,1); SetWindowTextW(GetDlgItem(window_,PreviewPlay),state_.playing?L"暂停":L"播放"); }
        if(command==PreviewComplete) {
            winrt::check_hresult(CopyRecordingFile(window_,path_));
            DestroyWindow(window_);
        }
        if(command==PreviewSave) {
            wchar_t path[32768]=L"录制.gif"; OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner=window_;
            dialog.lpstrFilter=L"GIF 动画 (*.gif)\0*.gif\0\0"; dialog.lpstrFile=path; dialog.nMaxFile=32768;
            dialog.lpstrDefExt=L"gif"; dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetSaveFileNameW(&dialog)) SaveTo(path);
        }
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l) override {
        if(m==WM_DRAWITEM) {
            const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(l); const auto dc=item->hDC;RECT r=item->rcItem;
            FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if(item->CtlID==PreviewComplete) {
                DrawToolbarButtonState(dc,r,dpi_,true,false,false,(item->itemState&ODS_SELECTED)!=0,RGB(45,159,240));
                auto font=CreateToolbarFont(dpi_);auto old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
                DrawTextW(dc,L"完成",-1,&r,DT_CENTER|DT_SINGLELINE|DT_VCENTER);SelectObject(dc,old);DeleteObject(font);
            } else DrawToolbarIcon(dc,item->CtlID==PreviewSave?ToolbarIcon::Save:state_.playing?ToolbarIcon::Pause:ToolbarIcon::Resume,r,dpi_,RGB(240,242,244));
            return TRUE;
        }
        if(m==WM_PAINT) { PAINTSTRUCT ps{}; auto dc=BeginPaint(window_,&ps); Paint(dc); EndPaint(window_,&ps); return 0; }
        if(m==WM_PRINTCLIENT) { Paint(reinterpret_cast<HDC>(w)); return 0; }
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_SIZE) { Layout(); return 0; }
        if(m==WM_GETMINMAXINFO) { auto info=reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize={Dip(340),Dip(180)}; return 0; }
        if(m==WM_TIMER && w==1 && state_.playing && GetTickCount64()>=next_) {
            frame_=(frame_+1)%count_; image_->SelectActiveFrame(&Gdiplus::FrameDimensionTime,frame_);
            state_.position=0; for(UINT i=0;i<frame_;++i)state_.position+=int64_t(delays_[i])*10000;
            Schedule(); InvalidateRect(window_,nullptr,FALSE); return 0;
        }
        if(m==WM_COMMAND) { Command(LOWORD(w)); return 0; }
        if(m==WM_DESTROY) { KillTimer(window_,1); state_.playing=state_.ready=false; image_.reset();delays_.clear();return 0; }
        if(m==WM_DPICHANGED) { auto result=ToolWindow::Handle(m,w,l);Layout();return result; }
        return ToolWindow::Handle(m,w,l);
    }
public:
    explicit GifPreview(RecordingResult result):path_(std::move(result.path)) {
        Gdiplus::GdiplusStartupInput input; if(Gdiplus::GdiplusStartup(&gdiplus_,&input,nullptr)!=Gdiplus::Ok)throw std::runtime_error("GDI+ startup failed");
        try {
        image_=std::make_unique<Gdiplus::Image>(path_.c_str());
        if(image_->GetLastStatus()!=Gdiplus::Ok)throw std::runtime_error("Unable to decode GIF");
        count_=image_->GetFrameCount(&Gdiplus::FrameDimensionTime); if(!count_)throw std::runtime_error("GIF has no frames");
        delays_.assign(count_,100);
        const auto bytes=image_->GetPropertyItemSize(PropertyTagFrameDelay); std::vector<BYTE> property(bytes);
        if(bytes && image_->GetPropertyItem(PropertyTagFrameDelay,bytes,reinterpret_cast<Gdiplus::PropertyItem*>(property.data()))==Gdiplus::Ok) {
            const auto* item=reinterpret_cast<Gdiplus::PropertyItem*>(property.data()); const auto* values=static_cast<UINT*>(item->value);
            for(UINT i=0;i<count_&&i<item->length/sizeof(UINT);++i)delays_[i]=std::max(10u,values[i]*10);
        }
        for(auto delay:delays_)state_.duration+=int64_t(delay)*10000;
        if(!Create(L"PcTool · GIF 预览",760,540,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,WS_EX_APPWINDOW))winrt::throw_last_error();
        Child(L"BUTTON",L"暂停",BS_OWNERDRAW|WS_TABSTOP,PreviewPlay); Child(L"BUTTON",L"下载",BS_OWNERDRAW|WS_TABSTOP,PreviewSave); Child(L"BUTTON",L"完成",BS_OWNERDRAW|WS_TABSTOP,PreviewComplete);
        POINT cursor{};GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi);
        const int width=std::min(Dip(760),int(mi.rcWork.right-mi.rcWork.left)),height=std::min(Dip(540),int(mi.rcWork.bottom-mi.rcWork.top));
        SetWindowPos(window_,nullptr,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-width)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-height)/2,width,height,SWP_NOZORDER);
        state_.ready=state_.playing=state_.controlsVisible=true; Layout();
        // Background/tray launches can carry STARTF_USESHOWWINDOW + SW_HIDE.
        // Match the MP4 preview: explicitly show independently of startup flags.
        winrt::check_bool(SetWindowPos(window_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW));
        SetForegroundWindow(window_); Schedule();
        if(!result.error.empty())MessageBoxW(window_,result.error.c_str(),L"GIF 录制提示",MB_OK|MB_ICONWARNING);
        } catch(...) { if(window_)DestroyWindow(window_);image_.reset();Gdiplus::GdiplusShutdown(gdiplus_);gdiplus_=0;throw; }
    }
    ~GifPreview() override { if(window_)DestroyWindow(window_);image_.reset();if(gdiplus_)Gdiplus::GdiplusShutdown(gdiplus_); }
    RecordingPreviewState State()const override{return state_;}
    void SaveTo(const std::wstring& path) override {
        if(std::filesystem::equivalent(std::filesystem::path(path_),std::filesystem::path(path),saveError_) && !saveError_)return;
        const auto temporary=path+L".pctool-"+std::to_wstring(GetTickCount64())+L".tmp";
        if(!CopyFileW(path_.c_str(),temporary.c_str(),TRUE))winrt::throw_last_error();
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { auto error=GetLastError();DeleteFileW(temporary.c_str());winrt::throw_hresult(HRESULT_FROM_WIN32(error)); }
    }
    std::error_code saveError_;
};
}
std::unique_ptr<RecordingPreview> OpenGifPreview(RecordingResult result) {return std::make_unique<GifPreview>(std::move(result));}
}
