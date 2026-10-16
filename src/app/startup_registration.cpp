#include "app/startup_registration.h"
#include "shared/platform/app_storage.h"
#include <taskschd.h>
#include <wrl/client.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace startup_registration {
namespace {
using Microsoft::WRL::ComPtr;
struct Bstr {
    BSTR value{};
    explicit Bstr(const std::wstring& text):value(SysAllocString(text.c_str())){if(!value)throw std::bad_alloc();}
    ~Bstr(){SysFreeString(value);}
    Bstr(const Bstr&)=delete;
    Bstr& operator=(const Bstr&)=delete;
    operator BSTR()const{return value;}
};
void Check(HRESULT result){if(FAILED(result))throw result;}
bool Missing(HRESULT result){return result==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||result==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);}
std::wstring RootHash(const std::filesystem::path& root){
    auto text=root.wstring();
    if(!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,text.c_str(),int(text.size()),text.data(),int(text.size()),nullptr,nullptr,0))throw HRESULT_FROM_WIN32(GetLastError());
    HCRYPTPROV provider{};HCRYPTHASH hash{};std::wstring result;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))throw HRESULT_FROM_WIN32(GetLastError());
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=FALSE;
    BYTE bytes[32]{};DWORD size=sizeof(bytes);
    if(ok)ok=CryptHashData(hash,reinterpret_cast<const BYTE*>(text.data()),DWORD(text.size()*sizeof(wchar_t)),0)&&CryptGetHashParam(hash,HP_HASHVAL,bytes,&size,0);
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);
    if(!ok)throw E_FAIL;
    for(auto byte:bytes){result+=L"0123456789ABCDEF"[byte>>4];result+=L"0123456789ABCDEF"[byte&15];}return result;
}
// Reject effective allow ACEs that grant replacement rights to identities other
// than Administrators, SYSTEM or TrustedInstaller. Ancestors are checked too:
// protecting an EXE alone does not prevent replacing its containing directory.
struct UnsafeStartupPath {std::filesystem::path path;};
void CheckAcl(const std::filesystem::path& path,bool payload){
        const DWORD attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw UnsafeStartupPath{path};
        PACL dacl{};PSID owner{};PSECURITY_DESCRIPTOR descriptor{};
        Check(HRESULT_FROM_WIN32(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|OWNER_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&descriptor)));
        struct Free {PSECURITY_DESCRIPTOR p;~Free(){LocalFree(p);}} release{descriptor};
        if(!dacl)throw UnsafeStartupPath{path};
        auto trusted=[](PSID sid){LPWSTR text{};if(!ConvertSidToStringSidW(sid,&text))return false;const std::wstring identity=text;LocalFree(text);return identity==L"S-1-5-18"||identity==L"S-1-5-32-544"||identity==L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";};
        if(!owner||!trusted(owner))throw UnsafeStartupPath{path};
        for(DWORD i=0;i<dacl->AceCount;++i){
            void* data{};if(!GetAce(dacl,i,&data))throw UnsafeStartupPath{path};
            const auto* header=static_cast<ACE_HEADER*>(data);
            if(header->AceFlags&INHERIT_ONLY_ACE)continue;
            if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){if(header->AceType!=ACCESS_DENIED_ACE_TYPE)throw UnsafeStartupPath{path};continue;}
            auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(data);auto* sid=&ace->SidStart;
            if(trusted(sid))continue;
            ACCESS_MASK mask=ace->Mask;GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};MapGenericMask(&mask,&mapping);
            // A volume root cannot itself be renamed or deleted. Default data
            // volumes grant DELETE on the root; that does not grant deletion
            // of protected children. FILE_DELETE_CHILD must still be rejected.
            ACCESS_MASK forbidden=WRITE_DAC|WRITE_OWNER|FILE_DELETE_CHILD;
            if(path!=path.root_path())forbidden|=DELETE;
            if(payload)forbidden|=FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES;
            if(mask&forbidden)throw UnsafeStartupPath{path};
        }
}
void CheckPath(const std::filesystem::path& executable){
    if(GetDriveTypeW(executable.root_path().c_str())!=DRIVE_FIXED)throw UnsafeStartupPath{executable.root_path()};
    for(auto path=executable;!path.empty();path=path.parent_path()){
        CheckAcl(path,path==executable||path==executable.parent_path());
        if(path==path.root_path())break;
    }
    // Dependencies loaded beside the executable must be protected as well.
    const auto root=executable.parent_path();
    for(auto iterator=std::filesystem::recursive_directory_iterator(root);iterator!=std::filesystem::recursive_directory_iterator();++iterator){
        const auto& entry=*iterator;
        const auto relative=entry.path().lexically_relative(root);
        auto first=relative.begin();
        if(first!=relative.end()&&(*first==L"Data"||*first==L"Cache")){if(entry.is_directory())iterator.disable_recursion_pending();continue;}
        if(GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw UnsafeStartupPath{entry.path()};
        const auto extension=entry.path().extension().wstring();
        if(entry.is_regular_file()&&(_wcsicmp(extension.c_str(),L".dll")==0||_wcsicmp(extension.c_str(),L".exe")==0)){
            for(auto path=entry.path();path!=root;path=path.parent_path())CheckAcl(path,true);
        }
    }
}
std::wstring Description(const std::filesystem::path& executable,const std::wstring& sid){return L"PcTool Startup v1|"+sid+L"|"+executable.wstring();}
bool SameUser(const wchar_t* account,const std::wstring& sid){
    if(!account)return false;
    if(sid==account)return true;
    DWORD size{},domainSize{};SID_NAME_USE use{};
    LookupAccountNameW(nullptr,account,nullptr,&size,nullptr,&domainSize,&use);
    if(!size)return false;std::vector<BYTE> bytes(size);std::vector<wchar_t> domain(domainSize);
    if(!LookupAccountNameW(nullptr,account,bytes.data(),&size,domain.data(),&domainSize,&use))return false;
    LPWSTR text{};if(!ConvertSidToStringSidW(bytes.data(),&text))return false;const bool equal=sid==text;LocalFree(text);return equal;
}
bool Owned(ITaskDefinition* definition,const std::filesystem::path& executable,const std::wstring& sid){
    ComPtr<IRegistrationInfo> info;ComPtr<IPrincipal> principal;ComPtr<IActionCollection> actions;ComPtr<IAction> action;ComPtr<IExecAction> exec;
    if(FAILED(definition->get_RegistrationInfo(&info))||FAILED(definition->get_Principal(&principal))||FAILED(definition->get_Actions(&actions)))return false;
    BSTR description{},user{},path{},arguments{};LONG count{};TASK_LOGON_TYPE logon{};TASK_RUNLEVEL_TYPE level{};
    bool ok=SUCCEEDED(info->get_Description(&description))&&description&&_wcsicmp(Description(executable,sid).c_str(),description)==0&&
        SUCCEEDED(principal->get_UserId(&user))&&SameUser(user,sid)&&SUCCEEDED(principal->get_LogonType(&logon))&&logon==TASK_LOGON_INTERACTIVE_TOKEN&&
        SUCCEEDED(principal->get_RunLevel(&level))&&level==TASK_RUNLEVEL_HIGHEST&&SUCCEEDED(actions->get_Count(&count))&&count==1&&
        SUCCEEDED(actions->get_Item(1,&action))&&SUCCEEDED(action.As(&exec))&&SUCCEEDED(exec->get_Path(&path))&&path&&_wcsicmp(path,executable.c_str())==0&&
        SUCCEEDED(exec->get_Arguments(&arguments))&&arguments&&wcscmp(arguments,L"--background")==0;
    SysFreeString(description);SysFreeString(user);SysFreeString(path);SysFreeString(arguments);return ok;
}
}
bool Apply(bool enabled,const std::filesystem::path& input,std::wstring& error,const std::wstring& nameSpace){
    error.clear();
    const bool testNamespace=nameSpace.size()==43&&nameSpace.substr(0,11)==L"PcToolTest-"; // validated below as well
    if(nameSpace!=L"PcTool"&&(!testNamespace||!std::all_of(nameSpace.begin()+11,nameSpace.end(),[](wchar_t c){return iswxdigit(c)!=0;}))){error=L"无效的启动任务命名空间。";return false;}
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct Apartment {HRESULT hr;~Apartment(){if(SUCCEEDED(hr))CoUninitialize();}} apartment{initialized};
    try{
        const auto executable=std::filesystem::absolute(input).lexically_normal();
        const auto sid=app_storage::UserSid();const auto name=sid+L"-"+RootHash(executable.parent_path());
        const auto folderPath=L"\\"+nameSpace+L"\\Startup";
        ComPtr<ITaskService> scheduler;Check(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&scheduler)));
        VARIANT empty{};VariantInit(&empty);Check(scheduler->Connect(empty,empty,empty,empty));
        ComPtr<ITaskFolder> folder;HRESULT found=scheduler->GetFolder(Bstr(folderPath),&folder);
        if(Missing(found)&&enabled){
            ComPtr<ITaskFolder> root,parent;Check(scheduler->GetFolder(Bstr(L"\\"),&root));
            auto hr=scheduler->GetFolder(Bstr(L"\\"+nameSpace),&parent);
            Bstr acl(L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)");VARIANT security{};security.vt=VT_BSTR;security.bstrVal=acl;
            if(Missing(hr))Check(root->CreateFolder(Bstr(nameSpace),security,&parent));else Check(hr);
            Check(parent->CreateFolder(Bstr(L"Startup"),security,&folder));
        }else if(!Missing(found))Check(found);
        ComPtr<IRegisteredTask> previous;ComPtr<ITaskDefinition> oldDefinition;
        if(folder){const auto hr=folder->GetTask(Bstr(name),&previous);if(!Missing(hr)){Check(hr);Check(previous->get_Definition(&oldDefinition));if(!Owned(oldDefinition.Get(),executable,sid))throw E_ACCESSDENIED;}}
        const auto runKey=nameSpace==L"PcTool"?L"Software\\Microsoft\\Windows\\CurrentVersion\\Run":L"Software\\"+nameSpace+L"\\Run";
        HKEY raw{};const auto opened=RegOpenKeyExW(HKEY_CURRENT_USER,runKey.c_str(),0,KEY_QUERY_VALUE|KEY_SET_VALUE,&raw);
        struct Key {HKEY h;~Key(){if(h)RegCloseKey(h);}} key{raw};
        if(opened!=ERROR_SUCCESS&&opened!=ERROR_FILE_NOT_FOUND)throw HRESULT_FROM_WIN32(opened);
        std::wstring legacy;DWORD type{},bytes{};
        if(raw&&RegQueryValueExW(raw,L"PcTool",nullptr,&type,nullptr,&bytes)==ERROR_SUCCESS&&type==REG_SZ){
            std::vector<wchar_t> buffer(bytes/sizeof(wchar_t)+1);Check(HRESULT_FROM_WIN32(RegQueryValueExW(raw,L"PcTool",nullptr,&type,reinterpret_cast<BYTE*>(buffer.data()),&bytes)));legacy=buffer.data();
        }
        const bool ownLegacy=_wcsicmp(legacy.c_str(),(L"\""+executable.wstring()+L"\" --background").c_str())==0;
        auto removeLegacy=[&]{
            if(!ownLegacy)return;
            DWORD currentSize{};auto status=RegQueryValueExW(raw,L"PcTool",nullptr,&type,nullptr,&currentSize);
            if(status==ERROR_FILE_NOT_FOUND)return;Check(HRESULT_FROM_WIN32(status));
            std::vector<wchar_t> current(currentSize/sizeof(wchar_t)+1);
            Check(HRESULT_FROM_WIN32(RegQueryValueExW(raw,L"PcTool",nullptr,&type,reinterpret_cast<BYTE*>(current.data()),&currentSize)));
            if(type!=REG_SZ||_wcsicmp(current.data(),legacy.c_str())!=0)throw HRESULT_FROM_WIN32(ERROR_RETRY);
            Check(HRESULT_FROM_WIN32(RegDeleteValueW(raw,L"PcTool")));
        };
        if(!enabled){
            // Delete the legacy value first: if it fails the task stays intact.
            removeLegacy();
            try{
                if(previous){Check(folder->DeleteTask(Bstr(name),0));ComPtr<IRegisteredTask> check;if(!Missing(folder->GetTask(Bstr(name),&check)))throw E_FAIL;}
            }catch(...){
                DWORD presentSize{};
                if(ownLegacy&&RegQueryValueExW(raw,L"PcTool",nullptr,nullptr,nullptr,&presentSize)==ERROR_FILE_NOT_FOUND)
                    Check(HRESULT_FROM_WIN32(RegSetValueExW(raw,L"PcTool",0,REG_SZ,reinterpret_cast<const BYTE*>(legacy.c_str()),DWORD((legacy.size()+1)*sizeof(wchar_t)))));
                throw;
            }
            return true;
        }
        try{CheckPath(executable);}
        catch(const UnsafeStartupPath& unsafe){error=L"无法启用管理员开机启动，路径未受保护：\n"+unsafe.path.wstring()+L"\n\n支持安装到非系统盘。请使用权限受保护的独立目录（如 D:\\PcTool），程序及上级目录不能被普通进程替换。";throw;}
        catch(...){error=L"无法验证安装目录的安全性，请检查文件是否存在及目录权限。支持非系统盘安装。";throw;}
        ComPtr<ITaskDefinition> task;Check(scheduler->NewTask(0,&task));
        ComPtr<IRegistrationInfo> info;Check(task->get_RegistrationInfo(&info));Check(info->put_Description(Bstr(Description(executable,sid))));
        ComPtr<IPrincipal> principal;Check(task->get_Principal(&principal));Check(principal->put_UserId(Bstr(sid)));Check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));Check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
        ComPtr<ITaskSettings> settings;Check(task->get_Settings(&settings));Check(settings->put_Enabled(VARIANT_TRUE));Check(settings->put_StartWhenAvailable(VARIANT_TRUE));Check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));Check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));Check(settings->put_ExecutionTimeLimit(Bstr(L"PT0S")));Check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
        ComPtr<ITriggerCollection> triggers;ComPtr<ITrigger> trigger;ComPtr<ILogonTrigger> logon;Check(task->get_Triggers(&triggers));Check(triggers->Create(TASK_TRIGGER_LOGON,&trigger));Check(trigger.As(&logon));Check(logon->put_UserId(Bstr(sid)));
        ComPtr<IActionCollection> actions;ComPtr<IAction> action;ComPtr<IExecAction> exec;Check(task->get_Actions(&actions));Check(actions->Create(TASK_ACTION_EXEC,&action));Check(action.As(&exec));Check(exec->put_Path(Bstr(executable.wstring())));Check(exec->put_Arguments(Bstr(L"--background")));Check(exec->put_WorkingDirectory(Bstr(executable.parent_path().wstring())));
        Bstr user(sid),acl(L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)");VARIANT userValue{},security{};userValue.vt=security.vt=VT_BSTR;userValue.bstrVal=user;security.bstrVal=acl;
        ComPtr<IRegisteredTask> registered;
        auto registerTask=[&](ITaskDefinition* definition){registered.Reset();Check(folder->RegisterTaskDefinition(Bstr(name),definition,TASK_CREATE_OR_UPDATE|TASK_DONT_ADD_PRINCIPAL_ACE,userValue,empty,TASK_LOGON_INTERACTIVE_TOKEN,security,&registered));};
        registerTask(task.Get());
        try{
            ComPtr<IRegisteredTask> verified;ComPtr<ITaskDefinition> definition;Check(folder->GetTask(Bstr(name),&verified));Check(verified->get_Definition(&definition));if(!Owned(definition.Get(),executable,sid))throw E_FAIL;
            removeLegacy();
        }catch(...){if(oldDefinition)registerTask(oldDefinition.Get());else Check(folder->DeleteTask(Bstr(name),0));throw;}
        return true;
    }catch(HRESULT hr){if(error.empty())error=L"无法更新管理员开机启动任务（错误 "+std::to_wstring(static_cast<unsigned long>(hr))+L"）。";}
    catch(...){if(error.empty())error=L"无法更新管理员开机启动任务，请检查安装目录和任务计划程序。";}
    return false;
}
}
