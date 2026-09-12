#include "translation/source_settings.h"
#include "translation/source_provider.h"
#include "translation/source_icons.h"
#include "translation/source_confirmation.h"
#include "shared/ui/settings_frame.h"
#include "shared/ui/toolbar_icons.h"
#include "shared/ui/popup_controls.h"
#include "shared/ui/capture_ui.h"
#include "shared/ui/resource.h"
#include "shared/annotation/text_layout.h"
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <winrt/base.h>
#include <algorithm>
#include <mutex>
#include <optional>
namespace translation {
bool HandleSourceSettingsMessage(MSG& message){
    if(HandleSourceConfirmationMessage(message))return true;
    HWND root=GetAncestor(message.hwnd,GA_ROOT);wchar_t cls[100]{};GetClassNameW(root,cls,100);
    if(wcscmp(cls,L"PcTool.TranslationSettings")&&wcscmp(cls,L"PcTool.TranslationSourcePicker"))return false;
    if(message.message==WM_KEYDOWN&&message.wParam=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){wchar_t child[32]{};GetClassNameW(message.hwnd,child,32);if(_wcsicmp(child,L"Edit")==0){SendMessageW(message.hwnd,EM_SETSEL,0,-1);return true;}}
    if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){PostMessageW(root,WM_CLOSE,0,0);return true;}
    if(wcscmp(cls,L"PcTool.TranslationSourcePicker")==0&&message.message==WM_KEYDOWN&&message.wParam==VK_RETURN){SendMessageW(root,WM_COMMAND,MAKEWPARAM(2210,BN_CLICKED),0);return true;}
    return IsDialogMessageW(root,&message)!=FALSE;
}
namespace {
constexpr int Name=2101,Address=2102,Key=2103,Model=2104,Region=2105,AppId=2106,Type=2107,Verify=2108,Add=2109,Remove=2110,Reveal=2111,List=2112,Bar=2113;
struct Label {int id;const wchar_t* text;};
const Label labels[]={{Name,L"名称"},{Address,L"API 基础地址"},{Key,L"密钥"},{Model,L"模型名称"},{Region,L"区域"},{AppId,L"App ID"},{Type,L"DeepL 类型"}};
bool SameConnection(const SourceConfig& a,const SourceConfig& b){return a.endpoint==b.endpoint&&a.key==b.key&&a.model==b.model&&a.region==b.region&&a.appId==b.appId&&a.pro==b.pro;}
void Round(Gdiplus::Graphics& g,const RECT& r,float radius,Gdiplus::Color color){Gdiplus::GraphicsPath p;capture::text_layout::AddRoundedRectangle(p,{float(r.left),float(r.top),float(r.right-r.left),float(r.bottom-r.top)},radius);Gdiplus::SolidBrush b(color);g.FillPath(&b,&p);}
}
struct SourceSettings::Impl {
    SourceStore& store;std::vector<SourceConfig>& sources;std::function<void()> changed;
    HWND window{},list{},picker{},revealTip{};HFONT font{};HBRUSH background{CreateSolidBrush(RGB(247,247,247))},fieldBrush{CreateSolidBrush(RGB(255,255,255))};UINT dpi{96};
    shared_ui::SlimScrollbar scroll;int offset{},selected{-1},hover{-1},drag{-1};POINT down{};bool dragging{},populating{},revealed{},busy{};
    SourceConfig draft;std::wstring status;std::vector<std::pair<int,RECT>> fields;
    int pickerSelection{-1};UINT pickerDpi{96};HFONT pickerFont{};
    shared_ui::PopupShadow pickerShadow{L"PcTool.TranslationSourcePickerShadow"};
    shared_ui::SettingsFrame frame;
    uint64_t generation{};std::shared_ptr<capture::Cancellation> cancel;std::shared_ptr<TranslationProvider> provider;
    struct Reply {uint64_t generation;TranslationResult result;};struct Mailbox {std::mutex mutex;std::optional<Reply> reply;};std::shared_ptr<Mailbox> mailbox=std::make_shared<Mailbox>();
    Impl(SourceStore& s,std::vector<SourceConfig>& c,std::function<void()> cb):store(s),sources(c),changed(std::move(cb)){}
    ~Impl(){Close();DeleteObject(font);DeleteObject(pickerFont);DeleteObject(background);DeleteObject(fieldBrush);}
    int D(int n)const{return MulDiv(n,dpi,96);}HWND Control(int id)const{return GetDlgItem(window,id);}
    void Error(const std::wstring& text){MessageBoxW(window,text.c_str(),L"PcTool · 翻译源设置",MB_OK|MB_ICONWARNING);}
    void Persist(std::vector<SourceConfig> next){store.Save(next);sources=std::move(next);changed();InvalidateRect(list,nullptr,FALSE);}
    bool Selected()const{return selected>=0&&selected<int(sources.size());}
    void Read(){if(!Selected()||populating)return;draft.name=capture::WindowText(Control(Name));draft.endpoint=capture::WindowText(Control(Address));draft.key=capture::WindowText(Control(Key));draft.model=capture::WindowText(Control(Model));draft.region=capture::WindowText(Control(Region));draft.appId=capture::WindowText(Control(AppId));draft.pro=SendMessageW(Control(Type),CB_GETCURSEL,0,0)==1;}
    bool Dirty(){Read();return Selected()&&!SameConnection(draft,sources[selected]);}
    void Cancel(){++generation;if(cancel)cancel->requested=true;cancel.reset();busy=false;}
    bool Leave(){
        const bool dirty=Dirty(),wasBusy=busy;Cancel();
        if(wasBusy){status=L"验证已取消";Layout();}
        if(!dirty)return true;
        if(!ConfirmSourceAction(window,L"放弃未保存的修改？",L"连接参数尚未验证保存。\n放弃后将恢复上次保存的配置。",L"放弃修改"))return false;
        // Roll back both the draft and EDIT contents. Later actions call Dirty()
        // again, so acknowledging the prompt alone would immediately re-prompt.
        // The action that raised this prompt is consumed, including WM_CLOSE.
        // A fresh user click may proceed after the form has been restored.
        Populate();return false;
    }
    void Select(int index){if(index==selected)return;if(!Leave())return;selected=index;Populate();}
    void Populate(){populating=true;draft=Selected()?sources[selected]:SourceConfig{};revealed=false;
        for(auto [id,value]:std::initializer_list<std::pair<int,std::wstring>>{{Name,draft.name},{Address,draft.endpoint},{Key,draft.key},{Model,draft.model},{Region,draft.region},{AppId,draft.appId}})SetWindowTextW(Control(id),value.c_str());
        SendMessageW(Control(Key),EM_SETPASSWORDCHAR,0x25CF,0);SendMessageW(Control(Type),CB_SETCURSEL,draft.pro?1:0,0);
        UpdateRevealHint();
        status=Selected()?(draft.verified?L"验证成功，配置已保存":L"待验证，验证成功后保存并启用"):L"点击 ＋ 添加翻译源";populating=false;Layout();InvalidateRect(list,nullptr,FALSE);
    }
    void Enable(bool enabled){if(!Selected())return;if(enabled&&!sources[selected].verified){status=L"请先填写参数并验证翻译源";InvalidateRect(window,nullptr,FALSE);return;}auto next=sources;next[selected].enabled=enabled;Persist(std::move(next));}
    void UpdateRevealHint(){
        const wchar_t* hint=revealed?L"隐藏密钥":L"显示密钥";SetWindowTextW(Control(Reveal),hint);
        if(revealTip){TOOLINFOW tool{sizeof(tool)};tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(Control(Reveal));tool.lpszText=const_cast<wchar_t*>(hint);SendMessageW(revealTip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));}
        InvalidateRect(Control(Reveal),nullptr,FALSE);
    }
    static LRESULT CALLBACK ButtonProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_MOUSEMOVE&&!GetPropW(w,L"SourceButtonHot")){SetPropW(w,L"SourceButtonHot",HANDLE(1));TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);InvalidateRect(w,nullptr,FALSE);}
        if(m==WM_MOUSELEAVE){RemovePropW(w,L"SourceButtonHot");InvalidateRect(w,nullptr,FALSE);}
        if(m==WM_NCDESTROY){RemovePropW(w,L"SourceButtonHot");RemoveWindowSubclass(w,ButtonProc,1);}return DefSubclassProc(w,m,wp,lp);
    }
    void AddSource(SourceKind kind){if(!Leave())return;auto next=sources;next.push_back(NewSource(kind));Persist(std::move(next));selected=int(sources.size())-1;offset=std::max(0,(selected-2)*D(64));Populate();SetFocus(Control(Name));}
    void Delete(){if(!Selected()||!Leave())return;if(!ConfirmSourceAction(window,L"删除翻译源？",L"删除“"+sources[selected].name+L"”后，需要重新添加并验证才能使用。",L"删除",L"取消"))return;auto next=sources;next.erase(next.begin()+selected);Persist(std::move(next));selected=std::min(selected,int(sources.size())-1);Populate();}
    void StartVerify(){
        if(busy){Cancel();status=L"验证已取消";Layout();return;}if(!Selected())return;Read();const auto error=ValidateConfig(draft);if(!error.empty()){status=error;InvalidateRect(window,nullptr,FALSE);return;}
        Cancel();busy=true;status=L"正在验证…";cancel=std::make_shared<capture::Cancellation>();provider=MakeSourceProvider(draft);const auto token=generation;auto box=mailbox;
        provider->Verify(cancel,[box,token](TranslationResult result){std::lock_guard<std::mutex> lock(box->mutex);box->reply=Reply{token,std::move(result)};});Layout();
    }
    void Poll(){std::optional<Reply> reply;{std::lock_guard<std::mutex> lock(mailbox->mutex);reply.swap(mailbox->reply);}if(!reply||reply->generation!=generation||!busy)return;
        busy=false;if(reply->result.error.empty()&&!reply->result.text.empty()&&Selected()){
            auto next=sources;draft.verified=true;draft.enabled=sources[selected].verified?sources[selected].enabled:true;next[selected]=draft;
            try{Persist(std::move(next));status=L"验证成功，配置已保存";}catch(const winrt::hresult_error& e){status=L"保存失败："+std::wstring(e.message());}
        }else status=reply->result.error.empty()?L"翻译源未返回内容":reply->result.error;
        Layout();
    }
    void Create(HWND owner){
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PcTool.TranslationSettings";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
        POINT p{};GetCursorPos(&p);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST),&mi);
        window=CreateWindowExW(WS_EX_APPWINDOW,wc.lpszClassName,L"翻译源设置 · PcTool",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,mi.rcWork.left+20,mi.rcWork.top+20,1000,680,owner,nullptr,wc.hInstance,this);
        if(!window)winrt::throw_last_error();frame.Attach(window);dpi=GetDpiForWindow(window);if(font)DeleteObject(font);font=CreateFontW(-D(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        WNDCLASSW lc{};lc.lpfnWndProc=ListProc;lc.hInstance=wc.hInstance;lc.lpszClassName=L"PcTool.TranslationSourceList";lc.hCursor=wc.hCursor;RegisterClassW(&lc);
        list=CreateWindowExW(0,lc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,1,1,window,HMENU(List),wc.hInstance,this);
        scroll.Create(window,Bar,[this](int value){offset=value;InvalidateRect(list,nullptr,FALSE);},RGB(247,247,247));
        auto control=[&](int id,const wchar_t* cls,const wchar_t* title,DWORD style){HWND w=CreateWindowExW(0,cls,title,WS_CHILD|WS_TABSTOP|style,0,0,1,1,window,HMENU(INT_PTR(id)),wc.hInstance,nullptr);SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);if(wcscmp(cls,L"BUTTON")==0)SetWindowSubclass(w,ButtonProc,1,0);return w;};
        for(const auto& label:labels)if(label.id!=Type)control(label.id,L"EDIT",L"",ES_AUTOHSCROLL);
        control(Type,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL);SendMessageW(Control(Type),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Free 免费版"));SendMessageW(Control(Type),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Pro 专业版"));
        control(Verify,L"BUTTON",L"验证并保存",BS_OWNERDRAW);control(Add,L"BUTTON",L"＋",BS_OWNERDRAW);control(Remove,L"BUTTON",L"−",BS_OWNERDRAW);control(Reveal,L"BUTTON",L"显示",BS_OWNERDRAW);
        revealTip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,wc.hInstance,nullptr);
        TOOLINFOW tip{sizeof(tip)};tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=window;tip.uId=reinterpret_cast<UINT_PTR>(Control(Reveal));tip.lpszText=const_cast<wchar_t*>(L"显示密钥");SendMessageW(revealTip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        for(int id:{Add,Remove})ShowWindow(Control(id),SW_SHOW);
        selected=sources.empty()?-1:0;Populate();SetTimer(window,1,50,nullptr);
        const int width=std::min(D(1040),int(mi.rcWork.right-mi.rcWork.left)-D(24)),height=std::min(D(690),int(mi.rcWork.bottom-mi.rcWork.top)-D(24));
        SetWindowPos(window,nullptr,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-width)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void Show(HWND owner){if(!window)Create(owner);ShowWindow(window,IsIconic(window)?SW_RESTORE:SW_SHOW);SetForegroundWindow(window);}
    void Close(){Cancel();if(picker)DestroyWindow(picker);picker=nullptr;if(window){KillTimer(window,1);DestroyWindow(window);window=nullptr;list=nullptr;revealTip=nullptr;}scroll=shared_ui::SlimScrollbar{};}
    void Layout(){if(!window||!list)return;RECT c{};GetClientRect(window,&c);const int sidebar=D(145),left=sidebar+D(18),listWidth=D(235),right=left+listWidth+D(18),formWidth=std::max(D(160),int(c.right)-right-D(22));
        MoveWindow(list,left,D(60),listWidth,std::max(D(40),int(c.bottom)-D(128)),FALSE);RECT lr{};GetClientRect(list,&lr);offset=std::clamp(offset,0,std::max(0,int(sources.size())*D(64)-int(lr.bottom)));scroll.Update(int(sources.size())*D(64),lr.bottom,offset,dpi);MoveWindow(scroll.Window(),left+listWidth-D(7),D(62),D(6),std::max(1,int(lr.bottom)-D(4)),FALSE);
        MoveWindow(Control(Add),left,c.bottom-D(55),D(42),D(32),FALSE);MoveWindow(Control(Remove),left+D(52),c.bottom-D(55),D(42),D(32),FALSE);EnableWindow(Control(Remove),Selected()&&!busy);
        fields.clear();int y=D(86);for(const auto& label:labels){bool visible=Selected();if(label.id==Address||label.id==Model)visible=visible&&draft.kind==SourceKind::OpenAI;if(label.id==Region)visible=visible&&draft.kind==SourceKind::Azure;if(label.id==AppId)visible=visible&&draft.kind==SourceKind::Baidu;if(label.id==Type)visible=visible&&draft.kind==SourceKind::DeepL;
            ShowWindow(Control(label.id),visible?SW_SHOW:SW_HIDE);EnableWindow(Control(label.id),!busy);if(!visible)continue;
            const int caption=D(105);RECT r{right+D(20),y,right+formWidth-D(20),y+D(36)};fields.push_back({label.id,r});const bool combo=label.id==Type;MoveWindow(Control(label.id),r.left+caption+(combo?0:D(10)),r.top+(combo?D(4):D(8)),std::max(D(50),int(r.right-r.left)-caption-(combo?0:D(20))-(label.id==Key?D(46):0)),combo?D(160):D(22),FALSE);
            if(label.id==Key)MoveWindow(Control(Reveal),r.right-D(42),r.top,D(42),D(36),FALSE);y+=D(56);
        }
        ShowWindow(Control(Reveal),Selected()?SW_SHOW:SW_HIDE);EnableWindow(Control(Reveal),!busy);ShowWindow(Control(Verify),Selected()?SW_SHOW:SW_HIDE);MoveWindow(Control(Verify),right+D(20),y+D(14),D(130),D(36),FALSE);SetWindowTextW(Control(Verify),busy?L"取消验证":L"验证并保存");
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);InvalidateRect(list,nullptr,FALSE);
    }
    void Paint(HWND target,HDC out,bool listOnly){RECT c{};GetClientRect(target,&c);HDC dc=CreateCompatibleDC(out);HBITMAP b=CreateCompatibleBitmap(out,c.right,c.bottom);auto old=SelectObject(dc,b);{
        Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.Clear(Gdiplus::Color(255,247,247,247));
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);SetTextColor(dc,RGB(65,68,73));
        auto text=[&](const std::wstring& s,RECT r,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS){DrawTextW(dc,s.c_str(),-1,&r,flags);};
        if(listOnly){Round(g,c,float(D(9)),Gdiplus::Color(255,237,237,237));
            if(sources.empty())text(L"尚未添加翻译源",{D(18),D(18),c.right-D(10),D(64)});
            for(int i=0;i<int(sources.size());++i){RECT r{D(6),i*D(64)-offset+D(4),c.right-D(10),(i+1)*D(64)-offset-D(4)};if(r.bottom<0||r.top>c.bottom)continue;
                if(i==selected||i==hover)Round(g,r,float(D(8)),Gdiplus::Color(255,i==selected?216:228,i==selected?216:228,i==selected?216:228));
                RECT badge{r.left+D(8),r.top+D(12),r.left+D(40),r.top+D(44)};Round(g,badge,float(D(6)),Gdiplus::Color(255,255,255,255));RECT icon=badge;InflateRect(&icon,-D(5),-D(5));DrawSourceIcon(dc,sources[i].kind,icon);
                text(sources[i].name,{r.left+D(48),r.top,r.right-D(55),r.bottom});RECT toggle{r.right-D(47),r.top+D(16),r.right-D(9),r.top+D(38)};Round(g,toggle,float(D(11)),sources[i].enabled?Gdiplus::Color(255,103,194,58):Gdiplus::Color(255,210,213,220));
                const int x=sources[i].enabled?toggle.right-D(20):toggle.left+D(3);Gdiplus::SolidBrush white(Gdiplus::Color::White);g.FillEllipse(&white,float(x),float(toggle.top+D(3)),float(D(16)),float(D(16)));
            }
        }else{
            SetTextColor(dc,RGB(103,194,58));text(L"翻译源设置",{D(20),D(50),D(145),D(90)});SetTextColor(dc,RGB(65,68,73));Gdiplus::Pen line(Gdiplus::Color(255,222,225,230));g.DrawLine(&line,D(145),D(24),D(145),c.bottom-D(24));
            text(L"已添加的翻译源",{D(166),D(15),D(390),D(50)});
            const int right=D(416);RECT card{right,D(60),c.right-D(22),c.bottom-D(22)};Round(g,card,float(D(9)),Gdiplus::Color(255,237,237,237));
            if(!Selected())text(L"添加翻译源后，在这里填写并验证配置",{right+D(20),D(100),c.right-D(40),D(175)},DT_WORDBREAK);
            else{for(const auto& [id,r]:fields){auto found=std::find_if(std::begin(labels),std::end(labels),[&](const auto& l){return l.id==id;});RECT label=r;label.right=label.left+D(100);text(found->text,label);if(id!=Type){RECT input{r.left+D(105),r.top,r.right-(id==Key?D(46):0),r.bottom};Round(g,input,float(D(5)),Gdiplus::Color::White);}}
                RECT button{};GetWindowRect(Control(Verify),&button);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2);text(L"验证短句：你好，世界。",{right+D(20),button.bottom+D(12),c.right-D(42),button.bottom+D(42)});
                SetTextColor(dc,busy?RGB(96,98,102):RGB(87,139,49));text(status,{right+D(20),button.bottom+D(48),c.right-D(42),c.bottom-D(36)},DT_WORDBREAK);
            }
        }
    }BitBlt(out,0,0,c.right,c.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(b);DeleteDC(dc);}
    int P(int n)const{return MulDiv(n,pickerDpi,96);}
    void PickerFonts(){
        if(pickerFont)DeleteObject(pickerFont);
        pickerFont=CreateFontW(-P(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        EnumChildWindows(picker,[](HWND child,LPARAM font)->BOOL{SendMessageW(child,WM_SETFONT,WPARAM(font),FALSE);return TRUE;},reinterpret_cast<LPARAM>(pickerFont));
    }
    void PickerLayout(){
        if(!picker)return;RECT r{};GetClientRect(picker,&r);
        const int margin=P(24),gap=P(16),width=(r.right-2*margin-gap)/2;
        const int rowHeight=(r.bottom-P(88)-P(92))/2;
        for(int i=0;i<4;++i)MoveWindow(GetDlgItem(picker,2200+i),margin+(i%2)*(width+gap),P(88)+(i/2)*(rowHeight+P(12)),width,rowHeight,FALSE);
        MoveWindow(GetDlgItem(picker,2210),r.right-P(206),r.bottom-P(52),P(86),P(34),FALSE);
        MoveWindow(GetDlgItem(picker,2211),r.right-P(108),r.bottom-P(52),P(84),P(34),FALSE);
        MoveWindow(GetDlgItem(picker,2212),r.right-P(48),P(16),P(30),P(30),FALSE);
        SetWindowRgn(picker,CreateRoundRectRgn(0,0,r.right+1,r.bottom+1,P(20),P(20)),FALSE);
        RedrawWindow(picker,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
        pickerShadow.Sync(picker,pickerDpi);
    }
    void PickerPaint(HDC target){
        RECT r{};GetClientRect(picker,&r);HDC dc=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,r.right,r.bottom);auto old=SelectObject(dc,bitmap);
        {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.Clear(Gdiplus::Color(255,247,248,250));
            Gdiplus::SolidBrush white(Gdiplus::Color(255,255,255,255));g.FillRectangle(&white,0,r.bottom-P(70),r.right,P(70));
            Gdiplus::Pen separator(Gdiplus::Color(255,232,234,237));g.DrawLine(&separator,0,r.bottom-P(70),r.right,r.bottom-P(70));
            Gdiplus::FontFamily family(L"Microsoft YaHei UI");Gdiplus::Font title(&family,float(P(20)),Gdiplus::FontStyleBold,Gdiplus::UnitPixel);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255,48,51,56));g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            g.DrawString(L"添加翻译源",-1,&title,Gdiplus::PointF(float(P(24)),float(P(20))),&ink);
            SelectObject(dc,pickerFont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(130,135,143));RECT subtitle{P(24),P(54),r.right-P(24),P(77)};
            DrawTextW(dc,L"选择翻译服务，添加后填写连接信息",-1,&subtitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            RECT hint{P(24),r.bottom-P(52),r.right-P(225),r.bottom-P(18)};
            DrawTextW(dc,pickerSelection<0?L"请选择一个翻译源":SourceName(SourceKind(pickerSelection)),-1,&hint,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    void PickerButton(DRAWITEMSTRUCT& d){
        HDC dc=CreateCompatibleDC(d.hDC);HBITMAP bitmap=CreateCompatibleBitmap(d.hDC,d.rcItem.right,d.rcItem.bottom);auto old=SelectObject(dc,bitmap);
        {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const bool card=d.CtlID>=2200&&d.CtlID<2204,hot=GetPropW(d.hwndItem,L"SourceButtonHot")!=nullptr,pressed=(d.itemState&ODS_SELECTED)!=0;
            const bool chosen=card&&int(d.CtlID-2200)==pickerSelection,disabled=(d.itemState&ODS_DISABLED)!=0;
            g.Clear(card||d.CtlID==2212?Gdiplus::Color(255,247,248,250):Gdiplus::Color(255,255,255,255));
            RECT r=d.rcItem;InflateRect(&r,-P(1),-P(1));
            const auto fill=card?(chosen?Gdiplus::Color(255,240,248,235):hot||pressed?Gdiplus::Color(255,229,233,238):Gdiplus::Color(255,237,239,242)):
                d.CtlID==2210?(disabled?Gdiplus::Color(255,190,222,172):pressed?Gdiplus::Color(255,75,155,39):hot?Gdiplus::Color(255,86,174,45):Gdiplus::Color(255,103,194,58)):
                hot?Gdiplus::Color(255,237,239,242):d.CtlID==2212?Gdiplus::Color(255,247,248,250):Gdiplus::Color(255,255,255,255);
            Round(g,r,float(P(card?10:6)),fill);
            if(chosen||(!card&&d.CtlID==2211)||(d.itemState&ODS_FOCUS)){
                Gdiplus::GraphicsPath path;capture::text_layout::AddRoundedRectangle(path,{float(r.left),float(r.top),float(r.right-r.left),float(r.bottom-r.top)},float(P(card?10:6)));
                Gdiplus::Pen border(chosen?Gdiplus::Color(255,103,194,58):(d.itemState&ODS_FOCUS)?Gdiplus::Color(255,175,185,199):Gdiplus::Color(255,219,223,229),float(P(1)));g.DrawPath(&border,&path);
            }
            SelectObject(dc,pickerFont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(48,51,56));
            if(card){
                const auto kind=SourceKind(d.CtlID-2200);const int middle=(r.top+r.bottom)/2;RECT badge{P(17),middle-P(22),P(61),middle+P(22)};
                Round(g,badge,float(P(10)),Gdiplus::Color(255,255,255,255));RECT icon=badge;InflateRect(&icon,-P(7),-P(7));DrawSourceIcon(dc,kind,icon);
                RECT label{P(75),middle-P(24),r.right-P(28),middle+P(1)};DrawTextW(dc,SourceName(kind),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
                const wchar_t* details[]={L"自定义模型与 API 地址",L"Free / Pro",L"Microsoft Translator",L"通用翻译 API"};
                SetTextColor(dc,RGB(139,144,152));label.top=middle+P(3);label.bottom=middle+P(26);DrawTextW(dc,details[int(kind)],-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
                if(chosen){RECT check{r.right-P(22),P(9),r.right-P(7),P(24)};Round(g,check,float(P(8)),Gdiplus::Color(255,103,194,58));Gdiplus::Pen tick(Gdiplus::Color(255,255,255,255),1.5F*pickerDpi/96.0F);tick.SetStartCap(Gdiplus::LineCapRound);tick.SetEndCap(Gdiplus::LineCapRound);tick.SetLineJoin(Gdiplus::LineJoinRound);Gdiplus::PointF points[]={{float(check.left+P(4)),float(check.top+P(7))},{float(check.left+P(6)),float(check.top+P(10))},{float(check.left+P(11)),float(check.top+P(5))}};g.DrawLines(&tick,points,3);}
            }else if(d.CtlID==2212)capture::DrawToolbarIcon(dc,capture::ToolbarIcon::Cancel,r,pickerDpi,RGB(125,131,140),11);
            else{SetTextColor(dc,d.CtlID==2210?RGB(255,255,255):RGB(96,98,102));DrawTextW(dc,d.CtlID==2210?L"添加":L"关闭",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        }
        BitBlt(d.hDC,0,0,d.rcItem.right,d.rcItem.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    void Pick(){
        if(picker){SetForegroundWindow(picker);return;}if(!Leave())return;
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=PickerProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PcTool.TranslationSourcePicker";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
        pickerDpi=GetDpiForWindow(window);pickerSelection=0;RECT parent{};GetWindowRect(window,&parent);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);
        const int width=std::min(P(640),int(mi.rcWork.right-mi.rcWork.left)-P(28)),height=std::min(P(360),int(mi.rcWork.bottom-mi.rcWork.top)-P(28));
        const int x=std::clamp(int(parent.left+(parent.right-parent.left-width)/2),int(mi.rcWork.left)+P(14),int(mi.rcWork.right)-P(14)-width);
        const int y=std::clamp(int(parent.top+(parent.bottom-parent.top-height)/2),int(mi.rcWork.top)+P(14),int(mi.rcWork.bottom)-P(14)-height);
        picker=CreateWindowExW(0,wc.lpszClassName,L"添加翻译源",WS_POPUP|WS_CLIPCHILDREN,x,y,width,height,window,nullptr,wc.hInstance,this);
        if(!picker)winrt::throw_last_error();
        for(int id:{2200,2201,2202,2203,2210,2211,2212}){
            const wchar_t* label=id<2204?SourceName(SourceKind(id-2200)):id==2210?L"添加":L"关闭";
            HWND b=CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,picker,HMENU(INT_PTR(id)),wc.hInstance,nullptr);SetWindowSubclass(b,ButtonProc,1,0);
        }
        PickerFonts();EnableWindow(GetDlgItem(picker,2210),TRUE);PickerLayout();EnableWindow(window,FALSE);ShowWindow(picker,SW_SHOW);pickerShadow.Sync(picker,pickerDpi);SetForegroundWindow(picker);SetFocus(GetDlgItem(picker,2200));
    }
    void DrawButton(DRAWITEMSTRUCT& d){
        HDC dc=CreateCompatibleDC(d.hDC);HBITMAP bitmap=CreateCompatibleBitmap(d.hDC,std::max(1L,d.rcItem.right),std::max(1L,d.rcItem.bottom));auto old=SelectObject(dc,bitmap);
        {
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const bool green=d.CtlID==Verify,eye=d.CtlID==Reveal,hot=GetPropW(d.hwndItem,L"SourceButtonHot")!=nullptr;
            // Match the actual parent card at the rounded corners, not the page.
            const BYTE background=green||eye?237:247;g.Clear(Gdiplus::Color(255,background,background,background));
            const BYTE gray=(d.itemState&ODS_SELECTED)?215:hot?222:232;
            if(!eye||hot||(d.itemState&ODS_SELECTED))Round(g,d.rcItem,float(D(6)),green?(hot?Gdiplus::Color(255,86,171,46):Gdiplus::Color(255,103,194,58)):Gdiplus::Color(255,gray,gray,gray));
            const COLORREF ink=(d.itemState&ODS_DISABLED)?RGB(170,170,170):green?RGB(255,255,255):RGB(96,98,102);
            if(eye){const int x=(d.rcItem.left+d.rcItem.right)/2,y=(d.rcItem.top+d.rcItem.bottom)/2;RECT icon{x-D(10),y-D(10),x+D(10),y+D(10)};DrawVisibilityIcon(dc,!revealed,icon,ink);}
            else{SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ink);SelectObject(dc,font);auto r=d.rcItem;const auto label=capture::WindowText(d.hwndItem);DrawTextW(dc,label.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        }
        BitBlt(d.hDC,0,0,d.rcItem.right,d.rcItem.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    static LRESULT CALLBACK PickerProc(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);self->PickerPaint(dc);EndPaint(w,&ps);return 0;}
        if(m==WM_DRAWITEM){self->PickerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;}
        if(m==WM_NCHITTEST){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&p);RECT r{};GetClientRect(w,&r);return p.y<self->P(50)&&p.x<r.right-self->P(52)?HTCAPTION:HTCLIENT;}
        if(m==WM_WINDOWPOSCHANGED){self->pickerShadow.Sync(w,self->pickerDpi);}
        if(m==WM_DPICHANGED){self->pickerDpi=HIWORD(wp);self->PickerFonts();auto r=*reinterpret_cast<RECT*>(lp);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&mi);const int width=std::min(int(r.right-r.left),int(mi.rcWork.right-mi.rcWork.left)-self->P(28)),height=std::min(int(r.bottom-r.top),int(mi.rcWork.bottom-mi.rcWork.top)-self->P(28));r.left=std::clamp(int(r.left),int(mi.rcWork.left)+self->P(14),int(mi.rcWork.right)-self->P(14)-width);r.top=std::clamp(int(r.top),int(mi.rcWork.top)+self->P(14),int(mi.rcWork.bottom)-self->P(14)-height);SetWindowPos(w,nullptr,r.left,r.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);self->PickerLayout();return 0;}
        if(m==WM_COMMAND&&HIWORD(wp)==BN_CLICKED){
            const int id=LOWORD(wp);
            if(id>=2200&&id<2204){self->pickerSelection=id-2200;EnableWindow(GetDlgItem(w,2210),TRUE);RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);return 0;}
            if(id==2210){if(self->pickerSelection<0)return 0;const auto kind=SourceKind(self->pickerSelection);DestroyWindow(w);try{self->AddSource(kind);}catch(const winrt::hresult_error& e){self->Error(e.message().c_str());}return 0;}
            if(id==2211||id==2212){DestroyWindow(w);return 0;}
        }
        if(m==WM_CLOSE){DestroyWindow(w);return 0;}
        if(m==WM_DESTROY){self->pickerShadow.Close();self->picker=nullptr;EnableWindow(self->window,TRUE);SetForegroundWindow(self->window);SetFocus(self->Control(Add));return 0;}return DefWindowProcW(w,m,wp,lp);
    }
    static LRESULT CALLBACK ListProc(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,m,wp,lp);
        try{
            if(m==WM_ERASEBKGND)return 1;if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(w,dc,true);EndPaint(w,&ps);return 0;}
            if(m==WM_MOUSEWHEEL){self->offset-=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*self->D(64);self->Layout();return 0;}
            if(m==WM_MOUSELEAVE){self->hover=-1;InvalidateRect(w,nullptr,FALSE);return 0;}
            if(m==WM_MOUSEMOVE){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,w,0};TrackMouseEvent(&track);}
            if(self->busy&&(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP))return 0;
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};int index=(p.y+self->offset)/self->D(64);if(index<0||index>=int(self->sources.size()))index=-1;
            if(m==WM_LBUTTONDOWN&&index>=0){RECT r{};GetClientRect(w,&r);if(p.x>r.right-self->D(65)){self->Select(index);if(self->selected==index)self->Enable(!self->sources[index].enabled);return 0;}self->Select(index);if(self->selected==index){self->drag=index;self->down=p;SetCapture(w);}return 0;}
            if(m==WM_MOUSEMOVE){if(self->drag>=0&&(wp&MK_LBUTTON)){if(abs(p.y-self->down.y)>self->D(5))self->dragging=true;RECT r{};GetClientRect(w,&r);if(p.y<self->D(12))self->offset-=self->D(16);if(p.y>r.bottom-self->D(12))self->offset+=self->D(16);self->Layout();}else if(self->hover!=index){self->hover=index;InvalidateRect(w,nullptr,FALSE);}return 0;}
            if(m==WM_LBUTTONUP){const int from=self->drag;const bool moving=self->dragging;self->drag=-1;self->dragging=false;if(GetCapture()==w)ReleaseCapture();if(moving&&from>=0&&index>=0&&index!=from){auto next=self->sources;auto value=next[from];next.erase(next.begin()+from);next.insert(next.begin()+index,std::move(value));self->Persist(std::move(next));self->selected=index;self->Populate();}return 0;}
            if(m==WM_CAPTURECHANGED||m==WM_CANCELMODE){self->drag=-1;self->dragging=false;}
        }catch(const winrt::hresult_error& e){self->Error(e.message().c_str());}catch(...){self->Error(L"无法更新翻译源配置");}return DefWindowProcW(w,m,wp,lp);
    }
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,m,wp,lp);
        try{
            switch(m){case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(w,dc,false);EndPaint(w,&ps);return 0;}
            case WM_SIZE:self->Layout();return 0;case WM_GETMINMAXINFO:{auto* mm=reinterpret_cast<MINMAXINFO*>(lp);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi);mm->ptMinTrackSize={std::min(self->D(820),int(mi.rcWork.right-mi.rcWork.left)),std::min(self->D(520),int(mi.rcWork.bottom-mi.rcWork.top))};return 0;}
            case WM_DPICHANGED:{self->dpi=HIWORD(wp);DeleteObject(self->font);self->font=CreateFontW(-self->D(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");EnumChildWindows(w,[](HWND c,LPARAM p)->BOOL{SendMessageW(c,WM_SETFONT,WPARAM(p),FALSE);return TRUE;},reinterpret_cast<LPARAM>(self->font));auto r=*reinterpret_cast<RECT*>(lp);SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();return 0;}
            case WM_TIMER:self->Poll();return 0;
            case WM_DRAWITEM:self->DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;
            case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(wp),RGB(96,98,102));SetBkColor(reinterpret_cast<HDC>(wp),RGB(255,255,255));return reinterpret_cast<LRESULT>(self->fieldBrush);
            case WM_COMMAND:{const int id=LOWORD(wp);if(HIWORD(wp)==EN_CHANGE&&!self->populating&&self->Selected()){
                    self->Read();if(id==Name){auto next=self->sources;next[self->selected].name=self->draft.name;self->Persist(std::move(next));}else{self->status=L"参数已修改，请重新验证后保存";InvalidateRect(w,nullptr,FALSE);}return 0;}
                if(HIWORD(wp)==CBN_SELCHANGE){self->Read();self->status=L"参数已修改，请重新验证后保存";InvalidateRect(w,nullptr,FALSE);return 0;}
                if(HIWORD(wp)==BN_CLICKED){if(id==Add)self->Pick();if(id==Remove)self->Delete();if(id==Verify)self->StartVerify();if(id==Reveal){self->revealed=!self->revealed;SendMessageW(self->Control(Key),EM_SETPASSWORDCHAR,self->revealed?0:0x25CF,0);self->UpdateRevealHint();InvalidateRect(self->Control(Key),nullptr,FALSE);}return 0;}break;}
            case WM_CLOSE:if(self->Leave())self->Close();return 0;
            }
        }catch(const winrt::hresult_error& e){self->Error(e.message().c_str());}catch(...){self->Error(L"翻译源操作失败，请重试");}return DefWindowProcW(w,m,wp,lp);
    }
};
SourceSettings::SourceSettings(SourceStore& store,std::vector<SourceConfig>& sources,std::function<void()> changed):impl_(std::make_unique<Impl>(store,sources,std::move(changed))){}
SourceSettings::~SourceSettings()=default;
void SourceSettings::Show(HWND owner){impl_->Show(owner);}void SourceSettings::Close(){impl_->Close();}
}
