#pragma once
#include "shared/image/image_document.h"
#include "shared/annotation/annotation_renderer.h"
#include <mutex>
namespace capture {
enum class RecordingFormat { Mp4, Gif };
std::wstring DefaultAudioDevice(bool microphone);
struct RecordingOptions {
    RecordingFormat format{RecordingFormat::Mp4};
    int frameRate{15};
    UINT gifDpi{}; // Zero: resolve the capture monitor once when recording starts.
    RECT region{};
    bool systemAudio{true}, microphone{};
    bool waitForStart{};
    std::wstring systemDevice,microphoneDevice,path;
};
enum class RecordingStopReason { Save, Discard, Shutdown };
struct RecordingState {
    std::atomic<bool> stop{false},paused{false},finished{false};
    std::atomic<bool> ready{false},start{false},systemEnabled{true},micEnabled{false};
    std::atomic<bool> cursorEnabled{true};
    std::atomic<int64_t> elapsed{0};
    std::mutex mutex;
    std::wstring error,savedPath;
    std::vector<std::wstring> ownedFiles;
    std::atomic<RecordingStopReason> stopReason{RecordingStopReason::Save};
    void RequestStop(RecordingStopReason reason) {
        auto previous=stopReason.load();
        while(previous!=RecordingStopReason::Discard && !stopReason.compare_exchange_weak(previous,reason)) {}
        stop=true;
    }
    std::shared_ptr<const RecordingAnnotations> annotations;
    // Pointer updates do not copy or invalidate the static annotation document.
    bool laserMode{};
    LaserPointer liveLaser;
};
void RecordScreen(const RecordingOptions& options,RecordingState& state);
// Call only after joining the worker. Returns deletion errors and retained paths.
std::wstring DiscardRecordingFiles(RecordingState& state);
}

