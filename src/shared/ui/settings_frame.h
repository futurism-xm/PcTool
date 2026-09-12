#pragma once
#include "shared/ui/popup_controls.h"
#include <commctrl.h>
namespace shared_ui {
class SettingsFrame {
public:
    SettingsFrame();~SettingsFrame();
    void Attach(HWND window);
    void Sync();
private:
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    static LRESULT CALLBACK CloseProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    void Edge();void Cleanup();
    HWND window_{},close_{},edge_{};int width_{},height_{};UINT dpi_{96};ULONG_PTR graphics_{};
    PopupShadow shadow_{L"PcTool.SettingsShadow"};
    bool syncing_{};
};
}
