#include "capture/image_editor.h"
#include "ocr/ocr_window.h"
#include "long_capture/long_capture_session.h"
#include "app/capture_tools.h"
#include "shared/ui/capture_ui.h"
#include "recording/recording_session.h"
#include "recording/recording_preview.h"
#include <winrt/base.h>
#include <shellapi.h>
#include <thread>
#include <chrono>
#include <mutex>

struct CaptureTools::Impl {
    std::vector<std::unique_ptr<capture::ToolWindow>> windows;
    std::unique_ptr<capture::LongCaptureSession> session;
    std::unique_ptr<capture::RecordingSession> recording;
    void Collect() { windows.erase(std::remove_if(windows.begin(),windows.end(),[](const auto& w){return !w->Window();}),windows.end()); }
};
CaptureTools::CaptureTools() : impl_(std::make_unique<Impl>()) {}
CaptureTools::~CaptureTools() { Shutdown(); }
void CaptureTools::Shutdown() { impl_->session.reset(); impl_->recording.reset(); impl_->windows.clear(); }
bool CaptureTools::FocusActive() {
    if(impl_->recording && impl_->recording->Active()) { impl_->recording->Focus(); return true; }
    if(impl_->session && impl_->session->Active()) { impl_->session->Focus(); return true; } return false;
}
void CaptureTools::Ocr(capture::Image image) { impl_->Collect(); impl_->windows.push_back(capture::OpenOcr(std::move(image))); }
void CaptureTools::LongCapture(RECT region,std::shared_ptr<capture::ImageDocument> document,std::function<void(bool)> completion) {
    if(FocusActive()) return;
    impl_->Collect(); impl_->session=capture::OpenLongCaptureSession(region,std::move(document),std::move(completion),[this,region](auto doc){impl_->windows.push_back(capture::OpenImageEditor(std::move(doc),region));});
}
void CaptureTools::Record(RECT region,std::function<void(bool)> completion,bool gif) {
    if(FocusActive()) return;
    impl_->Collect();
    if(impl_->recording) impl_->windows.push_back(std::move(impl_->recording));
    impl_->recording=capture::OpenRecordingSession(region,std::move(completion),[this](capture::RecordingResult result) {
        impl_->Collect(); impl_->windows.push_back(capture::OpenRecordingPreview(std::move(result)));
    },gif?capture::RecordingFormat::Gif:capture::RecordingFormat::Mp4);
}

void CaptureTools::Ocr(capture::OcrRequest request) { impl_->Collect(); impl_->windows.push_back(capture::OpenOcr(std::move(request))); }
