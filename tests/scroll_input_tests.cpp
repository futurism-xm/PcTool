#include "long_capture/scroll_input.h"
#include <iostream>
#include <stdexcept>
using namespace capture::scroll_input;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Intent Wheel(int direction,uint64_t time,uint64_t serial){return {direction,10,20,time,serial};}
int main(){try{
    Gate gate;
    gate.Store(Wheel(-1,0,1));Check(gate.Pending()&&gate.CanDispatch(0),"first intent waits only for owner's baseline readiness");
    Check(gate.Take()->serial==1&&!gate.Pending(),"first intent consumed once");gate.Delivered(1,0);
    gate.Store(Wheel(-1,100,2));Check(!gate.CanDispatch(100),"unfinished action still gated");
    gate.Completed(1,true);Check(gate.CanDispatch(220),"matched action releases before 350ms");
    auto next=gate.Take();Check(next&&next->serial==2&&!gate.Take(),"t100 intent survives until t220 exactly once");
    gate.Delivered(2,220);gate.Completed(2,true);Check(gate.CanDispatch(250),"matched permission persists until a later gesture");
    gate.Delivered(3,250);Check(!gate.CanDispatch(300),"next delivery consumes prior matched permission");
    gate.Completed(2,true);Check(!gate.CanDispatch(300),"stale completion cannot release new action");
    gate.Cancel();gate.Completed(3,false);Check(!gate.CanDispatch(300)&&gate.CanDispatch(600),"cancel/failure cannot masquerade as a match");
    gate.Completed(3,true);Check(!gate.CanDispatch(300),"late matched completion cannot release a cancelled action");

    Gate discrete;discrete.Delivered(1,0);discrete.Store(Wheel(-1,100,1));
    Check(!discrete.CanSupplement(349,30,false)&&discrete.CanSupplement(350,30,false),"full-notch supplement waits 350ms");
    Check(!discrete.CanSupplement(350,30,true),"observed movement blocks supplement");
    discrete.Delivered(1,350);Check(!discrete.CanSupplement(700,120,false),"120-unit notch cannot repeat");
    discrete.Completed(1,true);Check(discrete.CanDispatch(550)&&discrete.Take()->time==100,"discrete t100 intent survives t350 supplement and t550 match");

    Gate burst;burst.Delivered(1,0);
    for(uint64_t i=1;i<=40;++i)burst.Store(Wheel(-1,100+i,i));
    burst.Store(Wheel(1,150,41));burst.Completed(1,true);
    Check(burst.Take()->direction==1&&!burst.Pending(),"burst coalesces to latest reverse intent");
    burst.Store(Wheel(1,160,42));burst.Store(Wheel(-1,170,43));Check(burst.Take()->serial==43,"newer direction wins over older reversal");
    burst.Store(Wheel(-1,180,44));burst.DiscardThrough(44,-1);Check(!burst.Pending(),"boundary discards old same-direction intent");
    burst.Store(Wheel(1,180,44));burst.DiscardThrough(44,-1);Check(burst.Pending(),"boundary preserves pending reverse intent");
    burst.Store(Wheel(-1,220,45));burst.DiscardThrough(44,-1);Check(burst.Pending(),"fresh same-direction input after boundary can redisplay hint");
    burst.DiscardThrough(45);Check(!burst.Pending(),"failure discards all older input");
    burst.Store(Wheel(-1,230,46));burst.DiscardThrough(45);Check(burst.Pending(),"fresh input after failure can explicitly retry");
    burst.Cancel();Check(!burst.Pending(),"pause/finish clears queued intent");

    Mailbox mailbox;const auto old=mailbox.Put(Wheel(-1,0,1));
    Check(old&&!mailbox.Put(Wheel(1,10,2)),"one posted notice coalesces hook events");
    Check(mailbox.Take(old)->serial==2&&!mailbox.Take(old),"duplicate notice cannot replay last sample");
    const auto subsequent=mailbox.Put(Wheel(-1,15,3));
    Check(!mailbox.Take(old)&&mailbox.Pending(),"duplicate consumed notice cannot consume a later gesture");
    Check(mailbox.Take(subsequent)->time==15,"later gesture retains its own notice");
    const auto stale=mailbox.Put(Wheel(-1,20,3));mailbox.Clear();const auto fresh=mailbox.Put(Wheel(1,30,4));
    Check(fresh!=stale&&!mailbox.Take(stale)&&mailbox.Pending(),"late paused notice cannot consume fresh input");
    Check(mailbox.Take(fresh)->serial==4,"fresh generation remains deliverable");
    const auto boundary=mailbox.Put(Wheel(-1,40,5));mailbox.DiscardThrough(5,-1);Check(!mailbox.Take(boundary),"queued hook input cleared at boundary");
    const auto reverse=mailbox.Put(Wheel(1,50,6));mailbox.DiscardThrough(6,-1);Check(mailbox.Take(reverse)->direction==1,"reverse hook input survives boundary");
    const auto afterFailure=mailbox.Put(Wheel(-1,60,8));mailbox.DiscardThrough(7);Check(mailbox.Take(afterFailure)->serial==8,"failure cutoff preserves genuinely newer hook input");
    std::cout<<"SCROLL INPUT PASS: matched handoff, t0/t100/t350, bounded latest intent, reversal, boundary/failure cutoffs, pause/cancel epochs and supplement timing\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
