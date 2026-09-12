#include "shared/async/cancellation.h"
#pragma once
#include "shared/image/image_document.h"
#include <functional>
namespace capture {
struct OcrLanguage { std::wstring tag,name; };
struct OcrTextLine { double x{},y{},width{},height{}; std::wstring text; };
struct OcrParagraph { size_t number{},begin{},end{}; std::vector<OcrTextLine> lines; };
struct OcrDocument { int width{},height{}; std::vector<OcrParagraph> paragraphs; std::wstring text; };
OcrDocument GroupOcrLines(int width,int height,std::vector<OcrTextLine> lines);
void AppendOcrTextLines(std::vector<OcrTextLine>& out,const std::vector<OcrTextLine>& lines,int tileTop,int ownTop,int ownBottom);
std::vector<OcrLanguage> OcrLanguages();
OcrDocument RecognizeDocument(const Image& image,const std::wstring& language,const Cancellation& cancellation);
OcrDocument RecognizeLocalDocument(const Image& image,const Cancellation& cancellation);
std::wstring LocalOcrModelName();
std::wstring RecognizeImage(const Image& image,const std::wstring& language,const Cancellation& cancellation);
constexpr UINT OcrFinishedMessage=WM_APP+79;
struct OcrRequest {
    Image image,background;
    RECT imageBounds{},busyRegion{};
    HWND owner{};
};
}
