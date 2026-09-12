#pragma once
#include "ocr/ocr_service.h"
namespace capture {
// Evaluation builds only. Configure once, before starting any OCR work.
struct OcrEvaluationOptions {
    std::wstring modelDirectory;
    bool v6{true};
    std::wstring modelName{L"PP-OCRv6 tiny"};
    float threshold{0.2F},boxThreshold{0.4F},unclip{1.4F};
};
struct OcrEvaluationTiming { double loadMs{},detectMs{},recognizeMs{}; size_t loadedWorking{},loadedPrivate{}; };
void ConfigureOcrEvaluation(const OcrEvaluationOptions& options);
OcrEvaluationTiming LastOcrEvaluationTiming();
}
