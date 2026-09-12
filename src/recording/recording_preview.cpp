#include "recording/recording_preview.h"
#include "recording/gif_recording.h"
#include "shared/annotation/annotation_style.h"
#include "shared/annotation/annotation_renderer.h"
#include "shared/ui/toolbar_icons.h"
#include <mfplay.h>
#include <mfapi.h>
#include <mferror.h>
#include <evr.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winrt/base.h>
#include <gdiplus.h>
#include <filesystem>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>

namespace capture {
namespace {
constexpr UINT PlayerMessage=WM_APP+61,FocusMessage=WM_APP+62;
constexpr int VideoId=2110;
constexpr COLORREF Background=RGB(0,0,0),Ink=RGB(240,242,244),MutedInk=RGB(150,155,161);
std::atomic<UINT_PTR> nextGeneration{1};
void Fill(HDC dc,RECT rect,COLORREF color) { auto brush=CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush); }
std::wstring TimeText(int64_t time,bool hours) {
    const auto seconds=std::max<int64_t>(0,time)/10000000; wchar_t value[40]{};
    if(hours) swprintf_s(value,L"%02lld:%02lld:%02lld",seconds/3600,seconds/60%60,seconds%60);
    else swprintf_s(value,L"%02lld:%02lld",seconds/60,seconds%60);
    return value;
}
struct PlayerEvent { MFP_EVENT_TYPE type{}; HRESULT error{}; winrt::com_ptr<IMFPMediaItem> item; };
struct PlayerMailbox {
    std::mutex mutex; HWND window{}; UINT_PTR generation{}; std::deque<PlayerEvent> events;
};
struct PlayerCallback : winrt::implements<PlayerCallback,IMFPMediaPlayerCallback> {
    explicit PlayerCallback(std::shared_ptr<PlayerMailbox> mailbox):mailbox_(std::move(mailbox)) {}
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header) noexcept override {
        try {
            PlayerEvent event; event.type=header->eEventType; event.error=header->hrEvent;
            if(event.type==MFP_EVENT_TYPE_MEDIAITEM_CREATED && SUCCEEDED(event.error))
                event.item.copy_from(reinterpret_cast<MFP_MEDIAITEM_CREATED_EVENT*>(header)->pMediaItem);
            std::lock_guard<std::mutex> lock(mailbox_->mutex);
            if(!mailbox_->window) return;
            mailbox_->events.push_back(std::move(event));
            PostMessageW(mailbox_->window,PlayerMessage,mailbox_->generation,0);
        } catch(...) { /* Never unwind through the Media Foundation callback. */ }
    }
    std::shared_ptr<PlayerMailbox> mailbox_;
};

// Clipboard owns both allocations after SetClipboardData; no delayed rendering
// or process lifetime is required to paste the real file later.
HRESULT CopyVideoFile(HWND owner,const std::wstring& path) {
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    const size_t bytes=sizeof(DROPFILES)+(path.size()+2)*sizeof(wchar_t);
    HGLOBAL files=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes),effect=GlobalAlloc(GMEM_MOVEABLE,sizeof(DWORD));
    if(!files || !effect) { if(files) GlobalFree(files); if(effect) GlobalFree(effect); return E_OUTOFMEMORY; }
    auto* drop=static_cast<DROPFILES*>(GlobalLock(files)); auto* copy=static_cast<DWORD*>(GlobalLock(effect));
    if(!drop || !copy) { if(drop) GlobalUnlock(files); if(copy) GlobalUnlock(effect); GlobalFree(files); GlobalFree(effect); return E_OUTOFMEMORY; }
    drop->pFiles=sizeof(DROPFILES); drop->fWide=TRUE;
    memcpy(reinterpret_cast<BYTE*>(drop)+sizeof(DROPFILES),path.c_str(),(path.size()+1)*sizeof(wchar_t));
    *copy=DROPEFFECT_COPY; GlobalUnlock(files); GlobalUnlock(effect);
    const UINT format=RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if(!format || !OpenClipboard(owner)) { const auto hr=HRESULT_FROM_WIN32(GetLastError()); GlobalFree(files); GlobalFree(effect); return hr; }
    HRESULT hr=S_OK;
    if(!EmptyClipboard() || !SetClipboardData(CF_HDROP,files)) hr=HRESULT_FROM_WIN32(GetLastError());
    else files=nullptr;
    if(SUCCEEDED(hr) && SetClipboardData(format,effect)) effect=nullptr;
    CloseClipboard(); if(files) GlobalFree(files); if(effect) GlobalFree(effect); return hr;
}

