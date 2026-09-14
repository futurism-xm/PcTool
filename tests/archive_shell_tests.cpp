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
    if(argc!=2 && argc!=3) return 2;
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
        auto canUnload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));
        if(!canUnload||canUnload()!=S_OK)Check(E_FAIL);
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
        const auto hive=runtime.parent_path().parent_path()/L"Data/SevenZip/settings.hiv";
        if(std::filesystem::exists(hive)){
            HANDLE fileHandle=CreateFileW(hive.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(fileHandle==INVALID_HANDLE_VALUE)Check(HRESULT_FROM_WIN32(GetLastError()));CloseHandle(fileHandle);
            std::cout<<"PASS settings hive released while menu object and DLL remain alive.\n";
        }
        if(argc==3 && std::wstring(argv[2])==L"uninstall-guard") {
            const auto root=runtime.parent_path().parent_path();
            if(!std::filesystem::exists(root/L"PcTool.exe")) Check(E_INVALIDARG);
            const auto marker=root/L".pctool-uninstalling";
            std::ofstream(marker)<<"uninstall test";
            ComPtr<IClassFactory> blocked;
            if(getClass(clsid,IID_PPV_ARGS(&blocked))!=CLASS_E_CLASSNOTAVAILABLE) Check(E_FAIL);
            ComPtr<IShellExtInit> blockedInstance;
            if(factory->CreateInstance(nullptr,IID_PPV_ARGS(&blockedInstance))!=CLASS_E_CLASSNOTAVAILABLE) Check(E_FAIL);
            HMENU disabled=CreatePopupMenu();
            hr=context->QueryContextMenu(disabled,0,1,10000,CMF_NORMAL);
            const int remaining=GetMenuItemCount(disabled); DestroyMenu(disabled);
            if(FAILED(hr)||HRESULT_CODE(hr)!=0||remaining!=0) Check(E_FAIL);
            CMINVOKECOMMANDINFO command{};command.cbSize=sizeof(command);command.lpVerb=MAKEINTRESOURCEA(0);
            if(context->InvokeCommand(&command)!=E_ACCESSDENIED) Check(E_FAIL);
            std::filesystem::remove(marker);
            std::cout<<"PASS: loaded class factory, existing menu and cached command reject uninstall-time work.\n";
        }
        std::cout<<"PASS: original 7-Zip COM shell extension initialized with a real file and populated its context menu.\n";
        context.Reset();extension.Reset();Check(factory->LockServer(TRUE));factory.Reset();
        if(canUnload()!=S_FALSE)Check(E_FAIL);
        Check(getClass(clsid,IID_PPV_ARGS(&factory)));Check(factory->LockServer(FALSE));factory.Reset();
        if(canUnload()!=S_OK)Check(E_FAIL);
        std::cout<<"PASS COM unload count and LockServer balance.\n";
    } catch(HRESULT hr) {std::cerr<<"Shell integration failed: 0x"<<std::hex<<unsigned(hr)<<"\n";result=1;}
    if(module) FreeLibrary(module);
    std::filesystem::remove(file);
    std::filesystem::remove(temporary);
    CoUninitialize();return result;
}
