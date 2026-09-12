#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace capture {
constexpr size_t kImageBudget = 256ull * 1024 * 1024;
struct Image {
    int width{}, height{};
    std::vector<uint32_t> pixels; // top-down, opaque BGRA
    Image() = default;
    Image(int w, int h);
    bool Empty() const { return pixels.empty(); }
    Image Crop(int x, int y, int w, int h) const;
};
enum class Tool { None, Rectangle, Ellipse, Arrow, Pen, Mosaic, Text, Number };
struct Annotation {
    Tool tool{Tool::None};
    POINT start{}, end{};
    std::vector<POINT> points;
    COLORREF color{RGB(255,70,70)};
    int size{}, blurLevel{}, number{}, minimumTextCharacters{1};
    bool numberTextOnLeft{}, numberTextDirectionLocked{};
    SIZE textBoxSize{};
    std::wstring text;
    std::vector<std::wstring> renderedTextLines;
    // Long-capture fragments preserve vector geometry while clipping each
    // original pixel to one moving/static partition. Zero means ungrouped.
    std::optional<RECT> clip;
    uint64_t fragmentGroup{};
    POINT mosaicGridOrigin{};
};
struct ImageDocument {
    Image image;
    std::vector<Annotation> annotations;
    UINT dpi{96};
};
struct Viewport {
    double zoom{1};
    double x{}, y{}; // scroll offset in original pixels
    POINT ToImage(POINT p, int top) const {
        return {LONG(std::floor(p.x / zoom + x)), LONG(std::floor((p.y-top)/zoom+y))};
    }
    POINT ToClient(POINT p, int top) const {
        return {LONG(std::lround((p.x-x)*zoom)), LONG(std::lround((p.y-y)*zoom))+top};
    }
};
}
