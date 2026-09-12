#pragma once
#include <algorithm>
#include <cstdint>
namespace capture {
// All values use Media Foundation's 100 ns clock; paused time is excluded.
class Timeline {
public:
    void Start(int64_t now) { start_=now; pausedTotal_=0; paused_=false; }
    void Pause(int64_t now) { if (!paused_) { pauseStart_=now; paused_=true; } }
    void Resume(int64_t now) { if (paused_) { pausedTotal_+=now-pauseStart_; paused_=false; } }
    int64_t Elapsed(int64_t now) const { return std::max<int64_t>(0,(paused_?pauseStart_:now)-start_-pausedTotal_); }
private:
    int64_t start_{}, pauseStart_{}, pausedTotal_{};
    bool paused_{};
};
inline float MixSample(float a, float b) { return std::clamp(a+b,-1.0f,1.0f); }
}
