#pragma once
#include "shared/image/image_document.h"
#include "ocr/ocr_service.h"
#include <functional>
class CaptureTools final {
public:
    CaptureTools();
    ~CaptureTools();
    bool FocusActive();
    void Ocr(capture::Image image);
    void Ocr(capture::OcrRequest request);
    void LongCapture(RECT region,std::shared_ptr<capture::ImageDocument> document,
                     std::function<void(bool)> completion);
    void Record(RECT region,std::function<void(bool)> completion,bool gif=false);
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
