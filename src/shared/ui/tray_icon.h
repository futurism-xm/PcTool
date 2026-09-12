#pragma once
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>
#include <cstdint>

namespace shared_ui {
// Stable per executable location: restarting an unsigned portable application
// reuses its identity without conflicting with another build's executable path.
inline GUID TrayIdentity(std::wstring path){
    if(!path.empty())CharLowerBuffW(path.data(),DWORD(path.size()));
    uint64_t hash=14695981039346656037ull;
    for(wchar_t c:path){hash^=uint16_t(c);hash*=1099511628211ull;}
    GUID guid{0x739c00a1,0x882e,0x48cc,{}};
    for(int i=0;i<8;++i)guid.Data4[i]=BYTE(hash>>(i*8));
    guid.Data4[0]=BYTE((guid.Data4[0]&0x3f)|0x80);return guid;
}

class TrayIcon {
public:
    using Notify=std::function<bool(DWORD,NOTIFYICONDATAW&)>;
    using Ready=std::function<bool()>;
    enum class State { Unknown,Absent,Present };
    explicit TrayIcon(Notify notify=[](DWORD action,NOTIFYICONDATAW& data){return Shell_NotifyIconW(action,&data)!=FALSE;},
        Ready ready=[](){HWND shell=FindWindowW(L"Shell_TrayWnd",nullptr);DWORD_PTR result{};return shell&&SendMessageTimeoutW(shell,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&result)!=0;})
        :notify_(std::move(notify)),ready_(std::move(ready)){}
    void Bind(HWND owner,HICON icon,UINT callback,const GUID& identity){
        data_={sizeof(data_)};data_.hWnd=owner;data_.uID=1;data_.uFlags=NIF_GUID|NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP;
        data_.guidItem=identity;data_.hIcon=icon;data_.uCallbackMessage=callback;
        wcscpy_s(data_.szTip,L"PcTool — 右键打开功能菜单");state_=State::Unknown;newOwner_=true;
    }
    State CurrentState()const{return state_;}
    void ExplorerRestarted(){state_=State::Unknown;}
    bool Show(){
        // A previous process may still own the same GUID. Delete its record
        // before binding notifications to this new HWND.
        if(newOwner_){if(!Remove())return false;newOwner_=false;}
        if(state_==State::Present)return true;
        if(!ready_())return false;
        // Adopt an existing registration first, including an earlier add whose
        // reply was lost. Never blindly add another identity on retry.
        auto data=data_;
        bool ok=notify_(NIM_MODIFY,data);
        if(!ok){state_=State::Unknown;data=data_;ok=notify_(NIM_ADD,data);}
        if(!ok)return false;
        state_=State::Present;data=data_;data.uVersion=NOTIFYICON_VERSION_4;notify_(NIM_SETVERSION,data);return true;
    }
    bool Remove(){
        if(state_==State::Absent)return true;
        if(!ready_())return false;
        auto data=data_;data.uFlags=NIF_GUID;
        if(notify_(NIM_DELETE,data)){state_=State::Absent;return true;}
        // DELETE can also mean 'already absent'. Only clear local state after
        // checking that Explorer no longer has this identity. Failed deletion
        // of an existing icon remains pending for a later retry.
        data=data_;data.uFlags=NIF_GUID;
        if(notify_(NIM_MODIFY,data)){state_=State::Present;return false;}
        if(!ready_()){state_=State::Unknown;return false;}
        state_=State::Absent;return true;
    }
private:
    NOTIFYICONDATAW data_{};Notify notify_;Ready ready_;State state_{State::Unknown};bool newOwner_{true};
};
}
