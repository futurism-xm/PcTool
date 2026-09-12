#pragma once
#include "shared/ui/capture_ui.h"
#include "ocr/ocr_service.h"
namespace capture {
std::unique_ptr<ToolWindow> OpenOcr(Image image);
std::unique_ptr<ToolWindow> OpenOcr(OcrRequest request);
}
