#pragma once
#include "translation/translation_provider.h"
#include <array>
#include <algorithm>
#include <mutex>
#include <vector>

namespace translation {
enum class ResultPhase { Idle, Unconfigured, Loading, Success, Failed };
struct SourceCard {
    std::wstring id,name;
    std::optional<SourceKind> kind;
    ResultPhase phase{ResultPhase::Idle};
    bool expanded{};
    std::wstring text;
    bool CanExpand() const { return text.find_first_not_of(L" \t\r\n\v\f\u3000")!=std::wstring::npos; }
    void Toggle(){expanded=CanExpand()&&!expanded;}
};
// Callbacks only own this mailbox, never a window or controller.
class Submission {
    struct Reply { uint64_t generation; std::wstring source; TranslationResult result; bool partial{}; };
    struct Mailbox { std::mutex mutex; std::vector<Reply> replies; };
    std::shared_ptr<Mailbox> mailbox_=std::make_shared<Mailbox>();
    std::shared_ptr<capture::Cancellation> cancel_;
    uint64_t generation_{};
public:
    std::vector<SourceCard> cards;
    std::vector<std::shared_ptr<TranslationProvider>> providers;
    ~Submission(){Cancel();}
    void Cancel(){++generation_;if(cancel_)cancel_->requested=true;cancel_.reset();}
    void Reset(){Cancel();cards.clear();for(const auto& provider:providers){SourceCard card;if(provider){const auto info=provider->Info();card.id=info.id;card.name=info.name;card.kind=info.kind;}cards.push_back(std::move(card));}}
    void Submit(const std::wstring& text){
        Reset();cancel_=std::make_shared<capture::Cancellation>();
        for(size_t i=0;i<cards.size();++i){
            auto& card=cards[i];card.expanded=true;
            if(!providers[i]||!providers[i]->Info().configured){card.phase=ResultPhase::Unconfigured;card.text=L"翻译源待配置";continue;}
            card.phase=ResultPhase::Loading;card.text=L"正在翻译…";
            auto mailbox=mailbox_;const auto generation=generation_;
            const auto id=card.id;
            try{providers[i]->TranslateStreaming(text,cancel_,[mailbox,generation,id](TranslationResult result){
                std::lock_guard<std::mutex> lock(mailbox->mutex);mailbox->replies.push_back({generation,id,std::move(result)});
            },[mailbox,generation,id](std::wstring text){
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                auto previous=std::find_if(mailbox->replies.begin(),mailbox->replies.end(),[&](const auto& r){return r.partial&&r.generation==generation&&r.source==id;});
                if(previous!=mailbox->replies.end())previous->result.text=std::move(text);else mailbox->replies.push_back({generation,id,{std::move(text),{}},true});
            });}catch(...){card.phase=ResultPhase::Failed;card.text=L"翻译源请求失败";}
        }
    }
    bool Poll(){
        std::vector<Reply> replies;{std::lock_guard<std::mutex> lock(mailbox_->mutex);replies.swap(mailbox_->replies);}
        bool changed=false;
        for(auto& reply:replies){if(reply.generation!=generation_)continue;
            auto found=std::find_if(cards.begin(),cards.end(),[&](const auto& card){return card.id==reply.source;});if(found==cards.end())continue;
            auto& card=*found;card.phase=reply.partial?ResultPhase::Loading:reply.result.error.empty()?ResultPhase::Success:ResultPhase::Failed;
            card.text=reply.result.error.empty()?std::move(reply.result.text):std::move(reply.result.error);
            if(!card.CanExpand())card.expanded=false;
            changed=true;
        }return changed;
    }
};
}
