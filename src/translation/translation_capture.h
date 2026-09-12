#pragma once
#include "shared/platform/capture_platform.h"
#include <functional>
namespace translation {
RECT SelectionRect(POINT first,POINT second,RECT bounds);
class TranslationCapture {
public:
    TranslationCapture();
    ~TranslationCapture();
    bool Start(std::function<void(capture::Image)> complete,std::function<void()> cancel);
    void Cancel();
    bool Focus();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
