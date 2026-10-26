#include "translation/source_config.h"
#include "shared/platform/app_storage.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>

using namespace translation;
using namespace winrt::Windows::Data::Json;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string Read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);Check(bool(file),"fixture read");return {(std::istreambuf_iterator<char>(file)),{}};}
std::vector<SourceConfig> Sources(){
    std::vector<SourceConfig> sources;
    for(int i=0;i<4;++i){auto s=NewSource(SourceKind(i));s.name=L"portable 中文 "+std::to_wstring(i);s.key=L"fixture-only 密钥 \"\\ "+std::to_wstring(i);s.model=L"model";s.region=L"eastasia";s.appId=L"appid";s.pro=true;s.enabled=s.verified=i!=2;sources.push_back(s);}
    return sources;
}
void Equal(const std::vector<SourceConfig>& a,const std::vector<SourceConfig>& b){
    Check(a.size()==b.size(),"source count");
    for(size_t i=0;i<a.size();++i){const auto& x=a[i];const auto& y=b[i];
        Check(x.id==y.id&&x.name==y.name&&x.endpoint==y.endpoint&&x.key==y.key&&x.model==y.model&&x.region==y.region&&x.appId==y.appId&&x.kind==y.kind&&x.pro==y.pro&&x.enabled==y.enabled&&x.verified==y.verified,"source fields/order/flags changed");
    }
}
void DefaultWrite(){
    const auto legacy=app_storage::Data()/L"Translation";const auto sources=Sources();
    const std::string old=R"({"version":1,"sources":[{"secret":"not-a-portable-key"}]})";
    app_storage::AtomicWrite(legacy/L"sources.json",old);
    SourceStore store;Check(store.Directory()==app_storage::Root()/L"Data"/L"Translation","translation still SID-scoped");
    Check(store.Load().empty()&&!std::filesystem::exists(store.Directory()/L"sources.json"),"old SID sources were read or migrated");
    auto imported=sources;imported[0].key=L"copied-new-key";store.Save(imported);Equal(SourceStore().Load(),imported);
    Check(Read(legacy/L"sources.json")==old,"ignored old SID file changed");
    std::cout<<"PASS default Data/Translation only; old SID sources ignored\n";
}
void DefaultRead(){
    SourceStore store;auto sources=store.Load();Check(sources.size()==4&&sources[0].key==L"copied-new-key"&&sources[1].enabled&&!sources[2].enabled,"copied folder not immediately usable");
    store.Save(sources);Equal(store.Load(),sources);std::cout<<"PASS copied default Translation readable/writable without SID or DPAPI\n";
}
void Core(const std::filesystem::path& output){
    const auto sources=Sources();SourceStore original(output/L"old installation 中文"/L"Data"/L"Translation");original.Save(sources);Equal(original.Load(),sources);
    const auto bytes=Read(original.Directory()/L"sources.json");auto json=JsonObject::Parse(winrt::to_hstring(bytes));
    Check(json.GetNamedNumber(L"version")==2&&std::wstring(json.GetNamedArray(L"sources").GetObjectAt(0).GetNamedString(L"secret"))==sources[0].key,"portable credentials depend on OS encryption");
    const auto copied=output/L"new computer"/L"Data"/L"Translation";std::filesystem::create_directories(copied);
    std::filesystem::copy_file(original.Directory()/L"sources.json",copied/L"sources.json",std::filesystem::copy_options::overwrite_existing);
    SourceStore imported(copied);Equal(imported.Load(),sources);imported.Save(sources);Equal(imported.Load(),sources);
    // This independently authored v2 file does not call DPAPI at all.
    json.GetNamedArray(L"sources").GetObjectAt(0).SetNamedValue(L"secret",JsonValue::CreateStringValue(L"independent other-machine fixture"));
    app_storage::AtomicWrite(copied/L"sources.json",winrt::to_string(json.Stringify()));Check(imported.Load()[0].key==L"independent other-machine fixture","external portable file rejected");
    for(const auto version:{1,99}){
        json.SetNamedValue(L"version",JsonValue::CreateNumberValue(version));const auto unsupported=winrt::to_string(json.Stringify());app_storage::AtomicWrite(copied/L"sources.json",unsupported);bool rejected=false;
        try{imported.Load();}catch(const winrt::hresult_error&){rejected=true;}
        Check(rejected&&Read(copied/L"sources.json")==unsupported,"unsupported format read, converted or overwritten");
    }
    std::atomic<bool> failed{};auto writer=[&]{try{for(int i=0;i<20;++i)original.Save(sources);}catch(...){failed=true;}};
    std::thread a(writer),b(writer);a.join();b.join();Check(!failed,"concurrent saves collided on temp file");Equal(original.Load(),sources);
    for(const auto& entry:std::filesystem::directory_iterator(original.Directory()))Check(entry.path().extension()!=L".tmp","save left temp file");
    std::cout<<"PASS portable copy, all providers/Unicode/flags, v1/unknown version rejection, atomic concurrent save\n";
}
}
int wmain(int argc,wchar_t** argv){
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        if(argc>1&&std::wstring(argv[1])==L"--default-write")DefaultWrite();
        else if(argc>1&&std::wstring(argv[1])==L"--default-read")DefaultRead();
        else {Check(argc==2,"output directory required");Core(argv[1]);}
        return 0;
    }catch(const winrt::hresult_error& e){std::wcerr<<L"FAIL "<<e.message().c_str()<<L"\n";}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";}
    return 1;
}
