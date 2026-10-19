#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
static void Check(HRESULT hr) { if(FAILED(hr)) throw hr; }
static void Require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }

// Stop an unguarded Explorer command before it can launch any archive job.
class RefusingItems final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IShellItemArray> {
public:
    unsigned reads{};
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*,REFGUID,REFIID,void**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS,REFIID,void**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY,REFIID,void**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS,SFGAOF,SFGAOF*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCount(DWORD*)override{++reads;return E_ABORT;}
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD,IShellItem**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems**)override{return E_NOTIMPL;}
};
using GetClass=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
struct ShellProbe {
    GetClass getClass{};CLSID clsid{};
    ComPtr<IClassFactory> factory;
    ComPtr<IShellExtInit> extension;
    ComPtr<IContextMenu> context;
    ComPtr<IExplorerCommand> explorer,expandedCommand;
    ComPtr<IShellItemArray> items;
    ShellProbe(GetClass get,const fs::path& file):getClass(get){
        Check(CLSIDFromString(L"{23170F69-40C1-278A-1000-000100020000}",&clsid));
        Check(getClass(clsid,IID_PPV_ARGS(&factory)));
        Check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&extension)));
        PIDLIST_ABSOLUTE absolute{};Check(SHParseDisplayName(file.c_str(),nullptr,&absolute,0,nullptr));
        ComPtr<IShellFolder> parent;PCUITEMID_CHILD child{};
        auto hr=SHBindToParent(absolute,IID_PPV_ARGS(&parent),&child);
        ComPtr<IDataObject> data;
        if(SUCCEEDED(hr))hr=parent->GetUIObjectOf(nullptr,1,&child,IID_IDataObject,nullptr,reinterpret_cast<void**>(data.GetAddressOf()));
        CoTaskMemFree(absolute);Check(hr);
        Check(extension->Initialize(nullptr,data.Get(),nullptr));
        Check(extension.As(&context));Check(extension.As(&explorer));
        Check(SHCreateShellItemArrayFromDataObject(data.Get(),IID_PPV_ARGS(&items)));
        Enabled();
        LPWSTR title{};Check(explorer->GetTitle(items.Get(),&title));CoTaskMemFree(title);
        ComPtr<IEnumExplorerCommand> commands;Check(explorer->EnumSubCommands(&commands));
        ULONG fetched{};Check(commands->Next(1,&expandedCommand,&fetched));
        Require(fetched==1&&expandedCommand,"No cached Explorer subcommand");
        Enabled();
    }
    void Enabled(){
        HMENU menu=CreatePopupMenu();Require(menu!=nullptr,"Cannot allocate test menu");
        const auto hr=context->QueryContextMenu(menu,0,1,10000,CMF_NORMAL);const int count=GetMenuItemCount(menu);DestroyMenu(menu);
        Check(hr);Require(count>0&&HRESULT_CODE(hr)>0,"Normal context menu is empty");
        for(auto* cached:{explorer.Get(),expandedCommand.Get()})if(cached){
            EXPCMDSTATE state{};Check(cached->GetState(items.Get(),FALSE,&state));Require(state==ECS_ENABLED,"Normal cached Explorer command disabled");
        }
    }
    void Blocked(bool modern=true){
        ComPtr<IClassFactory> blocked;Require(getClass(clsid,IID_PPV_ARGS(&blocked))==CLASS_E_CLASSNOTAVAILABLE,"DLL factory available after uninstall");
        ComPtr<IShellExtInit> instance;Require(factory->CreateInstance(nullptr,IID_PPV_ARGS(&instance))==CLASS_E_CLASSNOTAVAILABLE,"Cached factory available after uninstall");
        HMENU menu=CreatePopupMenu();Require(menu!=nullptr,"Cannot allocate test menu");
        const auto hr=context->QueryContextMenu(menu,0,1,10000,CMF_NORMAL);const int count=GetMenuItemCount(menu);DestroyMenu(menu);
        Require(SUCCEEDED(hr)&&HRESULT_CODE(hr)==0&&count==0,"Cached context menu returned commands after uninstall");
        // The invalid verb also makes a regression harmless in an installed fixture.
        CMINVOKECOMMANDINFO command{};command.cbSize=sizeof(command);command.lpVerb=MAKEINTRESOURCEA(0xffff);
        Require(context->InvokeCommand(&command)==E_ACCESSDENIED,"Cached legacy command did not reject uninstall");
        if(modern)for(auto* cached:{explorer.Get(),expandedCommand.Get()}){
            EXPCMDSTATE state=ECS_ENABLED;Check(cached->GetState(items.Get(),FALSE,&state));
            Require(state==ECS_HIDDEN,"Cached Explorer command not hidden during uninstall");
            auto refusing=Microsoft::WRL::Make<RefusingItems>();
            Require(cached->Invoke(refusing.Get(),nullptr)==E_ACCESSDENIED&&refusing->reads==0,"Cached Explorer command did not reject uninstall before dispatch");
        }
    }
};
static fs::path FixturePath(const fs::path& path){
    const auto root=fs::weakly_canonical(fs::path(__FILE__).parent_path().parent_path()/L"out");
    const auto full=fs::weakly_canonical(fs::absolute(path));
    auto a=root.begin(),b=full.begin();
    for(;a!=root.end()&&b!=full.end();++a,++b)Require(_wcsicmp(a->c_str(),b->c_str())==0,"Lifecycle test path must be inside project out");
    Require(a==root.end()&&b!=full.end(),"Lifecycle test requires an independent out fixture");return full;
}
static void WriteSignal(const fs::path& path,const char* text){std::ofstream file(path);file<<text;file.close();Require(bool(file),"Cannot write lifecycle signal");}
static bool WaitSignal(const fs::path& path,const fs::path& release={}){
    const auto end=GetTickCount64()+600000;
    const auto ready=[&]{return fs::exists(path)||(!release.empty()&&fs::exists(release));};
    while(!ready()&&GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(20);}
    Require(ready(),"Timed out waiting for lifecycle controller");
    return fs::exists(path);
}
struct RestoreFixture {
    fs::path executable,backup,marker;
    explicit RestoreFixture(const fs::path& root):executable(root/L"PcTool.exe"),backup(root/(L"PcTool.exe.shell-test-"+std::to_wstring(GetCurrentProcessId()))),marker(root/L".pctool-uninstalling"){
        Require(fs::is_regular_file(executable)&&!fs::exists(marker)&&!fs::exists(backup),"Lifecycle fixture is not a clean installed tree");
    }
    ~RestoreFixture(){std::error_code error;if(fs::exists(backup,error))fs::rename(backup,executable,error);fs::remove(marker,error);}
};
int wmain(int argc,wchar_t** argv) {
    if(argc<2||argc>5)return 2;
    const std::wstring mode=argc>=3?argv[2]:L"";
    const bool late=mode==L"--late-retirement-host";
    const bool host=mode==L"--lifecycle-host"||late;
    if((host&&argc<4)||(!host&&argc>4)||(!mode.empty()&&!host&&mode!=L"uninstall-guard"&&mode!=L"uninstall-root-guard"&&mode!=L"uninstall-command-guard"))return 2;
    const fs::path runtime=fs::absolute(argv[1]);
    const fs::path dll=host?(argc==5?argv[4]:L"7-zip.dll"):(argc==4?argv[3]:L"7-zip.dll");
    if(dll!=L"7-zip.dll"&&dll!=L"7-zip32.dll")return 2;
    const auto temporary=fs::temp_directory_path()/(L"PcTool-shell-"+std::to_wstring(GetCurrentProcessId()));
    const auto file=temporary/L"context-menu.txt";
    fs::path prefix;HMODULE module{};bool initialized=false;int result=0;
    try {
        Check(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));initialized=true;
        fs::create_directories(temporary);WriteSignal(file,"7-Zip shell integration test");
        if(!mode.empty())FixturePath(runtime);
        if(host){prefix=FixturePath(argv[3]);for(const wchar_t* suffix:{L".ready",L".check",L".checked",L".release",L".failed"})Require(!fs::exists(prefix.wstring()+suffix),"Stale lifecycle signal");}
        module=LoadLibraryW((runtime/dll).c_str());if(!module)Check(HRESULT_FROM_WIN32(GetLastError()));
        auto getClass=reinterpret_cast<GetClass>(GetProcAddress(module,"DllGetClassObject"));Require(getClass!=nullptr,"Missing COM factory export");
        auto canUnload=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module,"DllCanUnloadNow"));Require(canUnload&&canUnload()==S_OK,"DLL unexpectedly pinned before creating objects");
        if(late){
            WriteSignal(prefix.wstring()+L".ready","ready");
            if(WaitSignal(prefix.wstring()+L".check",prefix.wstring()+L".release")){
                CLSID clsid{};Check(CLSIDFromString(L"{23170F69-40C1-278A-1000-000100020000}",&clsid));
                ComPtr<IClassFactory> factory;
                Require(getClass(clsid,IID_PPV_ARGS(&factory))==CLASS_E_CLASSNOTAVAILABLE,"Late COM request enabled a retired module");
                WriteSignal(prefix.wstring()+L".checked","late COM request blocked");WaitSignal(prefix.wstring()+L".release");
            }
        }else{
            ShellProbe probe(getClass,file);
            const auto root=runtime.parent_path().parent_path();
            const auto hive=root/L"Data/SevenZip/settings.hiv";
            if(fs::exists(hive)){
                HANDLE handle=CreateFileW(hive.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
                if(handle==INVALID_HANDLE_VALUE)Check(HRESULT_FROM_WIN32(GetLastError()));CloseHandle(handle);
                std::cout<<"PASS settings hive released while cached COM objects remain alive.\n";
            }
            if(host){
                Require(fs::is_regular_file(root/L"PcTool.exe"),"Lifecycle host must first observe an installed PcTool tree");
                WriteSignal(prefix.wstring()+L".ready","ready");
                // Upgrade tests only need the DLL held, then released. Uninstall
                // tests additionally request checks against these same objects.
                if(WaitSignal(prefix.wstring()+L".check",prefix.wstring()+L".release")){
                    probe.Blocked();WriteSignal(prefix.wstring()+L".checked","cached COM lifecycle checks passed");
                    WaitSignal(prefix.wstring()+L".release");
                }
            }else if(!mode.empty()){
                RestoreFixture restore(FixturePath(root));
                WriteSignal(restore.marker,"uninstall test");
                const bool modern=mode!=L"uninstall-root-guard";
                probe.Blocked(modern);
                if(mode!=L"uninstall-command-guard"){
                    fs::rename(restore.executable,restore.backup);probe.Blocked(modern);
                    fs::remove(restore.marker);probe.Blocked(modern);
                    fs::rename(restore.backup,restore.executable);probe.Enabled();
                    ComPtr<IClassFactory> restored;Check(getClass(probe.clsid,IID_PPV_ARGS(&restored)));
                }
                std::cout<<"PASS cached COM objects blocked through uninstall and restored after reinstall.\n";
            }
            if(!host)Check(probe.factory->LockServer(TRUE));
        }
        if(host)Require(canUnload()==S_OK,"Cached lifecycle objects did not release DLL");
        else{
            Require(canUnload()==S_FALSE,"LockServer did not retain DLL");
            CLSID clsid{};Check(CLSIDFromString(L"{23170F69-40C1-278A-1000-000100020000}",&clsid));
            ComPtr<IClassFactory> factory;Check(getClass(clsid,IID_PPV_ARGS(&factory)));Check(factory->LockServer(FALSE));factory.Reset();
            Require(canUnload()==S_OK,"COM unload reference count did not return to zero");
        }
        std::cout<<"PASS original 7-Zip menu and COM lifecycle.\n";
    }catch(HRESULT hr){std::cerr<<"Shell integration failed: 0x"<<std::hex<<unsigned(hr)<<"\n";result=1;}
    catch(const std::exception& error){std::cerr<<"Shell integration failed: "<<error.what()<<"\n";result=1;}
    if(result&&!prefix.empty()){try{WriteSignal(prefix.wstring()+L".failed","cached COM lifecycle check failed");}catch(...) {}}
    if(module)FreeLibrary(module);
    {std::error_code error;fs::remove(file,error);fs::remove(temporary,error);}
    if(initialized)CoUninitialize();return result;
}
