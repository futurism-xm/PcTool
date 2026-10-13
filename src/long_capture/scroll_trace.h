#pragma once

// Test-only, in-memory tracing. Define PCTOOL_SCROLL_TRACE for every translation
// unit in an instrumented integration target; production calls compile away.
#ifdef PCTOOL_SCROLL_TRACE
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace capture::scroll_trace {
enum class Stage {
    Input, Dispatch, BaselineReady, CaptureBegin, CaptureEnd, SourceReceive,
    SourcePaint, FrameChanged, StitchBegin, StitchEnd, PreviewBegin, PreviewEnd,
    Publish, Paint, FlattenBegin, FlattenEnd, Cancelled, Completed
};
struct Event {
    Stage stage{};
    uint64_t actionId{},imageVersion{},inputId{};
    int64_t qpc{};
    int direction{},units{},width{},height{},result{};
    bool supplement{};
};
using Callback=void(*)(const Event&) noexcept;
inline std::atomic<Callback> callback{};
inline std::atomic<uint64_t> inputSequence{};
inline int64_t Now() noexcept { LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart; }
inline int64_t Frequency() noexcept { LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return value.QuadPart; }
inline uint64_t NextInputId() noexcept { return ++inputSequence; }
// Install before creating a session and remove after destroying it. A callback
// runs on hook/worker/UI threads: it must be thread-safe and must not call back
// into the session, perform UI operations, or throw.
inline void SetCallback(Callback value) noexcept { callback.store(value,std::memory_order_release); }
inline void Emit(Event event) noexcept {
    if(auto sink=callback.load(std::memory_order_acquire)){
        if(!event.qpc)event.qpc=Now();sink(event);
    }
}
inline void Emit(Stage stage,uint64_t actionId=0,uint64_t imageVersion=0,uint64_t inputId=0,
    int direction=0,int units=0,int width=0,int height=0,int result=0,bool supplement=false) noexcept {
    Emit({stage,actionId,imageVersion,inputId,0,direction,units,width,height,result,supplement});
}
}
#define PCTOOL_SCROLL_EVENT(stage,...) ::capture::scroll_trace::Emit(::capture::scroll_trace::Stage::stage,__VA_ARGS__)
#else
#define PCTOOL_SCROLL_EVENT(stage,...) ((void)0)
#endif
