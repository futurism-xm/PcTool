#include "shared/async/cancellation.h"
#include "ocr/ocr_window.h"
#include "shared/ui/capture_ui.h"
#include "shared/annotation/annotation_renderer.h"
#include <objidl.h>
#include <gdiplus.h>
#include <richedit.h>
#include <future>
#include <chrono>
#include <atomic>
#include <winrt/base.h>

namespace capture {
namespace {
constexpr int ImageId=401,TextId=402,NumbersId=403;
constexpr wchar_t OwnerCookie[]=L"PcTool.OcrOwner";
std::atomic<UINT_PTR> nextCookie{1};
void Fill(HDC dc,RECT rect,COLORREF color) { HBRUSH b=CreateSolidBrush(color); FillRect(dc,&rect,b); DeleteObject(b); }
void RoundedPath(Gdiplus::GraphicsPath& path,const Gdiplus::RectF& r,float radius) {
    const float d=std::min(radius*2,std::min(r.Width,r.Height));
    path.AddArc(r.X,r.Y,d,d,180,90); path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90); path.AddArc(r.X,r.GetBottom()-d,d,d,90,90); path.CloseFigure();
}
struct Recognized { OcrDocument document; std::wstring error; uint64_t generation{}; };
struct OcrWork { Image image; Cancellation cancellation; };
class OcrWindow final : public ToolWindow {
public:
    explicit OcrWindow(OcrRequest request):request_(std::move(request)),workState_(std::make_shared<OcrWork>()) {
        try {
        workState_->image=std::move(request_.image); workState_->cancellation.generation=cookie_;
        if(!Create(L"正在识别 · PcTool",180,62,WS_POPUP,WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_LAYERED)) throw std::runtime_error("Cannot create OCR progress");
        SetLayeredWindowAttributes(window_,0,225,LWA_ALPHA);
        if(request_.owner && IsWindow(request_.owner)) {
            SetPropW(request_.owner,OwnerCookie,reinterpret_cast<HANDLE>(cookie_)); EnableWindow(request_.owner,FALSE);
            SetWindowLongPtrW(window_,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(request_.owner));
        }
        RECT region=request_.busyRegion;
        if(IsRectEmpty(&region)) { POINT p{}; GetCursorPos(&p); region={p.x-90,p.y-31,p.x+90,p.y+31}; }
        SetWindowPos(window_,HWND_TOPMOST,(region.left+region.right-Dip(180))/2,(region.top+region.bottom-Dip(62))/2,Dip(180),Dip(62),SWP_SHOWWINDOW);
        SetForegroundWindow(window_); SetTimer(window_,1,40,nullptr);
        const auto state=workState_;
        work_=std::async(std::launch::async,[state] {
            Recognized result; result.generation=state->cancellation.generation;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Uninit { ~Uninit(){winrt::uninit_apartment();} } uninit;
                result.document=RecognizeLocalDocument(state->image,state->cancellation);
            } catch(const winrt::hresult_error& e) { result.error=ErrorMessage(L"识别失败",e.code())+L"\n"+e.message().c_str(); }
            catch(const std::exception& e) {result.error=L"本地文字识别失败：\n"+std::wstring(winrt::to_hstring(e.what()).c_str())+L"\n请确认程序目录中的 onnxruntime.dll 和 ocr_models 文件夹完整。";}
            catch(...) { result.error=L"识别失败：图像过大或系统资源不足。"; }
            return result;
        });
        } catch(...) { RestoreOwner(false); if(window_) DestroyWindow(window_); throw; }
    }
    ~OcrWindow() override {
        workState_->cancellation.requested=true; RestoreOwner(false);
        if(work_.valid()) work_.wait();
        if(window_) DestroyWindow(window_);
        if(bitmap_) DeleteObject(bitmap_); if(numberFont_) DeleteObject(numberFont_);
        if(richLibrary_) FreeLibrary(richLibrary_);
    }
private:
    bool OwnerValid() const { return request_.owner && IsWindow(request_.owner) && GetPropW(request_.owner,OwnerCookie)==reinterpret_cast<HANDLE>(cookie_); }
    void RestoreOwner(bool success) {
        if(OwnerValid()) {
            RemovePropW(request_.owner,OwnerCookie); EnableWindow(request_.owner,TRUE);
            SendMessageW(request_.owner,OcrFinishedMessage,success,0);
            if(!success && IsWindow(request_.owner)) SetForegroundWindow(request_.owner);
        }
        request_.owner=nullptr;
    }
    void Finish() {
        auto result=work_.get();
        if(!workState_->cancellation.Accepts(result.generation)) return;
        if(!result.error.empty() || result.document.paragraphs.empty()) {
            ShowWindow(window_,SW_HIDE);
            MessageBoxW(window_,result.error.empty()?L"未识别到文字。":result.error.c_str(),L"文字识别",MB_OK|MB_ICONINFORMATION);
            RestoreOwner(false); DestroyWindow(window_); return;
        }
        document_=std::move(result.document);
        Image display;
        if(!request_.background.Empty()) {
            display=std::move(request_.background); imageOffset_={request_.imageBounds.left,request_.imageBounds.top};
            for(auto& p:display.pixels) p=0xff000000|(((p>>16&255)*2/5)<<16)|(((p>>8&255)*2/5)<<8)|((p&255)*2/5);
            for(int y=0;y<workState_->image.height;++y) for(int x=0;x<workState_->image.width;++x) {
                const int xx=x+imageOffset_.x,yy=y+imageOffset_.y;
                if(xx>=0 && yy>=0 && xx<display.width && yy<display.height) display.pixels[size_t(yy)*display.width+xx]=workState_->image.pixels[size_t(y)*workState_->image.width+x];
            }
        } else { display=std::move(workState_->image); request_.imageBounds={}; }
        displayWidth_=display.width; displayHeight_=display.height; bitmap_=ToBitmap(display);
        if(!bitmap_) throw std::runtime_error("Cannot allocate OCR preview");
        richLibrary_=LoadLibraryW(L"Msftedit.dll"); if(!richLibrary_) throw std::runtime_error("RichEdit unavailable");
        image_=Child(L"STATIC",L"",SS_NOTIFY|WS_HSCROLL|WS_VSCROLL,ImageId);
        numbers_=Child(L"STATIC",L"",SS_NOTIFY,NumbersId);
        text_=Child(MSFTEDIT_CLASS,L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,TextId);
        if(!image_ || !numbers_ || !text_) throw std::runtime_error("Cannot create OCR result controls");
        for(HWND child:{image_,numbers_,text_}) SetWindowSubclass(child,ChildProc,1,reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(text_,EM_EXLIMITTEXT,0,0x7ffffffe); SendMessageW(text_,EM_SETBKGNDCOLOR,0,RGB(255,255,255));
        SendMessageW(text_,EM_SETEVENTMASK,0,ENM_SCROLL|ENM_UPDATE);
        std::wstring richText;
        for(const auto& paragraph:document_.paragraphs) {
            if(!richText.empty()) richText+=L"\r\r";
            const LONG begin=LONG(richText.size());
            for(size_t i=0;i<paragraph.lines.size();++i) { if(i) richText+=L'\r'; richText+=paragraph.lines[i].text; }
            ranges_.push_back({begin,LONG(richText.size())});
        }
        SetWindowTextW(text_,richText.c_str());
        ready_=true;
        SetWindowLongPtrW(window_,GWLP_HWNDPARENT,0);
        SetWindowLongPtrW(window_,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN);
        SetWindowLongPtrW(window_,GWL_EXSTYLE,0);
        SetWindowTextW(window_,L"文字识别 · PcTool");
        POINT p{}; GetCursorPos(&p); MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST),&mi);
        const int width=std::min(Dip(1180),int(mi.rcWork.right-mi.rcWork.left)*9/10),height=std::min(Dip(760),int(mi.rcWork.bottom-mi.rcWork.top)*9/10);
        SetWindowPos(window_,HWND_NOTOPMOST,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-width)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-height)/2,width,height,SWP_FRAMECHANGED|SWP_SHOWWINDOW);
        Fonts(); Layout(); Fit(); ColorText(); RestoreOwner(true); SetForegroundWindow(window_); SetFocus(text_);
    }
    void Fonts() {
        if(numberFont_) DeleteObject(numberFont_);
        numberFont_=CreateFontW(-Dip(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        CHARFORMAT2W format{}; format.cbSize=sizeof(format); format.dwMask=CFM_FACE|CFM_SIZE|CFM_COLOR; HDC dc=GetDC(text_); format.yHeight=MulDiv(Dip(14),1440,GetDeviceCaps(dc,LOGPIXELSY)); ReleaseDC(text_,dc); format.crTextColor=RGB(35,35,35); wcscpy_s(format.szFaceName,L"Microsoft YaHei UI");
        SendMessageW(text_,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));
        SendMessageW(text_,EM_SETZOOM,1,1);
        // Keep the original CR separators for copying; only their visual height changes.
        CHARRANGE selection{}; SendMessageW(text_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
        SendMessageW(text_,EM_SETSEL,0,-1);
        PARAFORMAT2 paragraph{}; paragraph.cbSize=sizeof(paragraph);
        paragraph.dwMask=PFM_SPACEBEFORE|PFM_SPACEAFTER|PFM_LINESPACING;
        paragraph.bLineSpacingRule=0; SendMessageW(text_,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&paragraph));
        dc=GetDC(text_); paragraph.dyLineSpacing=MulDiv(Dip(6),1440,GetDeviceCaps(dc,LOGPIXELSY)); ReleaseDC(text_,dc);
        paragraph.bLineSpacingRule=4;
        for(size_t i=1;i<ranges_.size();++i) {
            CHARRANGE separator{ranges_[i].cpMin-1,ranges_[i].cpMin-1};
            SendMessageW(text_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&separator));
            SendMessageW(text_,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&paragraph));
        }
        SendMessageW(text_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));
    }
    int NumberWidth(size_t number) const {
        HDC dc=GetDC(window_); auto old=SelectObject(dc,numberFont_); SIZE size{};
        auto label=std::to_wstring(number); GetTextExtentPoint32W(dc,label.c_str(),int(label.size()),&size);
        SelectObject(dc,old); ReleaseDC(window_,dc); return std::max(Dip(20),int(size.cx)+Dip(8));
    }
    void Layout() {
        if(!ready_) return; RECT r{}; GetClientRect(window_,&r);
        const int split=std::clamp<int>(int(r.right*0.7),Dip(220),std::max(Dip(220),int(r.right)-Dip(180)));
        MoveWindow(image_,0,0,split,r.bottom,FALSE);
        const int gutter=NumberWidth(ranges_.size())+Dip(6),textLeft=split+Dip(12)+gutter;
        MoveWindow(numbers_,split+Dip(8),Dip(16),gutter,std::max(1L,r.bottom-Dip(32)),FALSE);
        MoveWindow(text_,textLeft,Dip(16),std::max(1L,r.right-textLeft-Dip(12)),std::max(1L,r.bottom-Dip(32)),FALSE);
        RECT editRect{}; GetClientRect(text_,&editRect); editRect.right-=Dip(4); SendMessageW(text_,EM_SETRECT,0,reinterpret_cast<LPARAM>(&editRect));
        UpdateView(); RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    void Fit() {
        RECT r{}; GetClientRect(image_,&r);
        const RECT focus=IsRectEmpty(&request_.imageBounds)?RECT{0,0,displayWidth_,displayHeight_}:request_.imageBounds;
        zoom_=std::max(0.01,std::min(double(r.right)/(focus.right-focus.left),double(r.bottom)/(focus.bottom-focus.top)));
        panX_=(focus.left+focus.right)*zoom_/2-r.right/2; panY_=(focus.top+focus.bottom)*zoom_/2-r.bottom/2;
        fitted_=true; UpdateView();
    }
    void UpdateView() {
        if(!image_) return; RECT r{}; GetClientRect(image_,&r);
        panX_=std::clamp(panX_,0.0,std::max(0.0,displayWidth_*zoom_-r.right));
        panY_=std::clamp(panY_,0.0,std::max(0.0,displayHeight_*zoom_-r.bottom));
        for(int bar:{SB_HORZ,SB_VERT}) {
            SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS}; si.nMax=std::max(0,int((bar==SB_HORZ?displayWidth_:displayHeight_)*zoom_)-1);
            si.nPage=UINT(bar==SB_HORZ?r.right:r.bottom); si.nPos=int(bar==SB_HORZ?panX_:panY_); SetScrollInfo(image_,bar,&si,TRUE);
        }
        InvalidateRect(image_,nullptr,FALSE);
    }
    POINT Origin() const {
        RECT r{}; GetClientRect(image_,&r);
        return {LONG(std::max(0.0,(r.right-displayWidth_*zoom_)/2)-panX_),LONG(std::max(0.0,(r.bottom-displayHeight_*zoom_)/2)-panY_)};
    }
    RECT LineRect(const OcrTextLine& line) const {
        const POINT o=Origin();
        return {LONG(o.x+(line.x+imageOffset_.x)*zoom_),LONG(o.y+(line.y+imageOffset_.y)*zoom_),
            LONG(o.x+(line.x+line.width+imageOffset_.x)*zoom_+1),LONG(o.y+(line.y+line.height+imageOffset_.y)*zoom_+1)};
    }
    int HitImage(POINT p) const {
        for(size_t i=0;i<document_.paragraphs.size();++i) for(const auto& line:document_.paragraphs[i].lines) { RECT r=LineRect(line); InflateRect(&r,Dip(2),Dip(2)); if(PtInRect(&r,p)) return int(i); }
        return -1;
    }
    int HitResultRow(int y) const {
        for(size_t i=0;i<ranges_.size();++i) {
            POINTL start{},end{}; SendMessageW(text_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&start),ranges_[i].cpMin);
            SendMessageW(text_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&end),std::max(ranges_[i].cpMin,ranges_[i].cpMax-1));
            LONG bottom=end.y+Dip(20);
            if(i+1<ranges_.size()) { POINTL separator{}; SendMessageW(text_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&separator),ranges_[i+1].cpMin-1); bottom=separator.y; }
            if(y>=start.y && y<bottom) return int(i);
        }
        return -1;
    }
    int HitText(POINT p) const { return HitResultRow(p.y); }
    void ColorText() {
        if(!ready_ || styling_) return; styling_=true;
        CHARRANGE selection{}; POINT scroll{}; SendMessageW(text_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection)); SendMessageW(text_,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
        SendMessageW(text_,WM_SETREDRAW,FALSE,0);
        CHARFORMAT2W format{}; format.cbSize=sizeof(format); format.dwMask=CFM_COLOR; format.crTextColor=RGB(35,35,35);
        SendMessageW(text_,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format)); format.crTextColor=RGB(235,45,48);
        for(int index:{selected_,hovered_}) if(index>=0) { SendMessageW(text_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&ranges_[index])); SendMessageW(text_,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&format)); }
        SendMessageW(text_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection)); SendMessageW(text_,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
        SendMessageW(text_,WM_SETREDRAW,TRUE,0); InvalidateRect(text_,nullptr,FALSE); InvalidateRect(image_,nullptr,FALSE); InvalidateRect(numbers_,nullptr,FALSE); styling_=false;
    }
    void Hover(int index) { if(index!=hovered_) {hovered_=index; SetPropW(window_,L"OcrHovered",reinterpret_cast<HANDLE>(INT_PTR(index+1))); ColorText();} }
    void Select(int index,bool fromImage) {
        if(index<0) return; selected_=index; ColorText();
        if(fromImage) {
            CHARRANGE range=ranges_[index]; range.cpMax=range.cpMin; SendMessageW(text_,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range)); SendMessageW(text_,EM_SCROLLCARET,0,0); SetFocus(text_); InvalidateRect(numbers_,nullptr,FALSE);
        } else {
            RECT r=LineRect(document_.paragraphs[index].lines.front()); RECT viewport{}; GetClientRect(image_,&viewport);
            if(r.left<0 || r.right>viewport.right) panX_+=r.left-viewport.right/4;
            if(r.top<0 || r.bottom>viewport.bottom) panY_+=r.top-viewport.bottom/3;
            UpdateView();
        }
        // Exposed as a stable diagnostic for native integration tests.
        SetPropW(window_,L"OcrSelected",reinterpret_cast<HANDLE>(INT_PTR(index+1)));
    }
    void PaintImage(HDC dc,RECT r) {
        Fill(dc,r,RGB(238,240,243)); if(!bitmap_) return;
        Gdiplus::Graphics g(dc); g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Bitmap bitmap(bitmap_,nullptr); POINT o=Origin(); g.DrawImage(&bitmap,Gdiplus::RectF(float(o.x),float(o.y),float(displayWidth_*zoom_),float(displayHeight_*zoom_)));
        Gdiplus::Pen border(Gdiplus::Color(255,240,35,40),float(std::max(1,Dip(1)))); Gdiplus::SolidBrush selected(Gdiplus::Color(64,255,25,25));
        for(size_t i=0;i<document_.paragraphs.size();++i) {
            for(const auto& line:document_.paragraphs[i].lines) {
                RECT box=LineRect(line); Gdiplus::RectF b(float(box.left),float(box.top),float(box.right-box.left),float(box.bottom-box.top));
                Gdiplus::GraphicsPath outline; RoundedPath(outline,b,float(Dip(2)));
                if(int(i)==selected_) g.FillPath(&selected,&outline); g.DrawPath(&border,&outline);
            }
        }
        // Draw the marker last so nearby line outlines cannot cross its number.
        if(hovered_>=0) {
                const size_t i=size_t(hovered_);
                RECT box=LineRect(document_.paragraphs[i].lines.front());
                if(box.bottom<0 || box.top>r.bottom || box.right<0 || box.left>r.right) return;
                const float width=float(NumberWidth(i+1)),body=float(Dip(20)),height=float(Dip(26));
                const bool below=box.top<Dip(28);
                const float x=float(std::clamp<int>(box.left,0,std::max(0,int(r.right-width))));
                const float y=float(std::clamp<int>(below?box.bottom+Dip(2):box.top-Dip(28),0,std::max(0,int(r.bottom-height))));
                Gdiplus::GraphicsPath drop;
                drop.AddBezier(0.0F,body/2,0.0F,-body/6,width,-body/6,width,body/2);
                drop.AddBezier(width,body/2,width,body*0.8F,width*0.7F,body,width/2,height);
                drop.AddBezier(width/2,height,width*0.3F,body,0.0F,body*0.8F,0.0F,body/2); drop.CloseFigure();
                Gdiplus::Matrix transform(1,0,0,below?-1.0F:1.0F,x,y+(below?height:0)); drop.Transform(&transform);
                Gdiplus::SolidBrush red(Gdiplus::Color(255,235,45,48)); g.FillPath(&red,&drop); g.Flush();
                RECT badge{LONG(x),LONG(y+(below?height-body:0)),LONG(x+width),LONG(y+(below?height:body))};
                auto old=SelectObject(dc,numberFont_); SetTextColor(dc,RGB(255,255,255)); SetBkMode(dc,TRANSPARENT);
                auto number=std::to_wstring(i+1); DrawTextW(dc,number.c_str(),-1,&badge,DT_CENTER|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,old);
        }
    }
    void PaintNumbers(HDC dc,RECT r) {
        Fill(dc,r,RGB(255,255,255)); auto old=SelectObject(dc,numberFont_); SetBkMode(dc,TRANSPARENT);
        for(size_t i=0;i<ranges_.size();++i) {
            POINTL point{}; SendMessageW(text_,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&point),ranges_[i].cpMin);
            RECT badge{Dip(2),point.y,Dip(2)+NumberWidth(i+1),point.y+Dip(20)};
            if(badge.bottom<0 || badge.top>r.bottom) continue;
            { Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
              Gdiplus::GraphicsPath outline; RoundedPath(outline,Gdiplus::RectF(float(badge.left),float(badge.top),float(badge.right-badge.left),float(badge.bottom-badge.top)),float(Dip(4)));
              Gdiplus::SolidBrush gray(Gdiplus::Color(255,237,237,237)); g.FillPath(&gray,&outline); }
            SetTextColor(dc,int(i)==selected_ || int(i)==hovered_?RGB(235,45,48):RGB(45,45,45));
            const auto number=std::to_wstring(i+1); DrawTextW(dc,number.c_str(),-1,&badge,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
        }
        SelectObject(dc,old);
    }
    void PaintBusy(HDC dc,RECT r) {
        Fill(dc,r,RGB(68,68,68)); Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Gdiplus::Color(255,0,186,209),float(Dip(2))); pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawArc(&pen,float(Dip(20)),float(Dip(21)),float(Dip(20)),float(Dip(20)),float(GetTickCount64()%1000)*0.36F,260.0F);
        HFONT font=CreateFontW(-Dip(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI"); auto old=SelectObject(dc,font);
        SetTextColor(dc,RGB(255,255,255)); SetBkMode(dc,TRANSPARENT); r.left= Dip(50); DrawTextW(dc,L"正在识别",-1,&r,DT_SINGLELINE|DT_VCENTER); SelectObject(dc,old); DeleteObject(font);
    }
    static LRESULT CALLBACK ChildProc(HWND child,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<OcrWindow*>(data);
        if(message==WM_NCDESTROY) { RemoveWindowSubclass(child,ChildProc,1); return DefSubclassProc(child,message,w,l); }
        if(message==WM_ERASEBKGND && child!=self->text_) return 1;
        if(message==WM_PAINT && child!=self->text_) {
            PAINTSTRUCT ps{}; HDC target=BeginPaint(child,&ps); RECT r{}; GetClientRect(child,&r);
            HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,r.right),std::max(1L,r.bottom)); auto old=SelectObject(dc,bitmap);
            if(child==self->image_) self->PaintImage(dc,r); else self->PaintNumbers(dc,r);
            BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(child,&ps); return 0;
        }
        if(message==WM_KEYDOWN) {
            if(w==VK_ESCAPE) { SendMessageW(self->window_,WM_CLOSE,0,0); return 0; }
            if(GetKeyState(VK_CONTROL)<0 && w=='A') { SendMessageW(self->text_,EM_SETSEL,0,-1); SetFocus(self->text_); return 0; }
            if(GetKeyState(VK_CONTROL)<0 && w=='C') {
                CHARRANGE range{}; SendMessageW(self->text_,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&range));
                if(range.cpMin!=range.cpMax) SendMessageW(self->text_,WM_COPY,0,0);
                else if(self->selected_>=0) { const auto& p=self->document_.paragraphs[self->selected_]; CopyText(self->window_,self->document_.text.substr(p.begin,p.end-p.begin)); }
                return 0;
            }
        }
        if(message==WM_CHAR && (w==1 || w==3)) return 0;
        if(message==WM_MOUSEMOVE) {
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,child,0}; TrackMouseEvent(&track);
            POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(child==self->image_ && self->panning_) { self->panX_-=p.x-self->last_.x; self->panY_-=p.y-self->last_.y; self->last_=p; self->fitted_=false; self->UpdateView(); return 0; }
            if(GetCapture()!=self->text_) self->Hover(child==self->image_?self->HitImage(p):child==self->text_?self->HitText(p):self->HitResultRow(p.y));
        }
        if(message==WM_MOUSELEAVE && GetCapture()!=self->text_) self->Hover(-1);
        if(message==WM_LBUTTONDOWN) {
            POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(child==self->image_) {
                int hit=self->HitImage(p); if(hit>=0 && GetKeyState(VK_SPACE)>=0) self->Select(hit,true);
                else {self->panning_=true;self->last_=p;SetCapture(child);SetFocus(child);} return 0;
            }
            if(child==self->text_) self->Select(self->HitText(p),false);
            if(child==self->numbers_) { self->Select(self->HitResultRow(p.y),false); SetFocus(self->text_); return 0; }
        }
        if(message==WM_LBUTTONUP && child==self->image_) { self->panning_=false; if(GetCapture()==child) ReleaseCapture(); return 0; }
        if(message==WM_CAPTURECHANGED && child==self->image_) self->panning_=false;
        if(message==WM_LBUTTONDBLCLK && child==self->image_ && self->HitImage({GET_X_LPARAM(l),GET_Y_LPARAM(l)})<0) {self->Fit();return 0;}
        if(message==WM_MOUSEWHEEL && child==self->image_) {
            int delta=GET_WHEEL_DELTA_WPARAM(w); self->fitted_=false;
            if(GET_KEYSTATE_WPARAM(w)&MK_CONTROL) {
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(child,&p); auto origin=self->Origin();
                const double x=(p.x-origin.x)/self->zoom_,y=(p.y-origin.y)/self->zoom_;
                self->zoom_=std::clamp(self->zoom_*(delta>0?1.15:1/1.15),0.01,8.0);
                RECT r{};GetClientRect(child,&r);self->panX_=std::max(0.0,(r.right-self->displayWidth_*self->zoom_)/2)+x*self->zoom_-p.x;
                self->panY_=std::max(0.0,(r.bottom-self->displayHeight_*self->zoom_)/2)+y*self->zoom_-p.y;
            } else if(GET_KEYSTATE_WPARAM(w)&MK_SHIFT) self->panX_-=delta; else self->panY_-=delta;
            self->UpdateView(); return 0;
        }
        if((message==WM_HSCROLL || message==WM_VSCROLL) && child==self->image_) {
            int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT; SCROLLINFO si{sizeof(si),SIF_ALL}; GetScrollInfo(child,bar,&si); double& position=bar==SB_HORZ?self->panX_:self->panY_;
            switch(LOWORD(w)) {case SB_LINEUP:position-=40;break;case SB_LINEDOWN:position+=40;break;case SB_PAGEUP:position-=si.nPage;break;case SB_PAGEDOWN:position+=si.nPage;break;case SB_THUMBTRACK:case SB_THUMBPOSITION:position=si.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=si.nMax;break;}
            self->fitted_=false; self->UpdateView(); return 0;
        }
        const auto result=DefSubclassProc(child,message,w,l);
        if(child==self->text_ && (message==WM_VSCROLL || message==WM_MOUSEWHEEL || message==WM_KEYDOWN)) InvalidateRect(self->numbers_,nullptr,FALSE);
        return result;
    }
    LRESULT Handle(UINT message,WPARAM w,LPARAM l) override {
        if(message==WM_CLOSE) { workState_->cancellation.requested=true; RestoreOwner(false); KillTimer(window_,1); return ToolWindow::Handle(message,w,l); }
        if(message==WM_KEYDOWN && w==VK_ESCAPE) { SendMessageW(window_,WM_CLOSE,0,0); return 0; }
        if(message==WM_TIMER) {
            if(!ready_) {
                if(request_.owner && !OwnerValid()) { SendMessageW(window_,WM_CLOSE,0,0); return 0; }
                InvalidateRect(window_,nullptr,FALSE);
                if(work_.valid() && work_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
                    try { Finish(); } catch(...) { ShowWindow(window_,SW_HIDE); MessageBoxW(window_,CurrentError(L"无法显示识别结果").c_str(),L"PcTool",MB_OK|MB_ICONERROR); RestoreOwner(false); DestroyWindow(window_); }
                }
            } else InvalidateRect(numbers_,nullptr,FALSE);
            return 0;
        }
        if(message==WM_SIZE) { Layout(); if(ready_ && fitted_) Fit(); return 0; }
        if(message==WM_DPICHANGED) { auto result=ToolWindow::Handle(message,w,l); if(ready_) {Fonts();Layout();ColorText();} return result; }
        if(message==WM_GETMINMAXINFO && ready_) { auto* info=reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize={Dip(640),Dip(360)}; return 0; }
        if(message==WM_ERASEBKGND) return 1;
        if(message==WM_PAINT) {
            PAINTSTRUCT ps{}; HDC dc=BeginPaint(window_,&ps); RECT r{};GetClientRect(window_,&r);
            if(ready_) Fill(dc,r,RGB(255,255,255)); else PaintBusy(dc,r);
            EndPaint(window_,&ps); return 0;
        }
        if(message==WM_COMMAND && !styling_) { InvalidateRect(numbers_,nullptr,FALSE); return 0; }
        return ToolWindow::Handle(message,w,l);
    }
    AnnotationRenderer graphicsLifetime_;
    OcrRequest request_; OcrDocument document_; std::shared_ptr<OcrWork> workState_; std::future<Recognized> work_;
    const UINT_PTR cookie_=nextCookie++;
    HWND image_{},text_{},numbers_{}; HMODULE richLibrary_{}; HFONT numberFont_{}; HBITMAP bitmap_{};
    int displayWidth_{},displayHeight_{},selected_=-1,hovered_=-1;
    POINT imageOffset_{},last_{}; double zoom_=1,panX_{},panY_{};
    bool ready_{},styling_{},panning_{},fitted_=true;
    std::vector<CHARRANGE> ranges_;
};
}
std::unique_ptr<ToolWindow> OpenOcr(OcrRequest request) { return std::make_unique<OcrWindow>(std::move(request)); }
std::unique_ptr<ToolWindow> OpenOcr(Image image) { OcrRequest request; request.image=std::move(image); return OpenOcr(std::move(request)); }
}
