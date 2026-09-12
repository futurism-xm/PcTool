#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if(FAILED(hr)) throw hr; }
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    Check(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));
    const std::filesystem::path runtime=argv[1];
    const auto temporary=std::filesystem::temp_directory_path()/(L"PcTool-shell-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(temporary);
    const auto file=temporary/L"context-menu.txt";
    std::ofstream(file)<<"7-Zip shell integration test";
    HMODULE module=LoadLibraryW((runtime/L"7-zip.dll").c_str());
    int result=0;
    try {
        if(!module) Check(HRESULT_FROM_WIN32(GetLastError()));
        auto getClass=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
        if(!getClass) Check(E_NOINTERFACE);
        CLSID clsid{};Check(CLSIDFromString(L"{23170F69-40C1-278A-1000-000100020000}",&clsid));
        ComPtr<IClassFactory> factory;Check(getClass(clsid,IID_PPV_ARGS(&factory)));
        ComPtr<IShellExtInit> extension;Check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&extension)));
        PIDLIST_ABSOLUTE absolute{};Check(SHParseDisplayName(file.c_str(),nullptr,&absolute,0,nullptr));
        ComPtr<IShellFolder> parent;PCUITEMID_CHILD child{};
        auto hr=SHBindToParent(absolute,IID_PPV_ARGS(&parent),&child);
        ComPtr<IDataObject> data;
        if(SUCCEEDED(hr)) hr=parent->GetUIObjectOf(nullptr,1,&child,IID_IDataObject,nullptr,reinterpret_cast<void**>(data.GetAddressOf()));
        CoTaskMemFree(absolute);Check(hr);
        Check(extension->Initialize(nullptr,data.Get(),nullptr));
        ComPtr<IContextMenu> context;Check(extension.As(&context));
        HMENU menu=CreatePopupMenu();
        hr=context->QueryContextMenu(menu,0,1,10000,CMF_NORMAL);
        const int count=GetMenuItemCount(menu);
        DestroyMenu(menu);Check(hr);
        if(count<=0 || HRESULT_CODE(hr)==0) Check(E_FAIL);
        std::cout<<"PASS: original 7-Zip COM shell extension initialized with a real file and populated its context menu.\n";
    } catch(HRESULT hr) {std::cerr<<"Shell integration failed: 0x"<<std::hex<<unsigned(hr)<<"\n";result=1;}
    if(module) FreeLibrary(module);
    std::filesystem::remove(file);
    std::filesystem::remove(temporary);
    CoUninitialize();return result;
}
