#pragma once
#include "shared/image/image_document.h"
#include <objidl.h>
#include <gdiplus.h>
namespace capture::text_layout {
inline void AddRoundedRectangle(
    Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    radius = std::clamp(radius, 0.0F,
        std::min(rect.Width, rect.Height) / 2.0F);
    const float diameter = radius * 2.0F;
    if (diameter <= 0.0F) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0F, 90.0F);
    path.AddArc(rect.GetRight() - diameter, rect.Y,
        diameter, diameter, 270.0F, 90.0F);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
        diameter, diameter, 0.0F, 90.0F);
    path.AddArc(rect.X, rect.GetBottom() - diameter,
        diameter, diameter, 90.0F, 90.0F);
    path.CloseFigure();
}

inline std::vector<std::wstring> WrapTextToGdiPixelWidth(
    HDC deviceContext, const std::wstring& value, int maximumWidth) {
    std::vector<std::wstring> lines;
    std::wstring line;
    maximumWidth = std::max(1, maximumWidth);
    for (std::size_t index = 0; index < value.size();) {
        if (value[index] == L'\r') {
            ++index;
            continue;
        }
        if (value[index] == L'\n') {
            lines.push_back(line);
            line.clear();
            ++index;
            continue;
        }
        int characterLength = 1;
        if (index + 1 < value.size() &&
            value[index] >= 0xD800 && value[index] <= 0xDBFF &&
            value[index + 1] >= 0xDC00 && value[index + 1] <= 0xDFFF) {
            characterLength = 2;
        }
        std::wstring candidate = line;
        candidate.append(value.data() + index,
            static_cast<std::size_t>(characterLength));
        SIZE extent{};
        GetTextExtentPoint32W(deviceContext, candidate.data(),
            static_cast<int>(candidate.size()), &extent);
        if (!line.empty() && extent.cx > maximumWidth) {
            lines.push_back(line);
            line.assign(value.data() + index,
                static_cast<std::size_t>(characterLength));
        } else {
            line = std::move(candidate);
        }
        index += static_cast<std::size_t>(characterLength);
    }
    if (!line.empty() || lines.empty() ||
        (!value.empty() && value.back() == L'\n')) {
        lines.push_back(std::move(line));
    }
    return lines;
}

template <typename MeasureLine>
int LongestLogicalLineWidth(
    const std::wstring& text, MeasureLine measureLine) {
    int width = 0;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t lineEnd = text.find(L'\n', lineStart);
        std::size_t lineLength =
            (lineEnd == std::wstring::npos ? text.size() : lineEnd) -
            lineStart;
        if (lineLength > 0 && text[lineStart + lineLength - 1] == L'\r') {
            --lineLength;
        }
        width = std::max(width, measureLine(
            text.data() + lineStart, static_cast<int>(lineLength)));
        if (lineEnd == std::wstring::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    return width;
}

class ScopedGdiTextMeasurement final {
public:
    explicit ScopedGdiTextMeasurement(int fontSize) {
        context_ = CreateCompatibleDC(nullptr);
        font_ = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (context_ && font_) {
            previousFont_ = SelectObject(context_, font_);
            if (previousFont_ == HGDI_ERROR) {
                previousFont_ = nullptr;
            }
        }
    }

    ~ScopedGdiTextMeasurement() {
        if (previousFont_) {
            SelectObject(context_, previousFont_);
        }
        if (font_) {
            DeleteObject(font_);
        }
        if (context_) {
            DeleteDC(context_);
        }
    }

    ScopedGdiTextMeasurement(const ScopedGdiTextMeasurement&) = delete;
    ScopedGdiTextMeasurement& operator=(
        const ScopedGdiTextMeasurement&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept {
        return previousFont_ != nullptr;
    }

    [[nodiscard]] HDC Context() const noexcept {
        return context_;
    }

private:
    HDC context_{};
    HFONT font_{};
    HGDIOBJ previousFont_{};
};

inline std::vector<std::wstring> WrapTextToPixelWidth(
    Gdiplus::Graphics& graphics, const Gdiplus::Font& font,
    const std::wstring& text, float maximumWidth) {
    maximumWidth = std::max(1.0F, maximumWidth);
    Gdiplus::StringFormat format(
        Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() |
        Gdiplus::StringFormatFlagsNoWrap);

    const auto measure = [&](const wchar_t* value, int length) {
        if (!value || length <= 0) return 0.0F;
        Gdiplus::RectF measured{};
        graphics.MeasureString(value, length, &font,
            Gdiplus::PointF(0.0F, 0.0F), &format, &measured);
        return std::max(1.0F, measured.Width);
    };

    std::vector<std::wstring> lines;
    std::wstring line;
    float lineWidth = 0.0F;
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == L'\r') {
            ++index;
            continue;
        }
        if (text[index] == L'\n') {
            lines.push_back(line);
            line.clear();
            lineWidth = 0.0F;
            ++index;
            continue;
        }

        int characterLength = 1;
        if (index + 1 < text.size() &&
            text[index] >= 0xD800 && text[index] <= 0xDBFF &&
            text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) {
            characterLength = 2;
        }
        const float characterWidth = measure(
            text.data() + index, characterLength);
        if (!line.empty() && lineWidth + characterWidth > maximumWidth) {
            lines.push_back(line);
            line.clear();
            lineWidth = 0.0F;
        }
        line.append(text.data() + index,
            static_cast<std::size_t>(characterLength));
        lineWidth += characterWidth;
        index += static_cast<std::size_t>(characterLength);
    }
    if (!line.empty() || lines.empty() ||
        (!text.empty() && text.back() == L'\n')) {
        lines.push_back(std::move(line));
    }
    return lines;
}

inline float WrappedTextHeight(Gdiplus::Graphics& graphics,
    const Gdiplus::Font& font, const std::wstring& text,
    float maximumWidth) {
    const auto lines = WrapTextToPixelWidth(
        graphics, font, text, maximumWidth);
    return std::max(1.0F, font.GetHeight(&graphics)) *
        static_cast<float>(std::max<std::size_t>(1, lines.size()));
}

}
