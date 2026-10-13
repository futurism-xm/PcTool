#pragma once
#include <cstdint>
#include <optional>
#include <utility>

namespace capture::scroll_input {
constexpr uint64_t SupplementDelayMs=350;
struct Intent {
    int direction{},x{},y{};
    uint64_t time{},serial{};
#ifdef PCTOOL_SCROLL_TRACE
    uint64_t traceInputId{};
#endif
};
inline bool Discarded(const Intent& intent,uint64_t through,int direction) {
    return intent.serial<=through&&(!direction||intent.direction==direction);
}

// The hook/UI mailbox is protected by its owner's mutex. Notification epochs
// prevent an already-posted message from replaying data after pause/cancel.
class Mailbox {
public:
    uint64_t Put(Intent value) {
        const bool notify=!pending_;pending_=value;return notify?epoch_:0;
    }
    std::optional<Intent> Take(uint64_t epoch) {
        if(epoch!=epoch_)return {};
        auto intent=std::exchange(pending_,{});if(intent)++epoch_;return intent;
    }
    bool Pending() const { return pending_.has_value(); }
    void Clear(){++epoch_;pending_.reset();}
    void DiscardThrough(uint64_t serial,int direction=0) {
        if(!pending_||Discarded(*pending_,serial,direction))Clear();
    }
private:
    uint64_t epoch_{1};
    std::optional<Intent> pending_;
};

// Pure scheduling policy: a single latest intent, no elapsed-time expiry.
// A matched completion releases only the corresponding last delivery; cancel
// and failed completions cannot grant that permission.
class Gate {
public:
    void Store(Intent intent){pending_=intent;}
    bool Pending()const{return pending_.has_value();}
    std::optional<Intent> Take(){return std::exchange(pending_,{});}
    void ClearPending(){pending_.reset();}
    void DiscardThrough(uint64_t serial,int direction=0){if(pending_&&Discarded(*pending_,serial,direction))pending_.reset();}
    void Delivered(uint64_t action,uint64_t now){lastAction_=action;lastDelivery_=now;delivered_=true;matchedAction_=0;}
    void Completed(uint64_t action,bool matched){if(action==lastAction_)matchedAction_=matched?action:0;}
    void Cancel(){pending_.reset();matchedAction_=0;lastAction_=0;}
    bool CanDispatch(uint64_t now)const{
        return !delivered_||(matchedAction_&&matchedAction_==lastAction_)||Elapsed(now);
    }
    bool CanSupplement(uint64_t now,int deliveredUnits,bool changed)const{
        return delivered_&&deliveredUnits<120&&!changed&&Elapsed(now);
    }
private:
    bool Elapsed(uint64_t now)const{return now>=lastDelivery_&&now-lastDelivery_>=SupplementDelayMs;}
    std::optional<Intent> pending_;
    uint64_t lastAction_{},lastDelivery_{},matchedAction_{};bool delivered_{};
};
}
