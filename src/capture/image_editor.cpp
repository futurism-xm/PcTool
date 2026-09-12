#include "shared/ui/capture_overlay_ui.h"
#include "shared/ui/popup_controls.h"
#include "capture/image_editor.h"
#include "ocr/ocr_window.h"
#include "shared/ui/capture_ui.h"
#include "capture/screenshot_overlay.h"
#include "shared/annotation/annotation_fragments.h"
#include <commdlg.h>
#include <array>
#include <stdexcept>

namespace capture {
class ImageEditor final : public ToolWindow {
public:
    explicit ImageEditor(std::shared_ptr<ImageDocument> document,std::optional<RECT> captureRegion)
        : document_(std::move(document)),renderer_(GetModuleHandleW(nullptr)),compact_(captureRegion.has_value()),captureRegion_(captureRegion.value_or(RECT{})) {
        auto& image=document_->image;
        renderer_.desktopBitmap_=ToBitmap(image);
        if(!renderer_.desktopBitmap_) throw std::runtime_error("Cannot allocate editor bitmap");
        renderer_.selection_={0,0,image.width,image.height};
        renderer_.virtualWidth_=image.width; renderer_.virtualHeight_=image.height;
        renderer_.selectionCommitted_=true; renderer_.dpi_=document_->dpi;
        renderer_.annotations_=document_->annotations;
        if(!Create(L"PcTool · 长图编辑",1000,800,compact_?WS_POPUP|WS_CLIPCHILDREN:WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_HSCROLL,compact_?WS_EX_TOPMOST|WS_EX_TOOLWINDOW:0)) throw std::runtime_error("Cannot create image editor");
        if(!compact_) {
        const wchar_t* names[]={L"选择",L"矩形",L"椭圆",L"箭头",L"画笔",L"马赛克",L"文本",L"序号"};
        for(int i=0;i<8;++i) Child(L"BUTTON",names[i],BS_PUSHBUTTON|WS_TABSTOP,100+i);
        Child(L"BUTTON",L"撤销",BS_PUSHBUTTON|WS_TABSTOP,110);
        Child(L"BUTTON",L"颜色…",BS_PUSHBUTTON|WS_TABSTOP,111);
        Child(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,112);
        for(const wchar_t* label:{L"细 / 11 号",L"中 / 14 号",L"粗 / 18 号",L"大 / 22 号"}) SendDlgItemMessageW(window_,112,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendDlgItemMessageW(window_,112,CB_SETCURSEL,0,0);
        Child(L"BUTTON",L"复制",BS_PUSHBUTTON|WS_TABSTOP,120);
        Child(L"BUTTON",L"保存 PNG",BS_PUSHBUTTON|WS_TABSTOP,121);
        Child(L"BUTTON",L"文字识别",BS_PUSHBUTTON|WS_TABSTOP,122);
        Child(L"BUTTON",L"钉图",BS_PUSHBUTTON|WS_TABSTOP,123);
        Child(L"BUTTON",L"适应窗口",BS_PUSHBUTTON|WS_TABSTOP,124);
        Child(L"BUTTON",L"100%",BS_PUSHBUTTON|WS_TABSTOP,125);
        Child(L"STATIC",L"",0,126);
        }
        if(compact_) {
            MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&captureRegion_,MONITOR_DEFAULTTONEAREST),&mi);work_=mi.rcWork;monitor_=mi.rcMonitor;
            dimension_=std::make_unique<CaptureLabel>(window_,L"PcTool · 编辑长图尺寸");
            scrollbar_.Create(window_,150,[this](int pos){CommitText();view_.y=pos;UpdateView();},RGB(255,255,255));
            compactBar_=std::make_unique<OverlayToolbar>(window_,L"PcTool · 长图编辑工具栏",std::vector<OverlayButton>{
                {101,ToolbarIcon::Rectangle,L"",L"矩形"},{102,ToolbarIcon::Ellipse,L"",L"椭圆"},{103,ToolbarIcon::Arrow,L"",L"箭头"},
                {104,ToolbarIcon::Pen,L"",L"画笔"},{105,ToolbarIcon::Mosaic,L"",L"马赛克"},{106,ToolbarIcon::Text,L"",L"文字"},{107,ToolbarIcon::Number,L"",L"序号"},
                {110,ToolbarIcon::Undo,L"",L"撤销"},{127,ToolbarIcon::Clear,L"",L"清除标注"},{122,ToolbarIcon::Ocr,L"",L"文字识别"},
                {121,ToolbarIcon::Save,L"",L"下载"},{128,ToolbarIcon::Cancel,L"",L"取消"},{129,ToolbarIcon::Finish,L"",L"复制完整长图",68}},
                [this](int id){PostMessageW(window_,WM_COMMAND,id,0);});
            const int width=std::min(image.width+Dip(10),int(work_.right-work_.left)-Dip(40));
            const int height=std::min(int(image.height*std::min(1.0,double(width-Dip(10))/image.width)),int(work_.bottom-work_.top)-Dip(120));
            SetWindowPos(window_,HWND_TOPMOST,work_.left+(work_.right-work_.left-width)/2,work_.top+(work_.bottom-work_.top-height-Dip(36))/2,width,std::max(Dip(80),height),SWP_NOACTIVATE);
            Layout();Fit();SetWindowPos(window_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);SetForegroundWindow(window_);return;
        }
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&mi);
        RECT wr{}; GetWindowRect(window_,&wr);
        SetWindowPos(window_,nullptr,mi.rcWork.left+Dip(30),mi.rcWork.top+Dip(30),
            std::min(int(wr.right-wr.left),int(mi.rcWork.right-mi.rcWork.left)-Dip(60)),
            std::min(int(wr.bottom-wr.top),int(mi.rcWork.bottom-mi.rcWork.top)-Dip(60)),SWP_NOZORDER);
        Layout(); Fit(); ShowWindow(window_,SW_SHOW);
    }
    ~ImageEditor() override {
        dimension_.reset();compactBar_.reset();stylePanel_.reset();surround_.Close();ocr_.clear();
        if(window_) DestroyWindow(window_);
        if(editorFont_) DeleteObject(editorFont_);
    }
