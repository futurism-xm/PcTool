#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
namespace capture {
struct OcrLine { double y{}, height{}; std::wstring text; };
void AppendOcrLines(std::vector<OcrLine>& output, const std::vector<OcrLine>& lines,
                    int tileTop, int ownershipTop, int ownershipBottom);
}
