#include <windows.h>
#include <filesystem>
#include <string>
#include <fstream>
static bool stubborn = false;
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLOSE) { if (!stubborn) PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, w, l);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    std::filesystem::path root(argv[1]);
    stubborn = std::wstring(argv[2]) == L"stubborn";
    HMODULE extension{};
    if(std::wstring(argv[2])==L"module"){
        extension=LoadLibraryW((root/L"modules/archive/7-zip.dll").c_str());
        if(!extension)return 4;
    }
    auto data = root / L"Data/test/SevenZip";
    std::filesystem::create_directories(data);
    HKEY hive{};
    if (RegLoadAppKeyW((data / L"settings.hiv").c_str(), &hive, KEY_ALL_ACCESS, 0, 0)) return 2;
    WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"PcTool.UninstallFixture";
    RegisterClassW(&cls);
    auto window = CreateWindowW(cls.lpszClassName, L"uninstall fixture", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 3;
    std::ofstream(root / (std::to_wstring(GetCurrentProcessId()) + L".ready")) << "ready";
    MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    RegCloseKey(hive);
    if(extension)FreeLibrary(extension);
    return 0;
}
