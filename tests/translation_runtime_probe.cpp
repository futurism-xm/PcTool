#include <windows.h>
#include <dwmapi.h>
#include <iostream>
#include <cwchar>

// Read-only inspection of the shipping process, without changing focus or input.
int main() {
    EnumWindows([](HWND w, LPARAM)->BOOL {
        wchar_t name[128]{}; GetClassNameW(w,name,128);
        if(wcsncmp(name,L"PcTool.Translation",18)!=0)return TRUE;
        RECT r{};GetWindowRect(w,&r);DWORD pid{},cloaked{};
        GetWindowThreadProcessId(w,&pid);
        DwmGetWindowAttribute(w,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
        HRGN region=CreateRectRgn(0,0,0,0);int regionType=GetWindowRgn(w,region);RECT box{};GetRgnBox(region,&box);DeleteObject(region);
        HWND hit=WindowFromPoint({(r.left+r.right)/2,(r.top+r.bottom)/2});
        std::wcout<<name<<L" hwnd="<<w<<L" pid="<<pid<<L" visible="<<IsWindowVisible(w)<<L" iconic="<<IsIconic(w)<<L" cloaked="<<cloaked
            <<L" style="<<std::hex<<GetWindowLongPtrW(w,GWL_STYLE)<<L" ex="<<GetWindowLongPtrW(w,GWL_EXSTYLE)<<std::dec
            <<L" rect="<<r.left<<L","<<r.top<<L","<<r.right<<L","<<r.bottom
            <<L" region="<<regionType<<L":"<<box.left<<L","<<box.top<<L","<<box.right<<L","<<box.bottom
            <<L" centerRoot="<<GetAncestor(hit,GA_ROOT)<<L" foreground="<<GetForegroundWindow()<<L"\n";
        return TRUE;
    },0);
}
