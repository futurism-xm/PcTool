#pragma once
#include "shared/ui/capture_ui.h"
#include "recording/screen_recorder.h"
namespace capture {
class RecordingSession : public ToolWindow {
public:
    virtual bool Active() const=0;
};
struct RecordingResult { std::wstring path,error; RecordingFormat format{RecordingFormat::Mp4}; };
std::unique_ptr<RecordingSession> OpenRecordingSession(RECT region,std::function<void(bool)> completion,
    std::function<void(RecordingResult)> result={},RecordingFormat format=RecordingFormat::Mp4);
}
