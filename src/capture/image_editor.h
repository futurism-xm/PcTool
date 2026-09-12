#pragma once
#include "shared/ui/capture_ui.h"
#include <optional>
namespace capture {
std::unique_ptr<ToolWindow> OpenImageEditor(std::shared_ptr<ImageDocument> document,std::optional<RECT> captureRegion={});
Image RenderDocument(const ImageDocument& document);
}