class Preview final : public RecordingPreview {
public:
    explicit Preview(RecordingResult result):path_(std::move(result.path)),notice_(std::move(result.error)) {
        try {
            if(!Create(L"屏幕录制预览 · PcTool",900,620,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN)) winrt::throw_last_error();
            video_=Child(L"STATIC",L"",SS_BLACKRECT|WS_CLIPSIBLINGS,VideoId);
            SetWindowSubclass(video_,ChildProc,1,reinterpret_cast<DWORD_PTR>(this));
            for(int id:{PreviewPlay,PreviewSeek,PreviewTime,PreviewMute,PreviewSave,PreviewComplete}) {
                const bool label=id==PreviewTime,seek=id==PreviewSeek;
                HWND child=Child(label || seek?L"STATIC":L"BUTTON",id==PreviewComplete?L"完成":L"",
                    label?SS_OWNERDRAW:seek?SS_OWNERDRAW|SS_NOTIFY|WS_TABSTOP:BS_OWNERDRAW|WS_TABSTOP,id);
                SetWindowSubclass(child,ChildProc,1,reinterpret_cast<DWORD_PTR>(this));
            }
            state_.controlsVisible=true;
            tooltip_=CreateToolbarTooltipWindow(window_,dpi_);
            if(tooltip_) for(int id:{PreviewPlay,PreviewSeek,PreviewMute,PreviewSave,PreviewComplete}) {
                TOOLINFOW info{sizeof(info)}; info.hwnd=window_; info.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(window_,id));
                info.uFlags=TTF_IDISHWND|TTF_SUBCLASS; info.lpszText=LPSTR_TEXTCALLBACKW;
                SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
            }
            MONITORINFO monitor{sizeof(monitor)}; POINT cursor{}; GetCursorPos(&cursor);
            GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY),&monitor);
            const int width=std::min(Dip(1000),int(monitor.rcWork.right-monitor.rcWork.left)*9/10);
            const int height=std::min(Dip(680),int(monitor.rcWork.bottom-monitor.rcWork.top)*9/10);
            SetWindowPos(window_,nullptr,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,
                monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
            Layout(); SetTimer(window_,1,100,nullptr);
            // A background launch can carry STARTF_USESHOWWINDOW/SW_HIDE.
            // ShowWindow's first overlapped-window call honors that startup
            // value instead of SW_SHOW. This user-requested result must show.
            winrt::check_bool(SetWindowPos(window_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW));
            SetForegroundWindow(window_);
            StartPlayer();
        } catch(...) { ReleasePlayer(); if(tooltip_) DestroyWindow(tooltip_); if(window_) DestroyWindow(window_); throw; }
    }
    ~Preview() override {
        // Destruction is application shutdown, not the user's discard command.
        ReleasePlayer(); if(copyThread_.joinable()) copyThread_.join();
        if(tooltip_) DestroyWindow(tooltip_);
        if(window_) DestroyWindow(window_);
    }
    RecordingPreviewState State() const override { return state_; }
    void SaveTo(const std::wstring& target) override {
        if(target.empty() || state_.busy || !Window()) return;
        if(SameFile(target,path_)) { ReleasePlayer(); DestroyWindow(window_); return; }
        state_.busy=true; state_.error.clear(); ShowControls(true); EnableActions();
        try {
            const auto source=path_; saveTarget_=target; copyDone_=false; copyError_=ERROR_SUCCESS;
            const auto partial=target+L".pctool-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(generation_)+L".partial";
            copyThread_=std::thread([this,source,target,partial] {
                // Copy beside the destination and replace only after success;
                // failures leave an existing destination untouched.
                DWORD error=ERROR_SUCCESS;
                if(!CopyFileW(source.c_str(),partial.c_str(),TRUE)) {
                    error=GetLastError(); if(error!=ERROR_FILE_EXISTS && error!=ERROR_ALREADY_EXISTS) DeleteFileW(partial.c_str());
                }
                else if(!MoveFileExW(partial.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { error=GetLastError(); DeleteFileW(partial.c_str()); }
                copyError_=error; copyDone_=true;
            });
        } catch(...) { state_.busy=false; SetError(CurrentError(L"无法开始保存")); EnableActions(); }
    }
private:
    static bool SameFile(const std::wstring& a,const std::wstring& b) {
        std::error_code error;
        if(std::filesystem::equivalent(a,b,error)) return true;
        return _wcsicmp(std::filesystem::absolute(a).lexically_normal().c_str(),std::filesystem::absolute(b).lexically_normal().c_str())==0;
    }
    void StartPlayer() {
        const auto hr=MFStartup(MF_VERSION); if(FAILED(hr)) { SetError(ErrorMessage(L"视频预览不可用",hr)); return; }
        mf_=true; mailbox_=std::make_shared<PlayerMailbox>(); mailbox_->window=window_; mailbox_->generation=generation_;
        auto callback=winrt::make_self<PlayerCallback>(mailbox_);
        auto error=MFPCreateMediaPlayer(nullptr,FALSE,MFP_OPTION_FREE_THREADED_CALLBACK,callback.get(),video_,player_.put());
        if(SUCCEEDED(error)) {
            player_->SetBorderColor(Background); player_->SetAspectRatioMode(MFVideoARMode_PreservePicture);
            error=player_->CreateMediaItemFromURL(path_.c_str(),FALSE,0,nullptr);
        }
        if(FAILED(error)) SetError(ErrorMessage(L"无法播放视频",error));
        EnableActions();
    }
    void ReleasePlayer() {
        if(mailbox_) { std::lock_guard<std::mutex> lock(mailbox_->mutex); mailbox_->window=nullptr; mailbox_->events.clear(); }
        if(player_) { player_->Shutdown(); player_=nullptr; }
        mailbox_.reset(); if(mf_) { MFShutdown(); mf_=false; }
        state_.ready=state_.playing=false;
    }
    void Events() {
        if(!mailbox_) return; std::deque<PlayerEvent> events;
        { std::lock_guard<std::mutex> lock(mailbox_->mutex); events.swap(mailbox_->events); }
        for(auto& event:events) {
            if(FAILED(event.error)) { seekPending_=seekQueued_=playAfterSeek_=false; state_.playing=false; SetError(ErrorMessage(L"视频播放失败",event.error)); continue; }
            if(!player_) break;
            switch(event.type) {
            case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
                Check(player_->SetMediaItem(event.item.get())); break;
            case MFP_EVENT_TYPE_MEDIAITEM_SET: {
                PROPVARIANT duration{};
                if(SUCCEEDED(player_->GetDuration(MFP_POSITIONTYPE_100NS,&duration)))
                    state_.duration=duration.vt==VT_UI8?int64_t(duration.uhVal.QuadPart):duration.vt==VT_I8?duration.hVal.QuadPart:0;
                PropVariantClear(&duration); state_.position=0; Layout();
                // MFPlay cannot pause a stopped session. Prime it muted,
                // pause on PLAY, then seek back to zero before enabling UI.
                priming_=true; Check(player_->SetMute(TRUE)); Check(player_->Play()); break;
            }
            case MFP_EVENT_TYPE_PLAY:
                if(priming_) Check(player_->Pause()); else if(!ended_ && !seeking_ && !seekPending_) state_.playing=true;
                break;
            case MFP_EVENT_TYPE_PAUSE:
                state_.playing=false;
                if(priming_) { priming_=false; state_.ready=true; Check(player_->SetMute(state_.muted)); SeekTo(0,false); }
                break;
            case MFP_EVENT_TYPE_PLAYBACK_ENDED: if(seeking_ || seekPending_) break; state_.playing=false; state_.position=state_.duration; ended_=true; break;
            case MFP_EVENT_TYPE_POSITION_SET:
                seekPending_=false;
                if(seekQueued_) { seekQueued_=false; SubmitSeek(); }
                else if(playAfterSeek_ && !seeking_) { playAfterSeek_=false; Check(player_->Play()); }
                player_->UpdateVideo(); break;
            default: break;
            }
        }
        EnableActions(); RefreshControls();
    }
    void Check(HRESULT hr) { if(FAILED(hr)) SetError(ErrorMessage(L"预览操作失败",hr)); }
    void SetError(std::wstring message) { state_.error=std::move(message); InvalidateRect(video_,nullptr,FALSE); ShowControls(true); }
    bool DeleteOriginal() {
        if(DeleteFileW(path_.c_str())) return true;
        const auto error=GetLastError(); if(error==ERROR_FILE_NOT_FOUND) return true;
        SetError(ErrorMessage(L"清理录制文件失败",HRESULT_FROM_WIN32(error))+L"\n文件保留于："+path_); return false;
    }
    void SeekTo(int64_t position,bool play) {
        if(!player_ || !state_.ready) return;
        const bool end=state_.duration>0 && position>=state_.duration;
        // Queue all seeks from paused state. Calling Play on an already-playing
        // end seek can race its EOS notification and leave a stale playing UI.
        if(state_.playing) Check(player_->Pause());
        state_.playing=false; ended_=end; playAfterSeek_=play && !end;
        state_.position=std::clamp<int64_t>(position,0,state_.duration);
        seekTarget_=state_.position;
        if(seekPending_) seekQueued_=true;
        else SubmitSeek();
        RefreshControls();
    }
    void SubmitSeek() {
        const bool end=state_.duration>0 && seekTarget_>=state_.duration;
        // An endpoint seek holds the final 30 fps video frame, even when AAC
        // padding extends the container duration slightly beyond that frame.
        const auto position=end?std::max<int64_t>(0,state_.duration-333334):std::clamp<int64_t>(seekTarget_,0,std::max<int64_t>(0,state_.duration-1));
        PROPVARIANT value{}; value.vt=VT_I8; value.hVal.QuadPart=position;
        seekPending_=true;
        const auto hr=player_->SetPosition(MFP_POSITIONTYPE_100NS,&value);
        if(FAILED(hr)) { seekPending_=seekQueued_=playAfterSeek_=false; Check(hr); }
    }
    void TogglePlay() {
        if(!player_ || !state_.ready) return;
        if(seekPending_) { playAfterSeek_=!playAfterSeek_; return; }
        if(state_.playing) Check(player_->Pause());
        else if(ended_ || state_.position>=state_.duration) SeekTo(0,true);
        else Check(player_->Play());
    }
    void Command(int id) {
        if(state_.busy || dialog_) return;
        if(id==PreviewPlay) TogglePlay();
        else if(id==PreviewMute && player_ && state_.ready) { const auto hr=player_->SetMute(!state_.muted); if(SUCCEEDED(hr)) state_.muted=!state_.muted; else Check(hr); RefreshControls(); }
        else if(id==PreviewComplete) {
            const auto hr=CopyVideoFile(window_,path_);
            if(FAILED(hr)) { SetError(ErrorMessage(L"复制视频文件失败，请重试",hr)); return; }
            ReleasePlayer(); DestroyWindow(window_);
        } else if(id==PreviewSave) {
            dialog_=true; ShowControls(true); if(player_ && state_.playing) player_->Pause();
            auto target=ChoosePath(window_,true); dialog_=false; if(!target.empty()) SaveTo(target);
        }
    }
    void Tick() {
        if(copyDone_ && copyThread_.joinable()) {
            copyThread_.join(); copyDone_=false; state_.busy=false;
            if(copyError_!=ERROR_SUCCESS) { SetError(ErrorMessage(L"保存视频失败",HRESULT_FROM_WIN32(copyError_))+L"\n原视频保留于："+path_); EnableActions(); }
            else {
                ReleasePlayer();
                if(DeleteOriginal()) { DestroyWindow(window_); return; }
                // A successful destination must survive a later cancel/retry.
                notice_=L"视频已保存至："+saveTarget_; Layout(); InvalidateRect(window_,nullptr,FALSE); EnableActions();
            }
        }
        if(player_ && state_.ready && !seeking_ && !seekPending_ && !ended_) {
            PROPVARIANT position{};
            if(SUCCEEDED(player_->GetPosition(MFP_POSITIONTYPE_100NS,&position)))
                state_.position=position.vt==VT_UI8?int64_t(position.uhVal.QuadPart):position.vt==VT_I8?position.hVal.QuadPart:state_.position;
            PropVariantClear(&position);
        }
        POINT cursor{}; GetCursorPos(&cursor); RECT bounds{}; GetWindowRect(window_,&bounds);
        const bool inside=PtInRect(&bounds,cursor) && !IsIconic(window_);
        ShowControls(inside || seeking_ || dialog_ || state_.busy || (keyboard_ && IsChild(window_,GetFocus())) || !state_.error.empty());
        RefreshControls();
    }
    void EnableActions() {
        for(int id:{PreviewPlay,PreviewSeek,PreviewMute}) EnableWindow(GetDlgItem(window_,id),state_.ready && !state_.busy);
        for(int id:{PreviewSave,PreviewComplete}) EnableWindow(GetDlgItem(window_,id),!state_.busy);
        EnableMenuItem(GetSystemMenu(window_,FALSE),SC_CLOSE,MF_BYCOMMAND|(state_.busy?MF_GRAYED:MF_ENABLED));
    }
    void ShowControls(bool show) {
        if(state_.controlsVisible==show) return; state_.controlsVisible=show;
        for(int id:{PreviewPlay,PreviewSeek,PreviewTime,PreviewMute,PreviewSave,PreviewComplete}) ShowWindow(GetDlgItem(window_,id),show?SW_SHOWNOACTIVATE:SW_HIDE);
        if(!show && tooltip_) SendMessageW(tooltip_,TTM_POP,0,0);
        InvalidateRect(window_,nullptr,FALSE);
    }
    void RefreshControls() {
        for(int id:{PreviewPlay,PreviewSeek,PreviewTime,PreviewMute}) InvalidateRect(GetDlgItem(window_,id),nullptr,FALSE);
    }
    void Layout() {
        if(!video_) return; RECT client{}; GetClientRect(window_,&client);
        const int noticeHeight=notice_.empty()?0:Dip(42);
        const int h=std::max(0L,client.bottom-Dip(56)-noticeHeight); MoveWindow(video_,0,0,client.right,h,TRUE);
        const int gap=Dip(8),button=Dip(36),y=h+noticeHeight+Dip(10),right=client.right-gap;
        const int finishWidth=Dip(76),timeWidth=Dip(state_.duration>=36000000000LL?154:114);
        MoveWindow(GetDlgItem(window_,PreviewComplete),right-finishWidth,y,finishWidth,button,TRUE);
        MoveWindow(GetDlgItem(window_,PreviewSave),right-finishWidth-gap-button,y,button,button,TRUE);
        MoveWindow(GetDlgItem(window_,PreviewMute),right-finishWidth-gap*2-button*2,y,button,button,TRUE);
        const int timeX=right-finishWidth-gap*3-button*2-timeWidth;
        MoveWindow(GetDlgItem(window_,PreviewTime),timeX,y,timeWidth,button,TRUE);
        MoveWindow(GetDlgItem(window_,PreviewPlay),gap,y,button,button,TRUE);
        MoveWindow(GetDlgItem(window_,PreviewSeek),gap*2+button,y,std::max(Dip(40),timeX-gap*3-button),button,TRUE);
        if(player_) player_->UpdateVideo();
    }
    void DrawControl(const DRAWITEMSTRUCT& item) {
        const int width=item.rcItem.right,height=item.rcItem.bottom; if(width<=0 || height<=0) return;
        HDC dc=CreateCompatibleDC(item.hDC); HBITMAP bitmap=CreateCompatibleBitmap(item.hDC,width,height); auto old=SelectObject(dc,bitmap);
        RECT rect{0,0,width,height}; Fill(dc,rect,Background);
        const int id=int(item.CtlID); const bool enabled=IsWindowEnabled(item.hwndItem)!=FALSE;
        if(id==PreviewComplete || id==hover_ || (item.itemState&ODS_SELECTED)) {
            const COLORREF fill=id==PreviewComplete?RGB(34,171,91):RGB(45,49,54);
            DrawToolbarButtonState(dc,rect,dpi_,enabled,false,id==hover_,(item.itemState&ODS_SELECTED)!=0,fill);
        }
        HFONT font=CreateToolbarFont(dpi_); auto oldFont=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,enabled?Ink:MutedInk);
        if(id==PreviewTime || id==PreviewComplete) {
            const auto text=id==PreviewComplete?(state_.busy?L"保存中":L"完成"):
                TimeText(state_.position,state_.duration>=36000000000LL)+L" / "+TimeText(state_.duration,state_.duration>=36000000000LL);
            DrawTextW(dc,text.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        } else if(id==PreviewSeek) {
            const int padding=Dip(8),cy=height/2;
            RECT track{padding,cy-Dip(2),width-padding,cy+Dip(2)}; Fill(dc,track,RGB(82,86,91));
            const double fraction=state_.duration>0?double(state_.position)/double(state_.duration):0;
            const int x=padding+int(std::clamp(fraction,0.0,1.0)*std::max(0,width-padding*2));
            track.right=x; Fill(dc,track,RGB(40,184,105));
            Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush brush(Gdiplus::Color(255,240,242,244)); const float radius=float(Dip(5));
            g.FillEllipse(&brush,float(x)-radius,float(cy)-radius,radius*2,radius*2);
        } else {
            const auto icon=id==PreviewPlay?(state_.playing?ToolbarIcon::Pause:ToolbarIcon::Resume):
                id==PreviewMute?(state_.muted?ToolbarIcon::SystemMuted:ToolbarIcon::System):ToolbarIcon::Save;
            DrawToolbarIcon(dc,icon,rect,dpi_,enabled?Ink:MutedInk,18);
        }
        if((item.itemState&ODS_FOCUS) && keyboard_) { RECT focus=rect; InflateRect(&focus,-Dip(3),-Dip(3)); DrawFocusRect(dc,&focus); }
        SelectObject(dc,oldFont); DeleteObject(font); BitBlt(item.hDC,0,0,width,height,dc,0,0,SRCCOPY);
        SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    }
    void PaintVideo(HDC dc) {
        if(player_ && state_.ready && state_.error.empty()) { player_->UpdateVideo(); return; }
        RECT r{}; GetClientRect(video_,&r); Fill(dc,r,Background); InflateRect(&r,-Dip(24),-Dip(24));
        HFONT font=CreateToolbarFont(dpi_); auto old=SelectObject(dc,font); SetTextColor(dc,Ink); SetBkMode(dc,TRANSPARENT);
        const auto message=!state_.error.empty()?state_.error+L"\n\n仍可另存、完成或取消。":!notice_.empty()?notice_:L"正在加载视频…";
        DrawTextW(dc,message.c_str(),-1,&r,DT_CENTER|DT_WORDBREAK|DT_NOPREFIX); SelectObject(dc,old); DeleteObject(font);
    }
    void DragSeek(HWND child,LPARAM point) {
        RECT bounds{}; GetClientRect(child,&bounds); const int padding=Dip(8);
        const double fraction=std::clamp(double(GET_X_LPARAM(point)-padding)/std::max(1L,bounds.right-padding*2),0.0,1.0);
        SeekTo(int64_t(fraction*state_.duration),false);
    }
    static LRESULT CALLBACK ChildProc(HWND child,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<Preview*>(data); const int id=GetDlgCtrlID(child);
        if(message==WM_NCDESTROY) { RemoveWindowSubclass(child,ChildProc,1); return DefSubclassProc(child,message,w,l); }
        if(message==WM_ERASEBKGND) return 1;
        if(id==VideoId && message==WM_PAINT) { PAINTSTRUCT ps{}; auto dc=BeginPaint(child,&ps); self->PaintVideo(dc); EndPaint(child,&ps); return 0; }
        if(message==WM_LBUTTONDOWN) { self->keyboard_=false; if(id==VideoId) SetFocus(self->window_); }
        if(message==WM_MOUSEMOVE) { self->hover_=id; self->ShowControls(true); InvalidateRect(child,nullptr,FALSE); TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,child,0}; TrackMouseEvent(&track); }
        if(message==WM_MOUSELEAVE) { if(self->hover_==id) self->hover_=0; InvalidateRect(child,nullptr,FALSE); }
        if(message==WM_KEYDOWN && w==VK_TAB) { self->keyboard_=true; SendMessageW(self->window_,FocusMessage,GetKeyState(VK_SHIFT)<0,0); return 0; }
        if(id==PreviewSeek && self->state_.ready && !self->state_.busy) {
            if(message==WM_LBUTTONDOWN) { self->seeking_=true; self->resumeSeek_=self->state_.playing || self->playAfterSeek_; SetFocus(child); SetCapture(child); self->DragSeek(child,l); return 0; }
            if(message==WM_MOUSEMOVE && self->seeking_) { self->DragSeek(child,l); return 0; }
            if(message==WM_LBUTTONUP && self->seeking_) { self->DragSeek(child,l); self->seeking_=false; ReleaseCapture(); self->SeekTo(self->state_.position,self->resumeSeek_); return 0; }
            if(message==WM_CAPTURECHANGED && self->seeking_) { self->seeking_=false; self->SeekTo(self->state_.position,self->resumeSeek_); return 0; }
            if(message==WM_KEYDOWN) {
                if(w==VK_LEFT || w==VK_RIGHT || w==VK_HOME || w==VK_END) { self->SeekTo(w==VK_HOME?0:w==VK_END?self->state_.duration:self->state_.position+(w==VK_LEFT?-50000000:50000000),self->state_.playing); return 0; }
                if(w==VK_SPACE) { self->TogglePlay(); return 0; }
            }
        }
        return DefSubclassProc(child,message,w,l);
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l) override {
        if(m==WM_SIZE) { Layout(); return 0; }
        if(m==WM_DPICHANGED) { auto result=ToolWindow::Handle(m,w,l); Layout(); return result; }
        if(m==WM_GETMINMAXINFO) { auto* size=reinterpret_cast<MINMAXINFO*>(l); size->ptMinTrackSize={Dip(540),Dip(260)}; return 0; }
        if(m==WM_ERASEBKGND) return 1;
        if(m==WM_PAINT) {
            PAINTSTRUCT ps{}; auto dc=BeginPaint(window_,&ps); RECT r{}; GetClientRect(window_,&r); Fill(dc,r,Background);
            if(!notice_.empty()) {
                RECT text{Dip(12),r.bottom-Dip(98),r.right-Dip(12),r.bottom-Dip(56)};
                auto font=CreateToolbarFont(dpi_); auto old=SelectObject(dc,font); SetTextColor(dc,RGB(255,204,115)); SetBkMode(dc,TRANSPARENT);
                DrawTextW(dc,notice_.c_str(),-1,&text,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX|DT_END_ELLIPSIS); SelectObject(dc,old); DeleteObject(font);
            }
            EndPaint(window_,&ps); return 0;
        }
        if(m==WM_DRAWITEM) { DrawControl(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE; }
        if(m==WM_TIMER) { Tick(); return 0; }
        if(m==PlayerMessage) { if(w==generation_) Events(); return 0; }
        if(m==WM_COMMAND) { Command(LOWORD(w)); return 0; }
        if(m==WM_CLOSE) { if(!state_.busy && !dialog_) { ReleasePlayer(); if(DeleteOriginal()) DestroyWindow(window_); else EnableActions(); } return 0; }
        if(m==WM_DESTROY) { KillTimer(window_,1); ReleasePlayer(); return 0; }
        if(m==WM_KEYDOWN && w==VK_SPACE) { TogglePlay(); return 0; }
        if((m==WM_KEYDOWN && w==VK_TAB) || m==FocusMessage) {
            keyboard_=true; ShowControls(true); const int ids[]={PreviewPlay,PreviewSeek,PreviewMute,PreviewSave,PreviewComplete};
            int index=-1; for(int i=0;i<5;++i) if(GetDlgItem(window_,ids[i])==GetFocus()) index=i;
            const int step=(m==FocusMessage?w!=0:GetKeyState(VK_SHIFT)<0)?-1:1;
            for(int i=0;i<5;++i) { index=(index+step+5)%5; HWND child=GetDlgItem(window_,ids[index]); if(IsWindowEnabled(child)) { SetFocus(child); break; } }
            return 0;
        }
        if(m==WM_NOTIFY && reinterpret_cast<NMHDR*>(l)->hwndFrom==tooltip_ && reinterpret_cast<NMHDR*>(l)->code==TTN_GETDISPINFOW) {
            auto* tip=reinterpret_cast<NMTTDISPINFOW*>(l); const int id=GetDlgCtrlID(reinterpret_cast<HWND>(tip->hdr.idFrom));
            const wchar_t* text=id==PreviewPlay?(state_.playing?L"暂停":L"播放"):id==PreviewSeek?L"播放进度":
                id==PreviewMute?(state_.muted?L"开启声音":L"禁用声音"):id==PreviewSave?L"另存视频":L"完成（复制视频文件）";
            tip->lpszText=const_cast<wchar_t*>(text); return 0;
        }
        return ToolWindow::Handle(m,w,l);
    }
    AnnotationRenderer graphicsLifetime_;
    std::wstring path_,notice_,saveTarget_;
    HWND video_{},tooltip_{}; RecordingPreviewState state_;
    bool mf_{},priming_{},ended_{},playAfterSeek_{},seeking_{},resumeSeek_{},dialog_{},keyboard_{}; int hover_{};
    bool seekPending_{},seekQueued_{}; int64_t seekTarget_{};
    const UINT_PTR generation_=nextGeneration++;
    winrt::com_ptr<IMFPMediaPlayer> player_; std::shared_ptr<PlayerMailbox> mailbox_;
    std::thread copyThread_; std::atomic<bool> copyDone_{false}; DWORD copyError_{};
};
}
std::unique_ptr<RecordingPreview> OpenRecordingPreview(RecordingResult result) { if(result.format==RecordingFormat::Gif)return OpenGifPreview(std::move(result)); return std::make_unique<Preview>(std::move(result)); }
HRESULT CopyRecordingFile(HWND owner,const std::wstring& path) { return CopyVideoFile(owner,path); }
}
