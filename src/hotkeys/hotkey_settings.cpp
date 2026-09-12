#include "hotkeys/hotkey_settings.h"
#include "shared/ui/settings_frame.h"
#include "shared/ui/confirmation.h"
#include "shared/ui/resource.h"
#include "shared/annotation/text_layout.h"
#include <windowsx.h>
#include <gdiplus.h>
namespace hotkeys { namespace {
constexpr const wchar_t* Descriptions[]={L"框选屏幕，进行截图、标注或录屏",L"查看最近复制的文本内容",L"打开翻译窗口并输入文字",L"框选屏幕，通过文字识别提取并翻译",L"复制原应用中的选中文字并翻译",L"打开 Windows 任务管理器",L"打开系统画图工具",L"打开内置 7-Zip 文件管理器",L"查看和修改全局快捷键"};
void Round(Gdiplus::Graphics& g,RECT r,float radius,Gdiplus::Color fill,Gdiplus::Color line){Gdiplus::GraphicsPath p;capture::text_layout::AddRoundedRectangle(p,{float(r.left),float(r.top),float(r.right-r.left-1),float(r.bottom-r.top-1)},radius);Gdiplus::SolidBrush b(fill);g.FillPath(&b,&p);Gdiplus::Pen pen(line,1);g.DrawPath(&pen,&p);}
struct Editor {
    HWND window{},viewport{},fields[9]{},reset{},saveButton{};HFONT font{};UINT dpi{96};int offset{},page{},errorIndex{-1},wheel{};std::wstring error;Bindings initial,draft;Binding beforeFocus{};
    const std::function<bool(const Bindings&,std::wstring&)>& save;
    std::function<void(bool)> captureChanged;bool capturing{};
    shared_ui::SettingsFrame frame;shared_ui::SlimScrollbar scroll;
    Editor(const Bindings& values,const std::function<bool(const Bindings&,std::wstring&)>& callback,const std::function<void(bool)>& capture):initial(values),draft(values),save(callback),captureChanged(capture){}
    ~Editor(){CaptureKeys(false);if(font)DeleteObject(font);}
    void CaptureKeys(bool value){if(capturing==value)return;capturing=value;if(captureChanged)captureChanged(value);}
    int D(int n)const{return MulDiv(n,dpi,96);}
    void Font(){if(font)DeleteObject(font);font=CreateFontW(-D(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");}
    void Set(size_t index,Binding binding){draft[index]=binding;error.clear();errorIndex=-1;SetWindowTextW(fields[index],Text(binding).c_str());InvalidateRect(fields[index],nullptr,FALSE);InvalidateRect(viewport,nullptr,FALSE);}
    void Fill(const Bindings& values){for(size_t i=0;i<values.size();++i)Set(i,values[i]);}
    void Scroll(int value){const int next=std::clamp(value,0,std::max(0,D(9*88+16)-page));if(next==offset)return;offset=next;Layout();}
    void Layout(){if(!viewport)return;RECT c{};GetClientRect(window,&c);page=std::max(D(80),int(c.bottom)-D(138));
        const int bodyWidth=std::max(D(300),int(c.right)-D(48));
        offset=std::clamp(offset,0,std::max(0,D(9*88+16)-page));const int x=D(102),width=std::max(D(160),bodyWidth-x-D(24));
        // Never carry old child pixels into a neighbouring row during scrolling.
        // Position everything without painting, then repaint the whole clipped tree.
        auto move=[](HWND child,int x,int y,int width,int height){SetWindowPos(child,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);};
        move(viewport,D(24),D(58),bodyWidth,page);
        for(size_t row=0;row<9;++row){
            const auto field=fields[DisplayOrder[row]];const int y=D(8+int(row)*88)-offset;
            move(field,x,y,width,D(36));
            // Don't leave a thin fragment of a binding at the viewport edge.
            // Keep it visible to keyboard navigation; focusing it scrolls it into view.
            SetWindowRgn(field,y>=0&&y+D(36)<=page?nullptr:CreateRectRgn(0,0,0,0),FALSE);
        }
        scroll.Update(D(9*88+16),page,offset,dpi);move(scroll.Window(),c.right-D(13),D(58),D(6),page);
        move(reset,c.right-D(262),c.bottom-D(57),D(134),D(36));move(saveButton,c.right-D(112),c.bottom-D(57),D(88),D(36));
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    }
    void Paint(HWND target,HDC out,bool body){RECT c{};GetClientRect(target,&c);HDC dc=CreateCompatibleDC(out);auto bitmap=CreateCompatibleBitmap(out,std::max(1L,c.right),std::max(1L,c.bottom));auto old=SelectObject(dc,bitmap);HBRUSH brush=CreateSolidBrush(RGB(247,247,247));FillRect(dc,&c,brush);DeleteObject(brush);SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(96,98,102));
        if(body){for(size_t row=0;row<9;++row){auto index=DisplayOrder[row];int y=D(8+int(row)*88)-offset;if(y+D(84)<0||y>c.bottom)continue;RECT label{0,y,D(88),y+D(36)};if(label.top>=0&&label.bottom<=c.bottom)DrawTextW(dc,Names[index],-1,&label,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);RECT desc{D(102),y+D(43),c.right-D(24),y+D(81)};SetTextColor(dc,errorIndex==int(index)?RGB(210,70,65):RGB(130,133,140));const auto* text=errorIndex==int(index)?error.c_str():Descriptions[index];if(desc.top>=0&&desc.bottom<=c.bottom)DrawTextW(dc,text,-1,&desc,DT_WORDBREAK);SetTextColor(dc,RGB(96,98,102));}}
        else{RECT title{D(26),D(14),c.right-D(68),D(44)};DrawTextW(dc,L"快捷键管理",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);HPEN pen=CreatePen(PS_SOLID,1,RGB(226,228,232));auto oldPen=SelectObject(dc,pen);MoveToEx(dc,D(24),c.bottom-D(78),nullptr);LineTo(dc,c.right-D(24),c.bottom-D(78));SelectObject(dc,oldPen);DeleteObject(pen);}
        BitBlt(out,0,0,c.right,c.bottom,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    bool Leave(){return draft==initial||shared_ui::ConfirmSourceAction(window,L"放弃未保存的修改？",L"快捷键尚未保存，关闭后将保留原来的绑定。",L"放弃修改");}
    void Submit(){errorIndex=-1;error.clear();for(size_t i=0;i<draft.size();++i){if(!Valid(draft[i])){errorIndex=int(i);error=L"请使用 Ctrl / Alt 组合键或功能键。";break;}if(Empty(draft[i]))continue;for(size_t j=0;j<i;++j)if(draft[i]==draft[j]){errorIndex=int(i);error=std::wstring(L"与“")+Names[j]+L"”重复，请修改。";break;}if(errorIndex>=0)break;}
        if(errorIndex<0&&!save(draft,error)){for(size_t i=0;i<draft.size();++i)if(error.find(Names[i])!=std::wstring::npos){errorIndex=int(i);break;}if(errorIndex<0)errorIndex=int(DisplayOrder[0]);}
        if(errorIndex>=0){for(size_t row=0;row<9;++row)if(DisplayOrder[row]==size_t(errorIndex)){Scroll(D(int(row)*88));break;}SetFocus(fields[errorIndex]);InvalidateRect(viewport,nullptr,FALSE);return;}
        initial=draft;SetFocus(window);CaptureKeys(false);InvalidateRect(viewport,nullptr,FALSE);
    }
    static LRESULT CALLBACK Field(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,m,wp,lp);const size_t index=GetDlgCtrlID(w)-300;
        if(m==WM_ERASEBKGND)return 1;if(m==WM_GETDLGCODE){auto* message=reinterpret_cast<MSG*>(lp);return message&&message->wParam!=VK_TAB?DLGC_WANTALLKEYS:DLGC_WANTCHARS|DLGC_WANTARROWS;}
        if(m==WM_SETFOCUS){self->CaptureKeys(true);self->beforeFocus=self->draft[index];RECT field{},clip{};GetWindowRect(w,&field);GetWindowRect(self->viewport,&clip);if(field.top<clip.top)self->Scroll(self->offset+field.top-clip.top);else if(field.bottom>clip.bottom)self->Scroll(self->offset+field.bottom-clip.bottom);InvalidateRect(w,nullptr,FALSE);return 0;}
        if(m==WM_KILLFOCUS){self->CaptureKeys(false);InvalidateRect(w,nullptr,FALSE);return 0;}
        if(m==WM_LBUTTONDOWN){SetFocus(w);RECT r{};GetClientRect(w,&r);if(GET_X_LPARAM(lp)>r.right-self->D(32))self->Set(index,{});return 0;}
        if(m==WM_MOUSEWHEEL)return SendMessageW(self->window,m,wp,lp);
        if(m==HKM_GETHOTKEY){auto b=self->draft[index];BYTE flags=BYTE((b.modifiers&MOD_CONTROL?HOTKEYF_CONTROL:0)|(b.modifiers&MOD_ALT?HOTKEYF_ALT:0)|(b.modifiers&MOD_SHIFT?HOTKEYF_SHIFT:0));return MAKEWORD(b.key,flags);}
        if(m==HKM_SETHOTKEY){BYTE f=HIBYTE(wp);self->Set(index,{UINT((f&HOTKEYF_CONTROL?MOD_CONTROL:0)|(f&HOTKEYF_ALT?MOD_ALT:0)|(f&HOTKEYF_SHIFT?MOD_SHIFT:0)),LOBYTE(wp)});return 0;}
        if(m==WM_KEYDOWN||m==WM_SYSKEYDOWN){
            if(wp==VK_TAB)return DefWindowProcW(w,m,wp,lp);
            if(wp==VK_ESCAPE){self->Set(index,self->beforeFocus);SetFocus(self->window);return 0;}
            if(wp==VK_DELETE||wp==VK_BACK){self->Set(index,{});return 0;}
            if(wp==VK_CONTROL||wp==VK_MENU||wp==VK_SHIFT)return 0;
            Binding b{UINT((GetKeyState(VK_CONTROL)<0?MOD_CONTROL:0)|(GetKeyState(VK_MENU)<0?MOD_ALT:0)|(GetKeyState(VK_SHIFT)<0?MOD_SHIFT:0)),UINT(wp)};
            if(GetKeyState(VK_LWIN)<0||GetKeyState(VK_RWIN)<0||!Valid(b)){
                self->errorIndex=int(index);self->error=L"请使用 Ctrl / Alt 加字母、数字，或 F1–F24；不支持 Win 键。";InvalidateRect(self->viewport,nullptr,FALSE);return 0;
            }
            self->Set(index,b);return 0;
        }
        if(m==WM_CHAR||m==WM_SYSCHAR||m==WM_SYSKEYUP)return 0;
        if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);HBRUSH bg=CreateSolidBrush(RGB(247,247,247));FillRect(dc,&r,bg);DeleteObject(bg);Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Round(g,r,float(self->D(5)),Gdiplus::Color(255,255,255,255),GetFocus()==w?Gdiplus::Color(255,103,194,58):Gdiplus::Color(255,220,223,230));SelectObject(dc,self->font);SetBkMode(dc,TRANSPARENT);const bool empty=Empty(self->draft[index]);SetTextColor(dc,empty?RGB(168,171,178):RGB(96,98,102));RECT text{self->D(12),0,r.right-self->D(35),r.bottom};auto label=empty?std::wstring(L"请设置快捷键"):Text(self->draft[index]);DrawTextW(dc,label.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
            if(!empty){float x=float(r.right-self->D(19)),y=r.bottom/2.0f,s=float(self->D(5));Gdiplus::Pen pen(Gdiplus::Color(255,160,164,171),1);g.DrawEllipse(&pen,x-s,y-s,2*s,2*s);g.DrawLine(&pen,x-s/2,y-s/2,x+s/2,y+s/2);g.DrawLine(&pen,x+s/2,y-s/2,x-s/2,y+s/2);}EndPaint(w,&ps);return 0;}
        return DefWindowProcW(w,m,wp,lp);
    }
    static LRESULT CALLBACK View(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(self){if(m==WM_ERASEBKGND)return 1;if(m==WM_LBUTTONDOWN){SetFocus(self->window);return 0;}if(m==WM_MOUSEWHEEL)return SendMessageW(self->window,m,wp,lp);if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(w,dc,true);EndPaint(w,&ps);return 0;}}return DefWindowProcW(w,m,wp,lp);}
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(w,m,wp,lp);
        if(m==WM_ERASEBKGND)return 1;if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(w,&ps);self->Paint(w,dc,false);EndPaint(w,&ps);return 0;}
        if(m==WM_LBUTTONDOWN){SetFocus(w);return 0;}if(m==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE)self->CaptureKeys(false);if(m==WM_SIZE){self->Layout();return 0;}if(m==WM_GETMINMAXINFO){auto* mm=reinterpret_cast<MINMAXINFO*>(lp);mm->ptMinTrackSize={self->D(600),self->D(400)};return 0;}
        if(m==WM_DPICHANGED){self->dpi=HIWORD(wp);self->Font();auto r=*reinterpret_cast<RECT*>(lp);SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();return 0;}
        if(m==WM_MOUSEWHEEL){self->wheel+=GET_WHEEL_DELTA_WPARAM(wp);const int steps=self->wheel/WHEEL_DELTA;self->wheel%=WHEEL_DELTA;self->Scroll(self->offset-steps*self->D(66));return 0;}
        if(m==WM_DRAWITEM){const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);HBRUSH bg=CreateSolidBrush(RGB(247,247,247));FillRect(item->hDC,&item->rcItem,bg);DeleteObject(bg);Gdiplus::Graphics g(item->hDC);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);bool save=item->CtlID==IDOK;auto color=save?Gdiplus::Color(255,103,194,58):Gdiplus::Color(255,255,255,255);Round(g,item->rcItem,float(self->D(5)),color,save?color:Gdiplus::Color(255,220,223,230));SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,self->font);SetTextColor(item->hDC,save?RGB(255,255,255):RGB(96,98,102));RECT r=item->rcItem;DrawTextW(item->hDC,save?L"保存":L"恢复默认值",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return TRUE;}
        if(m==WM_COMMAND){if(LOWORD(wp)==501){self->Fill(Defaults);self->Scroll(0);return 0;}if(LOWORD(wp)==IDOK){self->Submit();return 0;}if(LOWORD(wp)==IDCANCEL){SendMessageW(w,WM_CLOSE,0,0);return 0;}}
        if(m==WM_CLOSE){if(self->Leave())DestroyWindow(w);return 0;}if(m==WM_NCDESTROY)self->window=nullptr;
        return DefWindowProcW(w,m,wp,lp);
    }
};
}
void ShowSettings(HWND owner,const Bindings& current,const std::function<bool(const Bindings&,std::wstring&)>& save,const std::function<void(bool)>& captureChanged){
    Editor view(current,save,captureChanged);WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Editor::Proc;wc.lpszClassName=L"PcTool.HotkeySettings";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_APP_ICON));RegisterClassW(&wc);
    POINT p{};GetCursorPos(&p);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST),&mi);HWND w=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"快捷键管理 · PcTool",WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,mi.rcWork.left+20,mi.rcWork.top+20,760,720,owner,nullptr,wc.hInstance,&view);if(!w)return;view.dpi=GetDpiForWindow(w);view.Font();
    wc.lpszClassName=L"PcTool.HotkeyViewport";wc.lpfnWndProc=Editor::View;RegisterClassW(&wc);view.viewport=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,1,1,w,HMENU(INT_PTR(250)),wc.hInstance,&view);
    wc.lpszClassName=L"PcTool.HotkeyBinding";wc.lpfnWndProc=Editor::Field;RegisterClassW(&wc);for(size_t index:DisplayOrder)view.fields[index]=CreateWindowExW(0,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS,0,0,1,1,view.viewport,HMENU(INT_PTR(300+index)),wc.hInstance,&view);
    view.scroll.Create(w,600,[&](int value){view.Scroll(value);},RGB(247,247,247));view.reset=CreateWindowExW(0,L"BUTTON",L"恢复默认值",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,w,HMENU(INT_PTR(501)),wc.hInstance,nullptr);view.saveButton=CreateWindowExW(0,L"BUTTON",L"保存",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,w,HMENU(INT_PTR(IDOK)),wc.hInstance,nullptr);
    view.Fill(current);view.frame.Attach(w);int width=std::min(view.D(760),int(mi.rcWork.right-mi.rcWork.left)-view.D(28)),height=std::min(view.D(720),int(mi.rcWork.bottom-mi.rcWork.top)-view.D(28));SetWindowPos(w,nullptr,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-width)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-height)/2,width,height,SWP_NOZORDER);view.Layout();ShowWindow(w,SW_SHOW);SetForegroundWindow(w);SetFocus(w);
    MSG msg{};while(view.window){int result=GetMessageW(&msg,nullptr,0,0);if(result<=0){if(!result)PostQuitMessage(int(msg.wParam));break;}if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE&&GetFocus()==w){SendMessageW(w,WM_CLOSE,0,0);continue;}const bool own=msg.hwnd==w||IsChild(w,msg.hwnd);if(!own||!IsDialogMessageW(w,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(view.window)DestroyWindow(view.window);
}
}
