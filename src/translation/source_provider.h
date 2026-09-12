#pragma once
#include "translation/source_config.h"
#include "translation/translation_provider.h"
#include <vector>
namespace translation {
struct HttpMessage {std::wstring url,headers;std::string body;};
HttpMessage BuildTranslationRequest(const SourceConfig&,const std::wstring& text,bool stream,const std::string& salt={});
TranslationResult ParseTranslationResponse(SourceKind,const std::string& body,unsigned status);
class EventStream {
public:
    void Feed(const std::string& bytes,const TranslationProvider::Progress& progress);
    void Finish(const TranslationProvider::Progress& progress);
    std::wstring text,error;
private:
    std::string pending_,event_;
    void Dispatch(const TranslationProvider::Progress& progress);
};
std::shared_ptr<TranslationProvider> MakeSourceProvider(const SourceConfig&);
// An explicit transport seam for protocol tests; production always uses Windows HTTP.
using HttpTransport=std::function<std::pair<unsigned,std::string>(const HttpMessage&,std::shared_ptr<capture::Cancellation>,std::function<void(const std::string&)>)>;
std::pair<unsigned,std::string> SendTranslationHttp(const HttpMessage&,std::shared_ptr<capture::Cancellation>,std::function<void(const std::string&)>);
std::shared_ptr<TranslationProvider> MakeSourceProviderForTest(const SourceConfig&,HttpTransport);
}