private:
    int Top() const { return compact_||pinned_?0:Dip(88); }
    int CanvasWidth() const {return std::max(1L,Client().right-(compact_?Dip(10):0));}
    void UpdateCompactBar() {
        if(!compactBar_)return;
        for(int i=101;i<=107;++i)compactBar_->State(i,true,tool_==Tool(i-100));
        compactBar_->State(110,!history_.empty());compactBar_->State(127,!renderer_.annotations_.empty());
        if(tool_==Tool::None){if(stylePanel_)stylePanel_->Hide();return;}
        if(!stylePanel_)stylePanel_=std::make_unique<AnnotationStylePanel>(compactBar_->Window(),[this](AnnotationStyle style){style_=style;color_=style.color;},[this]{tool_=Tool::None;UpdateCompactBar();});
        stylePanel_->Update(style_,tool_==Tool::Text||tool_==Tool::Number,dpi_,compactBar_->Bounds(),monitor_);
    }
    void Layout() {
        if(compact_) {
            RECT bounds{};GetWindowRect(window_,&bounds);
            if(compactBar_){surround_.Show(window_,bounds,monitor_,dpi_);compactBar_->PlaceNear(bounds,work_,dpi_);UpdateCompactBar();}
            if(dimension_)dimension_->Show(std::to_wstring(document_->image.width)+L" × "+std::to_wstring(document_->image.height),bounds,work_,dpi_,false);
            if(scrollbar_.Window())MoveWindow(scrollbar_.Window(),Client().right-Dip(8),0,Dip(6),Client().bottom,TRUE);
            UpdateView();return;
        }
        for(int i=0;i<8;++i) Place(100+i,8+i*52,8,48,30);
        Place(110,428,8,54,30); Place(111,486,8,64,30); Place(112,556,8,112,200);
        for(int i=0;i<6;++i) Place(120+i,8+i*88,46,82,30);
        Place(126,540,48,390,28);
        UpdateView();
    }
    RECT Client() const { RECT r{}; GetClientRect(window_,&r); return r; }
    void UpdateView() {
        if(!window_) return;
        const RECT r=Client();
        const int pageX=std::max(1,int(CanvasWidth()/view_.zoom));
        const int pageY=std::max(1,int((r.bottom-Top())/view_.zoom));
        view_.x=std::clamp(view_.x,0.0,double(std::max(0,document_->image.width-pageX)));
        view_.y=std::clamp(view_.y,0.0,double(std::max(0,document_->image.height-pageY)));
        SCROLLINFO s{sizeof(s),SIF_RANGE|SIF_PAGE|SIF_POS,0,document_->image.width-1,UINT(pageX),int(view_.x)};
        if(!compact_)SetScrollInfo(window_,SB_HORZ,&s,TRUE);
        s.nMax=document_->image.height-1; s.nPage=pageY; s.nPos=int(view_.y);
        if(compact_){if(scrollbar_.Window())scrollbar_.Update(document_->image.height,pageY,int(view_.y),dpi_);}
        else SetScrollInfo(window_,SB_VERT,&s,TRUE);
        Text(126,std::to_wstring(document_->image.width)+L" × "+std::to_wstring(document_->image.height)+L"  ·  "+std::to_wstring(int(view_.zoom*100))+L"%");
        PositionText(); InvalidateRect(window_,nullptr,FALSE);
    }
    void Fit() {
        CommitText(); auto r=Client();
        view_.zoom=compact_?std::min(1.0,double(CanvasWidth())/document_->image.width):std::min({1.0,double(std::max(1L,r.right-8))/document_->image.width,double(std::max(1L,r.bottom-Top()-8))/document_->image.height});
        view_.x=0; view_.y=0; UpdateView();
    }
    void Snapshot() {
        if(history_.size()>=32) history_.erase(history_.begin());
        history_.push_back(renderer_.annotations_);
    }
    void Changed() { document_->annotations=renderer_.annotations_; UpdateCompactBar();InvalidateRect(window_,nullptr,FALSE); }
    POINT ImagePoint(POINT p) const {
        p=view_.ToImage(p,Top());
        p.x=std::clamp(p.x,0L,LONG(document_->image.width-1));
        p.y=std::clamp(p.y,0L,LONG(document_->image.height-1)); return p;
    }
    void Paint(HDC printContext=nullptr) {
        PAINTSTRUCT ps{}; HDC dc=printContext?printContext:BeginPaint(window_,&ps);
        RECT r=Client(); HDC output=CreateCompatibleDC(dc);
        if(printContext) ps.rcPaint=r;
        HBITMAP buffer=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom));
        if(!output || !buffer) { if(output) DeleteDC(output); if(buffer) DeleteObject(buffer); if(!printContext) EndPaint(window_,&ps); return; }
        auto old=SelectObject(output,buffer); FillRect(output,&r,compact_?static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)):GetSysColorBrush(COLOR_APPWORKSPACE));
        if(!pinned_&&!compact_) { RECT bar{0,0,r.right,Top()}; FillRect(output,&bar,GetSysColorBrush(COLOR_BTNFACE)); }
        // Render only the visible native-pixel strip; the source document remains full resolution.
        const int x=int(view_.x), y=int(view_.y);
        const int width=std::min(document_->image.width-x,std::max(1,int(std::ceil(CanvasWidth()/view_.zoom))));
        const int height=std::min(document_->image.height-y,std::max(1,int(std::ceil((r.bottom-Top())/view_.zoom))));
        HDC native=CreateCompatibleDC(dc),source=CreateCompatibleDC(dc);
        HBITMAP tile=CreateCompatibleBitmap(dc,width,height);
        if(native && source && tile) {
            auto nativeOld=SelectObject(native,tile),sourceOld=SelectObject(source,renderer_.desktopBitmap_);
            BitBlt(native,0,0,width,height,source,x,y,SRCCOPY);
            const RECT visible{x,y,x+width,y+height};
            for(size_t i=0;i<renderer_.annotations_.size();++i) {
                if(int(i)==editingIndex_) continue;
                auto bounds=renderer_.AnnotationBounds(renderer_.annotations_[i]);
                InflateRect(&bounds,100,100); RECT overlap{};
                if(IntersectRect(&overlap,&bounds,&visible)) renderer_.PaintAnnotation(native,renderer_.annotations_[i],-x,-y);
            }
            if(drawing_) renderer_.PaintAnnotation(native,working_,-x,-y);
            SetStretchBltMode(output,HALFTONE); SetBrushOrgEx(output,0,0,nullptr);
            StretchBlt(output,0,Top(),int(std::ceil(width*view_.zoom)),int(std::ceil(height*view_.zoom)),native,0,0,width,height,SRCCOPY);
            SelectObject(native,nativeOld); SelectObject(source,sourceOld);
        }
        if(tile) DeleteObject(tile); if(native) DeleteDC(native); if(source) DeleteDC(source);
        BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,output,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
        SelectObject(output,old); DeleteObject(buffer); DeleteDC(output); if(!printContext) EndPaint(window_,&ps);
    }
    Image Render() {
        CommitText(); HBITMAP bitmap=renderer_.CreateOutputBitmap(true);
        if(!bitmap) throw std::runtime_error("Cannot render image");
        try { auto image=FromBitmap(bitmap); DeleteObject(bitmap); return image; }
        catch(...) { DeleteObject(bitmap); throw; }
    }
    void BeginText(Annotation a,int index) {
        CommitText(); editing_=std::move(a); editingIndex_=index;
        text_=Child(L"EDIT",editing_.text.c_str(),WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_TABSTOP,140);
        SendMessageW(text_,EM_SETLIMITTEXT,1000000,0);
        SetWindowSubclass(text_,TextProc,1,reinterpret_cast<DWORD_PTR>(this));
        PositionText(); SetFocus(text_); SendMessageW(text_,EM_SETSEL,0,-1);
    }
    static LRESULT CALLBACK TextProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<ImageEditor*>(data);
        if(m==WM_KEYDOWN && (wp==VK_ESCAPE || (wp==VK_RETURN && GetKeyState(VK_CONTROL)<0))) {
            if(wp==VK_ESCAPE) self->CancelText(); else self->CommitText(); return 0;
        }
        return DefSubclassProc(w,m,wp,lp);
    }
    void PositionText() {
        if(!text_) return;
        if(editorFont_) { SendMessageW(text_,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE); DeleteObject(editorFont_); }
        editorFont_=CreateFontW(-std::max(1,int(editing_.size*view_.zoom)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei UI");
        SendMessageW(text_,WM_SETFONT,reinterpret_cast<WPARAM>(editorFont_),TRUE);
        POINT p=view_.ToClient(editing_.start,Top()); const auto r=Client();
        const int width=std::min(std::max(Dip(180),int((editing_.end.x-editing_.start.x)*view_.zoom)),std::max(Dip(40),int(r.right-p.x)));
        const int height=std::min(std::max(Dip(80),int((editing_.end.y-editing_.start.y)*view_.zoom)),std::max(Dip(30),int(r.bottom-p.y)));
        MoveWindow(text_,p.x,std::max(Top(),int(p.y)),width,height,TRUE);
    }
    void CancelText() {
        if(!text_) return;
        HWND edit=text_; text_=nullptr; DestroyWindow(edit); editingIndex_=-1; SetFocus(window_); InvalidateRect(window_,nullptr,FALSE);
    }
    void CommitText() {
        if(!text_) return;
        editing_.text=WindowText(text_);
        Snapshot();
        if(editing_.tool==Tool::Number || !editing_.text.empty()) {
            renderer_.AdaptTextAnnotationLayout(editing_);
            if(editingIndex_>=0) ReplaceAnnotationGroup(renderer_.annotations_,editingIndex_,editing_);
            else renderer_.annotations_.push_back(editing_);
        } else if(editingIndex_>=0) EraseAnnotationGroup(renderer_.annotations_,editingIndex_);
        CancelText(); Changed();
    }
    void PointerDown(POINT p) {
        if(p.y<Top()) return;
        if(pinned_) { ReleaseCapture(); SendMessageW(window_,WM_NCLBUTTONDOWN,HTCAPTION,0); return; }
        CommitText(); SetFocus(window_); p=ImagePoint(p); pointer_=p;
        moving_=renderer_.HitTestAnnotation(p,true);
        selected_=moving_;
        if(moving_>=0) {
            Snapshot();before_=renderer_.annotations_[moving_];beforeGroup_.clear();
            for(size_t i=0;i<renderer_.annotations_.size();++i)
                if(int(i)==moving_||(before_.fragmentGroup&&renderer_.annotations_[i].fragmentGroup==before_.fragmentGroup))
                    beforeGroup_.push_back({i,renderer_.annotations_[i]});
            SetCapture(window_);return;
        }
        if(tool_==Tool::None){if(compact_){ReleaseCapture();SendMessageW(window_,WM_NCLBUTTONDOWN,HTCAPTION,0);}return;}
        working_={}; working_.tool=tool_; working_.start=p; working_.end=p; working_.color=color_;
        int size=compact_?style_.lineIndex:int(SendDlgItemMessageW(window_,112,CB_GETCURSEL,0,0)); size=std::clamp(size,0,3);
        const int strokes[]={2,4,7,12},fonts[]={11,14,18,22},mosaics[]={12,24,40,64};
        working_.size=MulDiv(tool_==Tool::Text||tool_==Tool::Number?MulDiv(fonts[size],96,72):tool_==Tool::Mosaic?mosaics[size]:strokes[size],int(document_->dpi),96);
        if(compact_&&(tool_==Tool::Text||tool_==Tool::Number))working_.size=MulDiv(style_.fontPoints,int(document_->dpi),72);
        working_.blurLevel=3; working_.points.push_back(p);
        if(tool_==Tool::Text || tool_==Tool::Number) {
            working_.number=renderer_.NextNumberAfterAnnotations();
            BeginText(working_,-1); return;
        }
        Snapshot(); drawing_=true; SetCapture(window_);
    }
    void PointerMove(POINT p) {
        if(!drawing_ && moving_<0) return;
        p=ImagePoint(p);
        if(moving_>=0) {
            RECT bounds=renderer_.AnnotationBounds(before_);
            for(const auto& part:beforeGroup_){auto b=renderer_.AnnotationBounds(part.second);UnionRect(&bounds,&bounds,&b);}
            int dx=p.x-pointer_.x,dy=p.y-pointer_.y;
            dx=std::clamp(dx,-int(bounds.left),std::max(-int(bounds.left),document_->image.width-int(bounds.right)));
            dy=std::clamp(dy,-int(bounds.top),std::max(-int(bounds.top),document_->image.height-int(bounds.bottom)));
            for(const auto& part:beforeGroup_){auto a=part.second;renderer_.OffsetAnnotation(a,dx,dy);renderer_.annotations_[part.first]=std::move(a);}
        } else {
            working_.end=p;
            if(tool_==Tool::Pen || tool_==Tool::Mosaic) {
                if(p.x!=working_.points.back().x || p.y!=working_.points.back().y) working_.points.push_back(p);
            }
        }
        InvalidateRect(window_,nullptr,FALSE);
    }
    void PointerUp() {
        if(drawing_) renderer_.annotations_.push_back(std::move(working_));
        drawing_=false; moving_=-1; if(GetCapture()==window_) ReleaseCapture(); Changed();
    }
    void TogglePin() {
        CommitText(); pinned_=!pinned_;
        for(int i=100;i<=126;++i) if(HWND c=GetDlgItem(window_,i)) ShowWindow(c,pinned_?SW_HIDE:SW_SHOW);
        SetWindowTextW(window_,pinned_?L"PcTool 钉图 · 双击图片继续编辑":L"PcTool · 长图编辑");
        SetWindowPos(window_,pinned_?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        tool_=Tool::None; UpdateView();
    }
    LRESULT Handle(UINT m,WPARAM w,LPARAM l) override {
        if(m==WM_MOVE&&compact_){Layout();return 0;}
        if(m==WM_MOVING&&compact_){auto* r=reinterpret_cast<RECT*>(l);const int width=r->right-r->left,height=r->bottom-r->top;r->left=std::clamp<int>(r->left,work_.left,std::max(work_.left,work_.right-width));r->top=std::clamp<int>(r->top,work_.top,std::max(work_.top,work_.bottom-height));r->right=r->left+width;r->bottom=r->top+height;return TRUE;}
        if(m==WM_DESTROY&&compact_){dimension_.reset();stylePanel_.reset();compactBar_.reset();surround_.Close();return 0;}
        if(m==WM_DPICHANGED&&compact_){auto result=ToolWindow::Handle(m,w,l);Layout();return result;}
        if(m==WM_SIZE) { Layout(); return 0; }
        if(m==WM_ERASEBKGND) return 1;
        if(m==WM_PAINT) { Paint(); return 0; }
        if(m==WM_PRINTCLIENT) { Paint(reinterpret_cast<HDC>(w)); return 0; }
        if(m==WM_GETMINMAXINFO) { auto* info=reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize={Dip(700),Dip(300)}; return 0; }
        if(m==WM_LBUTTONDOWN) { PointerDown({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); return 0; }
        if(m==WM_MOUSEMOVE) { PointerMove({GET_X_LPARAM(l),GET_Y_LPARAM(l)}); return 0; }
        if(m==WM_LBUTTONUP) { PointerUp(); return 0; }
        if(m==WM_CAPTURECHANGED && (drawing_ || moving_>=0)) { PointerUp(); return 0; }
        if(m==WM_LBUTTONDBLCLK) {
            if(pinned_) TogglePin();
            else { const int index=renderer_.HitTestAnnotation(ImagePoint({GET_X_LPARAM(l),GET_Y_LPARAM(l)}),true); if(index>=0) { const auto& a=renderer_.annotations_[index]; if(a.tool==Tool::Text || a.tool==Tool::Number) BeginText(a,index); } }
            return 0;
        }
        if(m==WM_MOUSEWHEEL) {
            CommitText(); const int delta=GET_WHEEL_DELTA_WPARAM(w);
            if(GET_KEYSTATE_WPARAM(w)&MK_CONTROL) {
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(window_,&p); const POINT anchor=view_.ToImage(p,Top());
                view_.zoom=std::clamp(view_.zoom*std::pow(1.2,double(delta)/120),0.01,8.0);
                view_.x=anchor.x-p.x/view_.zoom; view_.y=anchor.y-(p.y-Top())/view_.zoom;
            } else if(GET_KEYSTATE_WPARAM(w)&MK_SHIFT) view_.x-=double(delta)*0.8/view_.zoom;
            else view_.y-=double(delta)*0.8/view_.zoom;
            UpdateView(); return 0;
        }
        if(m==WM_VSCROLL || m==WM_HSCROLL) {
            CommitText(); const int bar=m==WM_VSCROLL?SB_VERT:SB_HORZ;
            SCROLLINFO s{sizeof(s),SIF_ALL}; GetScrollInfo(window_,bar,&s); double pos=s.nPos;
            switch(LOWORD(w)) { case SB_LINEUP:pos-=40/view_.zoom;break;case SB_LINEDOWN:pos+=40/view_.zoom;break;case SB_PAGEUP:pos-=s.nPage;break;case SB_PAGEDOWN:pos+=s.nPage;break;case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=s.nTrackPos;break;case SB_TOP:pos=0;break;case SB_BOTTOM:pos=s.nMax;break; }
            (bar==SB_VERT?view_.y:view_.x)=pos; UpdateView(); return 0;
        }
        if(m==WM_KEYDOWN) {
            if(w==VK_ESCAPE) { if(compact_&&tool_==Tool::None&&!text_){SendMessageW(window_,WM_CLOSE,0,0);return 0;}tool_=Tool::None; CancelText();UpdateCompactBar(); }
            if(w==VK_DELETE && selected_>=0 && selected_<int(renderer_.annotations_.size())) {
                Snapshot(); EraseAnnotationGroup(renderer_.annotations_,selected_); selected_=-1; Changed();
            }
            if(GetKeyState(VK_CONTROL)<0 && w=='Z') SendMessageW(window_,WM_COMMAND,110,0);
            if(GetKeyState(VK_CONTROL)<0 && w=='S') SendMessageW(window_,WM_COMMAND,121,0);
            if(GetKeyState(VK_CONTROL)<0 && w=='C') SendMessageW(window_,WM_COMMAND,120,0);
            return 0;
        }
        if(m==WM_COMMAND) {
            const int id=LOWORD(w);
            if(id>=100 && id<=107) { CommitText(); tool_=compact_&&tool_==Tool(id-100)?Tool::None:Tool(id-100);UpdateCompactBar(); for(int i=0;i<8;++i) SendDlgItemMessageW(window_,100+i,BM_SETSTATE,i==id-100,0); return 0; }
            if(id==140) return 0;
            CommitText();
            switch(id) {
            case 110: if(!history_.empty()) { renderer_.annotations_=std::move(history_.back()); history_.pop_back(); Changed(); } break;
            case 111: { static COLORREF custom[16]{}; CHOOSECOLORW c{sizeof(c)}; c.hwndOwner=window_; c.rgbResult=color_; c.lpCustColors=custom; c.Flags=CC_FULLOPEN|CC_RGBINIT; if(ChooseColorW(&c)) color_=c.rgbResult; break; }
            case 120: if(!(compact_?CopyImageFile(window_,Render()):CopyImage(window_,Render()))) MessageBoxW(window_,L"无法复制图像，请稍后重试。",L"PcTool",MB_OK|MB_ICONWARNING); break;
            case 121: { auto path=ChoosePath(window_,false);if(!path.empty()){if(!SavePng(Render(),path))MessageBoxW(window_,L"PNG 保存失败，请检查路径和可用空间。",L"PcTool",MB_OK|MB_ICONERROR);else if(compact_)DestroyWindow(window_);}break; }
            case 127: if(!renderer_.annotations_.empty()){Snapshot();renderer_.annotations_.clear();selected_=-1;Changed();}break;
            case 128: DestroyWindow(window_);break;
            case 129: if(CopyImageFile(window_,Render()))DestroyWindow(window_);else MessageBoxW(window_,L"复制失败，请稍后重试。",L"PcTool",MB_OK|MB_ICONWARNING);break;
            case 122: {
                OcrRequest request; request.image=Render(); request.owner=window_;
                GetClientRect(window_,&request.busyRegion); request.busyRegion.top=Top();
                MapWindowPoints(window_,nullptr,reinterpret_cast<POINT*>(&request.busyRegion),2);
                ocr_.push_back(OpenOcr(std::move(request))); break;
            }
            case 123: TogglePin(); break;
            case 124: Fit(); break;
            case 125: view_.zoom=1; UpdateView(); break;
            }
            return 0;
        }
        if(m==WM_CLOSE) { CommitText(); document_->annotations=renderer_.annotations_; }
        return ToolWindow::Handle(m,w,l);
    }
    std::shared_ptr<ImageDocument> document_;
    ScreenshotOverlay renderer_;
    Viewport view_;
    bool compact_{};RECT captureRegion_{},monitor_{},work_{};CaptureSurround surround_;shared_ui::SlimScrollbar scrollbar_;
    std::unique_ptr<CaptureLabel> dimension_;std::unique_ptr<OverlayToolbar> compactBar_;std::unique_ptr<AnnotationStylePanel> stylePanel_;AnnotationStyle style_;
    Tool tool_{Tool::None};
    COLORREF color_{RGB(255,70,70)};
    bool pinned_{},drawing_{};
    int moving_{-1},editingIndex_{-1},selected_{-1};
    POINT pointer_{};
    Annotation working_,before_,editing_;
    std::vector<std::pair<size_t,Annotation>> beforeGroup_;
    HWND text_{};
    HFONT editorFont_{};
    std::vector<std::vector<Annotation>> history_;
    std::vector<std::unique_ptr<ToolWindow>> ocr_;
};
std::unique_ptr<ToolWindow> OpenImageEditor(std::shared_ptr<ImageDocument> document,std::optional<RECT> captureRegion) { return std::make_unique<ImageEditor>(std::move(document),captureRegion); }
Image RenderDocument(const ImageDocument& document) {
    ScreenshotOverlay renderer(GetModuleHandleW(nullptr));
    renderer.desktopBitmap_=ToBitmap(document.image);
    if(!renderer.desktopBitmap_) throw std::bad_alloc();
    renderer.virtualWidth_=document.image.width; renderer.virtualHeight_=document.image.height;
    renderer.selection_={0,0,document.image.width,document.image.height}; renderer.selectionCommitted_=true;
    renderer.dpi_=document.dpi; renderer.annotations_=document.annotations;
    HBITMAP output=renderer.CreateOutputBitmap();
    if(!output) throw std::bad_alloc();
    try { Image image=FromBitmap(output); DeleteObject(output); return image; }
    catch(...) { DeleteObject(output); throw; }
}
}
