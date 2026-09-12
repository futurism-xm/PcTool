#pragma once
#include "shared/ui/capture_ui.h"
#include "recording/recording_session.h"

namespace capture {
enum PreviewCommand { PreviewPlay=2101, PreviewSeek, PreviewTime, PreviewMute, PreviewSave, PreviewComplete };
struct RecordingPreviewState {
    bool ready{},playing{},muted{},busy{},controlsVisible{};
    int64_t position{},duration{};
    std::wstring error;
};
class RecordingPreview : public ToolWindow {
public:
    virtual RecordingPreviewState State() const=0;
    // The caller has already obtained overwrite consent for this destination.
    virtual void SaveTo(const std::wstring& path)=0;
};
std::unique_ptr<RecordingPreview> OpenRecordingPreview(RecordingResult result);
HRESULT CopyRecordingFile(HWND owner,const std::wstring& path);
}
