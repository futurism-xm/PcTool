#pragma once
#include "recording/screen_recorder.h"
#include "recording/recording_preview.h"
namespace capture {
void RecordGif(const RecordingOptions& options,RecordingState& state);
std::unique_ptr<RecordingPreview> OpenGifPreview(RecordingResult result);
}
