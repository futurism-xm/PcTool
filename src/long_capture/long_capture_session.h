#pragma once
#include "shared/ui/capture_ui.h"
#include "long_capture/scroll_stitcher.h"
namespace capture {
enum class LongCaptureMode { Manual, Restoring, Automatic, Waiting, Paused };
struct LongCaptureProgress {
    int height{}; ScrollAnalysis analysis; std::wstring status; bool atEnd{},atTop{};
    LongCaptureMode mode{LongCaptureMode::Manual}; bool inFlight{};
    uint64_t actionId{},completedActionId{}; int restoreTargetY{};
};
class LongCaptureSession : public ToolWindow {
public:
    virtual bool Active() const = 0;
    virtual LongCaptureProgress Progress()const=0;
};
std::unique_ptr<LongCaptureSession> OpenLongCaptureSession(RECT region,std::shared_ptr<ImageDocument> document,
    std::function<void(bool)> completion,std::function<void(std::shared_ptr<ImageDocument>)> open);
}
