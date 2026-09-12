#pragma once
#include "shared/async/cancellation.h"
#include <functional>
#include <memory>
#include <string>
#include <optional>
namespace translation {
enum class SourceKind;
struct ProviderInfo {std::wstring id,name; bool configured{}; std::optional<SourceKind> kind;};
struct TranslationResult {std::wstring text,error;};
class TranslationProvider {
public:
    using Completion=std::function<void(TranslationResult)>;
    using Progress=std::function<void(std::wstring)>;
    virtual ~TranslationProvider()=default;
    virtual ProviderInfo Info() const=0;
    virtual void Translate(std::wstring text,std::shared_ptr<capture::Cancellation> cancel,
        std::function<void(TranslationResult)> completion)=0;
    virtual void TranslateStreaming(std::wstring text,std::shared_ptr<capture::Cancellation> cancel,Completion completion,Progress){Translate(std::move(text),std::move(cancel),std::move(completion));}
    virtual void Verify(std::shared_ptr<capture::Cancellation> cancel,Completion completion){Translate(L"你好，世界。",std::move(cancel),std::move(completion));}
};
// No provider is registered until an actual translation source is chosen.
}
