#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <future>

namespace translation {
// Hooks run on their own message thread; they never call into a foreign window.
// Only generation-tagged messages cross back to the UI thread.
class InputWatch {
public:
    static constexpr UINT Mouse=WM_APP+80,Key=WM_APP+81;
    static constexpr UINT CancelCopy=WM_APP+82;
    static constexpr ULONG_PTR InjectionTag=0x50435443;
    ~InputWatch(){Stop();}
    bool Start(HWND window);
    void Stop();
    void Invocation(uint64_t value){invocation_=value;}
    uint64_t Serial()const{return serial_;}
    void BeginCopy(uint64_t request,UINT key='E'){triggerKey_=key;waitingKeys_=true;copyRequest_=request;}
    void KeysReleased(){waitingKeys_=false;}
    void EndCopy(){copyRequest_=0;waitingKeys_=false;}
private:
    static LRESULT CALLBACK MouseHook(int,WPARAM,LPARAM);
    static LRESULT CALLBACK KeyHook(int,WPARAM,LPARAM);
    inline static thread_local InputWatch* current_{};
    std::thread thread_;DWORD threadId_{};HWND window_{};
    std::atomic<uint64_t> invocation_{},serial_{};
    std::atomic<uint64_t> copyRequest_{};
    std::atomic<bool> waitingKeys_{};
    std::atomic<UINT> triggerKey_{'E'};
};
}
