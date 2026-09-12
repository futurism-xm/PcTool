#include "shared/annotation/annotation_painter.h"
#include "shared/annotation/text_layout.h"
namespace capture {
using namespace text_layout;
namespace {
constexpr int kDefaultTextEditorWidthDip=120;
constexpr BYTE kAnnotationTextQuality=ANTIALIASED_QUALITY;
int RectWidth(RECT r) { return r.right-r.left; }
int RectHeight(RECT r) { return r.bottom-r.top; }
RECT NormalizeRect(POINT a, POINT b) {return {std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)};}
struct ScopedAnnotationClip {
    HDC dc; int saved{};
    ScopedAnnotationClip(HDC context,const Annotation& a,int x,int y):dc(context) {
        if(a.clip){saved=SaveDC(dc);IntersectClipRect(dc,a.clip->left+x,a.clip->top+y,a.clip->right+x,a.clip->bottom+y);}
    }
    ~ScopedAnnotationClip(){if(saved)RestoreDC(dc,saved);}
};
}
void PaintMosaic(HDC dc,const Annotation& annotation,UINT dpi,int offsetX,int offsetY) {
    if(annotation.points.empty())return;
    const ScopedAnnotationClip clipping(dc,annotation,offsetX,offsetY);
    RECT sampleBounds{};
    if(GetClipBox(dc,&sampleBounds)==NULLREGION||sampleBounds.left>=sampleBounds.right||sampleBounds.top>=sampleBounds.bottom)return;
    const auto scale=[dpi](int value){return MulDiv(value,int(dpi),96);};
    const int diameter=std::max(scale(6),annotation.size);
    const int blockSize=std::min(std::max(scale(2),scale(2+annotation.blurLevel*2)),std::max(scale(2),diameter/2));
    const int radius=diameter/2,step=std::max(1,std::min(blockSize/2,diameter/10));
    const POINT origin{annotation.mosaicGridOrigin.x+offsetX,annotation.mosaicGridOrigin.y+offsetY};
    const auto align=[blockSize](int value,int start){const int remainder=((value-start)%blockSize+blockSize)%blockSize;return value-remainder;};
    const auto stamp=[&](POINT center) {
        HBRUSH brush=static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        for(int y=align(center.y-radius,origin.y);y<=center.y+radius;y+=blockSize)
            for(int x=align(center.x-radius,origin.x);x<=center.x+radius;x+=blockSize) {
                const int sampleX=x+blockSize/2,sampleY=y+blockSize/2;
                const int dx=sampleX-center.x,dy=sampleY-center.y;
                if(dx*dx+dy*dy>radius*radius)continue;
                RECT block{x,y,x+blockSize,y+blockSize},visible{};
                if(!IntersectRect(&visible,&block,&sampleBounds))continue;
                // A boundary cell must sample this partition, never an
                // appended background gap or another independently moved part.
                const COLORREF color=GetPixel(dc,std::clamp(sampleX,int(visible.left),int(visible.right)-1),
                    std::clamp(sampleY,int(visible.top),int(visible.bottom)-1));
                if(color==CLR_INVALID)continue;
                SetDCBrushColor(dc,color);FillRect(dc,&block,brush);
            }
    };
    POINT previous{annotation.points.front().x+offsetX,annotation.points.front().y+offsetY};stamp(previous);
    for(size_t i=1;i<annotation.points.size();++i) {
        const POINT current{annotation.points[i].x+offsetX,annotation.points[i].y+offsetY};
        const int dx=current.x-previous.x,dy=current.y-previous.y;
        const int distance=int(std::ceil(std::hypot(double(dx),double(dy)))),count=std::max(1,(distance+step-1)/step);
        for(int part=1;part<=count;++part)stamp({LONG(std::lround(previous.x+double(dx)*part/count)),LONG(std::lround(previous.y+double(dy)*part/count))});
        previous=current;
    }
}
struct AnnotationPainter::TextLayoutContext {
    TextLayoutContext(int fontSizeValue, int diameterValue,
        int selectionWidthValue, int selectionHeightValue,
        int rightContentEdgeValue, int minimumCharacterCount,
        const std::wstring& text)
        : fontSize(fontSizeValue),
          diameter(diameterValue),
          selectionWidth(selectionWidthValue),
          selectionHeight(selectionHeightValue),
          rightContentEdge(rightContentEdgeValue),
          graphics(&measurementSurface),
          font(&fontFamily, static_cast<float>(fontSizeValue),
              Gdiplus::FontStyleRegular, Gdiplus::UnitPixel),
          naturalFormat(Gdiplus::StringFormat::GenericTypographic()) {
        naturalFormat.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        naturalWidth = LongestLogicalLineWidth(text,
            [this](const wchar_t* line, int length) {
                return MeasureLine(line, length);
            });
        const wchar_t minimumSample[] = L"文文文";
        minimumTextWidth = std::max(
            fontSize * minimumCharacterCount,
            MeasureLine(minimumSample, minimumCharacterCount));
    }

    [[nodiscard]] int MeasureLine(const wchar_t* text, int length) {
        if (!text || length <= 0) {
            return 0;
        }
        Gdiplus::RectF measured{};
        graphics.MeasureString(text, length, &font,
            Gdiplus::PointF(0.0F, 0.0F), &naturalFormat, &measured);
        return static_cast<int>(std::ceil(measured.Width));
    }

    int fontSize{};
    int diameter{};
    int selectionWidth{};
    int selectionHeight{};
    int rightContentEdge{};
    int naturalWidth{};
    int minimumTextWidth{};
    Gdiplus::Bitmap measurementSurface{1, 1};
    Gdiplus::Graphics graphics;
    Gdiplus::FontFamily fontFamily{L"Microsoft YaHei UI"};
    Gdiplus::Font font;
    Gdiplus::StringFormat naturalFormat;
};

void AnnotationPainter::PaintAnnotation(
    HDC deviceContext, const Annotation& annotation,
    int offsetX, int offsetY, bool showEmptyNumberLabel) const {
    if (annotation.tool == Tool::Mosaic) {
        return;
    }
    const ScopedAnnotationClip clip(deviceContext,annotation,offsetX,offsetY);

    const int annotationSize = std::max(1, annotation.size);
    const POINT start{annotation.start.x + offsetX, annotation.start.y + offsetY};
    const POINT end{annotation.end.x + offsetX, annotation.end.y + offsetY};
    const Gdiplus::Color vectorColor(255, GetRValue(annotation.color),
        GetGValue(annotation.color), GetBValue(annotation.color));
    Gdiplus::Graphics graphics(deviceContext);
    if(annotation.clip)graphics.SetClip(Gdiplus::Rect(annotation.clip->left+offsetX,
        annotation.clip->top+offsetY,annotation.clip->right-annotation.clip->left,
        annotation.clip->bottom-annotation.clip->top),Gdiplus::CombineModeIntersect);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    // Linear channel blending matches the desktop compositor and transparent recording layer.
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityAssumeLinear);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::Pen pen(vectorColor, static_cast<float>(annotationSize));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    switch (annotation.tool) {
    case Tool::Rectangle: {
        RECT rect = NormalizeRect(start, end);
        graphics.DrawRectangle(&pen, static_cast<float>(rect.left),
            static_cast<float>(rect.top), static_cast<float>(RectWidth(rect)),
            static_cast<float>(RectHeight(rect)));
        break;
    }
    case Tool::Ellipse: {
        RECT rect = NormalizeRect(start, end);
        graphics.DrawEllipse(&pen, static_cast<float>(rect.left),
            static_cast<float>(rect.top), static_cast<float>(RectWidth(rect)),
            static_cast<float>(RectHeight(rect)));
        break;
    }
    case Tool::Arrow: {
        graphics.DrawLine(&pen, static_cast<float>(start.x),
            static_cast<float>(start.y), static_cast<float>(end.x),
            static_cast<float>(end.y));
        const double angle = std::atan2(
            static_cast<double>(end.y - start.y),
            static_cast<double>(end.x - start.x));
        const double wing = 0.55;
        const double length = static_cast<double>(std::max(Scale(10), annotationSize * 3));
        const Gdiplus::PointF first{
            static_cast<float>(end.x - std::cos(angle - wing) * length),
            static_cast<float>(end.y - std::sin(angle - wing) * length)};
        const Gdiplus::PointF second{
            static_cast<float>(end.x - std::cos(angle + wing) * length),
            static_cast<float>(end.y - std::sin(angle + wing) * length)};
        graphics.DrawLine(&pen, static_cast<float>(end.x),
            static_cast<float>(end.y), first.X, first.Y);
        graphics.DrawLine(&pen, static_cast<float>(end.x),
            static_cast<float>(end.y), second.X, second.Y);
        break;
    }
    case Tool::Pen: {
        if (annotation.points.size() >= 2) {
            std::vector<Gdiplus::PointF> points;
            points.reserve(annotation.points.size());
            for (const POINT& point : annotation.points) {
                points.emplace_back(static_cast<float>(point.x + offsetX),
                    static_cast<float>(point.y + offsetY));
            }
            graphics.DrawLines(&pen, points.data(),
                static_cast<INT>(points.size()));
        }
        break;
    }
    case Tool::Text: {
        const RECT textRect = NormalizeRect(start, end);
        const int width=RectWidth(textRect),height=RectHeight(textRect);
        if(width<=0 || height<=0) break;
        // Native EDIT and DrawText use identical advances and line heights.
        // Recover grayscale glyph coverage separately from the text color;
        // this remains smooth on both opaque screenshots and alpha overlays.
        BITMAPINFO info{}; info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};
        void* pixels=nullptr; HBITMAP mask=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!mask) break;
        HDC dc=CreateCompatibleDC(deviceContext); auto old=SelectObject(dc,mask);
        RECT local{0,0,width,height}; FillRect(dc,&local,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        HFONT font=CreateFontW(-annotationSize,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei UI");
        auto oldFont=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
        DrawTextW(dc,annotation.text.c_str(),static_cast<int>(annotation.text.size()),&local,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX);
        GdiFlush(); auto* data=static_cast<uint32_t*>(pixels);
        for(size_t i=0;i<size_t(width)*height;++i) {
            const unsigned alpha=data[i]&255;
            data[i]=(alpha<<24)|((GetRValue(annotation.color)*alpha/255)<<16)|((GetGValue(annotation.color)*alpha/255)<<8)|(GetBValue(annotation.color)*alpha/255);
        }
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        AlphaBlend(deviceContext,textRect.left,textRect.top,width,height,dc,0,0,width,height,blend);
        SelectObject(dc,oldFont); DeleteObject(font); SelectObject(dc,old); DeleteDC(dc); DeleteObject(mask);        break;
    }
    case Tool::Number: {
        const int diameter = std::max(
            Scale(18), annotationSize + Scale(8));
        const float radius = diameter / 2.0F;
        Gdiplus::SolidBrush badgeBrush(vectorColor);
        graphics.FillEllipse(&badgeBrush, start.x - radius, start.y - radius,
            static_cast<float>(diameter), static_cast<float>(diameter));

        wchar_t numberText[16]{};
        swprintf_s(numberText, L"%d", annotation.number);
        Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
        Gdiplus::Font badgeFont(&fontFamily,
            static_cast<float>(std::max(1, annotationSize)),
            Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font labelMeasureFont(&fontFamily,
            static_cast<float>(std::max(1, annotationSize)),
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::StringFormat centeredFormat;
        centeredFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
        centeredFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        const Gdiplus::RectF badgeText(start.x - radius, start.y - radius,
            static_cast<float>(diameter), static_cast<float>(diameter));
        graphics.DrawString(numberText, -1, &badgeFont,
            badgeText, &centeredFormat, &whiteBrush);

        if (annotation.textBoxSize.cx > 0 || showEmptyNumberLabel) {
            const bool onLeft = annotation.numberTextOnLeft;
            const float pointerTip = start.x +
                (onLeft ? -1.0F : 1.0F) * (radius + Scale(3));
            const float pointerWidth = static_cast<float>(Scale(8));
            const float horizontalPadding = static_cast<float>(Scale(8));
            const float editorWidth = annotation.textBoxSize.cx > 0
                ? static_cast<float>(annotation.textBoxSize.cx)
                : static_cast<float>(Scale(180));
            const float bodyWidth = std::max(1.0F,
                editorWidth - pointerWidth);
            const float bodyLeft = onLeft
                ? pointerTip - editorWidth
                : pointerTip + pointerWidth;
            const float pointerBase = onLeft
                ? pointerTip - pointerWidth
                : pointerTip + pointerWidth;
            const float textWidth = std::max(1.0F,
                bodyWidth - horizontalPadding * 2.0F);
            const float wrappedTextHeight = WrappedTextHeight(
                graphics, labelMeasureFont, annotation.text, textWidth);
            const float labelHeight = annotation.textBoxSize.cy > 0
                ? std::max(static_cast<float>(diameter),
                    static_cast<float>(annotation.textBoxSize.cy))
                : std::max(static_cast<float>(diameter),
                    wrappedTextHeight +
                        static_cast<float>(Scale(5) * 2));
            const float labelTop = annotation.textBoxSize.cy > 0
                ? static_cast<float>(annotation.end.y + offsetY)
                : start.y - labelHeight / 2.0F;
            const Gdiplus::Color labelBackground(255, 105, 105, 105);
            Gdiplus::SolidBrush backgroundBrush(labelBackground);
            const Gdiplus::PointF pointer[3]{
                Gdiplus::PointF(pointerTip, static_cast<float>(start.y)),
                Gdiplus::PointF(pointerBase,
                    static_cast<float>(start.y - Scale(6))),
                Gdiplus::PointF(pointerBase,
                    static_cast<float>(start.y + Scale(6)))};
            graphics.FillPolygon(&backgroundBrush, pointer, 3);
            Gdiplus::GraphicsPath backgroundPath;
            AddRoundedRectangle(backgroundPath,
                Gdiplus::RectF(bodyLeft, labelTop, bodyWidth, labelHeight),
                static_cast<float>(Scale(4)));
            graphics.FillPath(&backgroundBrush, &backgroundPath);
            const float verticalPadding = static_cast<float>(Scale(5));
            const Gdiplus::RectF labelText(
                bodyLeft + horizontalPadding,
                labelTop + verticalPadding,
                textWidth,
                std::max(0.0F, labelHeight - verticalPadding * 2.0F));
            const int saved = SaveDC(deviceContext);
            RECT textRect{
                static_cast<LONG>(std::lround(labelText.X)),
                static_cast<LONG>(std::lround(labelText.Y)),
                static_cast<LONG>(std::lround(labelText.GetRight())),
                static_cast<LONG>(std::lround(labelText.GetBottom()))};
            IntersectClipRect(deviceContext, textRect.left, textRect.top,
                textRect.right, textRect.bottom);
            HFONT labelFont = CreateFontW(-annotationSize,
                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, kAnnotationTextQuality,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HGDIOBJ oldFont = labelFont
                ? SelectObject(deviceContext, labelFont) : nullptr;
            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(255, 255, 255));
            TEXTMETRICW metrics{};
            const bool hasMetrics =
                GetTextMetricsW(deviceContext, &metrics) != FALSE;
            const int lineHeight = std::max(annotationSize + Scale(3),
                hasMetrics
                    ? static_cast<int>(metrics.tmHeight +
                        metrics.tmExternalLeading)
                    : 0);
            std::vector<std::wstring> visualLines =
                annotation.renderedTextLines;
            if (visualLines.empty()) {
                visualLines = WrapTextToGdiPixelWidth(
                    deviceContext, annotation.text,
                    std::max(1, RectWidth(textRect)));
            }
            const int contentHeight = static_cast<int>(visualLines.size()) *
                std::max(1, lineHeight);
            int lineTop = textRect.top;
            if (contentHeight < RectHeight(textRect)) {
                lineTop += (RectHeight(textRect) - contentHeight) / 2;
            }
            for (const std::wstring& line : visualLines) {
                if (lineTop + lineHeight > textRect.bottom) {
                    break;
                }
                RECT lineRect{textRect.left, lineTop,
                    textRect.right, lineTop + lineHeight};
                DrawTextW(deviceContext, line.c_str(),
                    static_cast<int>(line.size()), &lineRect,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                lineTop += lineHeight;
            }
            if (oldFont) SelectObject(deviceContext, oldFont);
            if (labelFont) DeleteObject(labelFont);
            RestoreDC(deviceContext, saved);
        }
        break;
    }
    case Tool::Mosaic:
    case Tool::None:
        break;
    }
}

void AnnotationPainter::AdaptTextAnnotationLayout(Annotation& annotation) const {
    if (annotation.tool != Tool::Text && annotation.tool != Tool::Number) {
        return;
    }

    const int diameter = std::max(
        Scale(18), annotation.size + Scale(8));
    const int fontSize = std::max(1, annotation.size);
    const int minimumCharacterCount = std::clamp(
        annotation.minimumTextCharacters, 1, 3);
    const int selectionWidth = std::max(1, RectWidth(selection_));
    const int selectionHeight = std::max(1, RectHeight(selection_));
    const int rightContentEdge = std::max(1,
        selectionWidth - Scale(2));
    TextLayoutContext context(fontSize, diameter, selectionWidth,
        selectionHeight, rightContentEdge, minimumCharacterCount,
        annotation.text);

    if (annotation.tool == Tool::Number) {
        AdaptNumberTextAnnotationLayout(annotation, context);
    } else {
        AdaptPlainTextAnnotationLayout(annotation, context);
    }
}

void AnnotationPainter::AdaptNumberTextAnnotationLayout(
    Annotation& annotation, TextLayoutContext& context) const {
    annotation.renderedTextLines.clear();
    const int pointerGap = Scale(3);
    const int pointerWidth = Scale(8);
    const int horizontalPadding = Scale(8);
    const int radius = (context.diameter + 1) / 2;
    ScopedGdiTextMeasurement measurement(context.fontSize);
    int minimumLineHeight = context.fontSize + Scale(3);
    int numberNaturalWidth = context.naturalWidth;
    int oneCharacterWidth = std::max(
        context.fontSize, context.MeasureLine(L"文", 1));
    if (measurement.IsAvailable()) {
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(measurement.Context(), &metrics)) {
            minimumLineHeight = std::max(minimumLineHeight,
                static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
        }
        SIZE characterExtent{};
        if (GetTextExtentPoint32W(
                measurement.Context(), L"文", 1, &characterExtent)) {
            oneCharacterWidth = std::max(
                context.fontSize, static_cast<int>(characterExtent.cx));
        }
        numberNaturalWidth = LongestLogicalLineWidth(annotation.text,
            [&measurement](const wchar_t* line, int length) {
                SIZE extent{};
                return length > 0 && GetTextExtentPoint32W(
                    measurement.Context(), line, length, &extent)
                    ? static_cast<int>(extent.cx) : 0;
            });
    }

    const int verticalPadding = Scale(5) * 2;
    const int minimumUsableEditorWidth = pointerWidth +
        horizontalPadding * 2 + oneCharacterWidth;
    const int minimumEditorWidth = std::max(
        minimumUsableEditorWidth, Scale(kDefaultTextEditorWidthDip));
    const int maximumBadgeX = std::max(
        radius, context.selectionWidth - radius);
    int clampedAnchorX = std::clamp<int>(
        annotation.start.x, radius, maximumBadgeX);
    int clampedAnchorY = context.selectionHeight / 2;
    if (context.selectionHeight >= context.diameter) {
        clampedAnchorY = std::clamp<int>(annotation.start.y,
            radius, context.selectionHeight - radius);
    }
    const int leftAvailable = std::max(
        1, clampedAnchorX - radius - pointerGap);
    const int rightAvailable = std::max(1,
        context.rightContentEdge - clampedAnchorX - radius - pointerGap);
    if (!annotation.numberTextDirectionLocked) {
        if (rightAvailable < minimumUsableEditorWidth &&
            leftAvailable >= minimumUsableEditorWidth) {
            annotation.numberTextOnLeft = true;
        }
        annotation.numberTextDirectionLocked = true;
    }

    if (annotation.numberTextOnLeft) {
        const int minimumAnchorX =
            radius + pointerGap + minimumUsableEditorWidth;
        if (minimumAnchorX <= maximumBadgeX) {
            clampedAnchorX = std::clamp(
                clampedAnchorX, minimumAnchorX, maximumBadgeX);
        }
    } else {
        const int maximumAnchorX = context.rightContentEdge - radius -
            pointerGap - minimumUsableEditorWidth;
        if (radius <= maximumAnchorX) {
            clampedAnchorX = std::clamp(
                clampedAnchorX, radius, maximumAnchorX);
        }
    }
    OffsetAnnotation(annotation,
        clampedAnchorX - annotation.start.x,
        clampedAnchorY - annotation.start.y);

    const int availableWidth = annotation.numberTextOnLeft
        ? std::max(1, static_cast<int>(annotation.start.x) -
            radius - pointerGap)
        : std::max(1, context.rightContentEdge -
            static_cast<int>(annotation.start.x) - radius - pointerGap);
    const int desiredEditorWidth = std::min(availableWidth,
        std::max(minimumEditorWidth,
            pointerWidth + numberNaturalWidth + horizontalPadding * 2));
    const int textWidth = std::max(1,
        desiredEditorWidth - pointerWidth - horizontalPadding * 2);
    std::vector<std::wstring> wrappedLines = measurement.IsAvailable()
        ? WrapTextToGdiPixelWidth(
            measurement.Context(), annotation.text, textWidth)
        : WrapTextToPixelWidth(context.graphics, context.font,
            annotation.text, static_cast<float>(textWidth));
    const int requestedLineCount = std::max(
        1, static_cast<int>(wrappedLines.size()));
    // The body may shift vertically around the fixed badge so it can use
    // all available height. Its height still contains whole lines only.
    const int maximumLabelHeight = context.selectionHeight;
    const int availableLineCount = std::max(0,
        (maximumLabelHeight - verticalPadding) / minimumLineHeight);
    const int visibleLineCount = std::min(
        requestedLineCount, availableLineCount);
    const int desiredHeight = std::min(maximumLabelHeight,
        std::max(context.diameter,
            verticalPadding + visibleLineCount * minimumLineHeight));
    const int labelTop = std::clamp<int>(
        annotation.start.y - desiredHeight / 2,
        0, std::max(0, context.selectionHeight - desiredHeight));
    annotation.textBoxSize = SIZE{desiredEditorWidth, desiredHeight};
    annotation.end.y = labelTop;
    annotation.renderedTextLines = std::move(wrappedLines);
}

void AnnotationPainter::AdaptPlainTextAnnotationLayout(
    Annotation& annotation, TextLayoutContext& context) const {
    const int minimumWidth = context.minimumTextWidth + Scale(4);
    const int maximumStartX = std::max(0,
        context.rightContentEdge - minimumWidth);
    const int clampedStartX = std::clamp<int>(annotation.start.x,
        0, maximumStartX);
    const int lineHeight = std::max(context.fontSize + Scale(6),
        static_cast<int>(std::ceil(
            context.font.GetHeight(&context.graphics))));
    const int verticalPadding = Scale(8);
    const int minimumTextHeight = lineHeight + verticalPadding;
    const int maximumStartY = std::max(0,
        context.selectionHeight - minimumTextHeight);
    const int clampedStartY = context.selectionHeight >= minimumTextHeight
        ? std::clamp<int>(annotation.start.y, 0, maximumStartY)
        : 0;
    OffsetAnnotation(annotation,
        clampedStartX - annotation.start.x,
        clampedStartY - annotation.start.y);
    const int availableWidth = std::max(1,
        context.rightContentEdge - static_cast<int>(annotation.start.x));
    const int defaultTextEditorWidth = std::max(
        minimumWidth, Scale(kDefaultTextEditorWidthDip));
    const int contentWidth = std::max(
        defaultTextEditorWidth, context.naturalWidth + Scale(20));
    const int desiredWidth = std::min(availableWidth, contentWidth);
    const auto wrappedLines = WrapTextToPixelWidth(
        context.graphics, context.font, annotation.text,
        static_cast<float>(desiredWidth));
    const int requestedLineCount = std::max(1,
        static_cast<int>(wrappedLines.size()));
    const int availableHeight = std::max(0,
        context.selectionHeight - static_cast<int>(annotation.start.y));
    const int availableLineCount = std::max(0,
        (availableHeight - verticalPadding) / lineHeight);
    const int visibleLineCount = std::min(
        requestedLineCount, availableLineCount);
    const int desiredHeight = std::min(availableHeight,
        verticalPadding + visibleLineCount * lineHeight);
    annotation.end = POINT{
        annotation.start.x + desiredWidth,
        annotation.start.y + desiredHeight};
}

void AnnotationPainter::OffsetAnnotation(
    Annotation& annotation, int deltaX, int deltaY) const {
    annotation.start.x += deltaX;
    annotation.start.y += deltaY;
    annotation.end.x += deltaX;
    annotation.end.y += deltaY;
    for (POINT& point : annotation.points) {
        point.x += deltaX;
        point.y += deltaY;
    }
    annotation.mosaicGridOrigin.x+=deltaX;annotation.mosaicGridOrigin.y+=deltaY;
    if(annotation.clip)OffsetRect(&*annotation.clip,deltaX,deltaY);
}

}
