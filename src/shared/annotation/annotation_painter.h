#pragma once
#include "shared/image/image_document.h"
namespace capture {
// Stateless vector drawing plus bounded text layout; caller owns GDI+ lifetime.
// Mosaic is a separate entry point and requires an opaque source image.
class AnnotationPainter {
public:
    explicit AnnotationPainter(UINT dpi=96):dpi_(dpi) {}
    void PaintAnnotation(HDC dc,const Annotation& annotation,int x=0,int y=0,bool empty=false) const;
    void LayoutText(Annotation& a,int width,int height) {selection_={0,0,width,height}; AdaptTextAnnotationLayout(a);}
private:
    struct TextLayoutContext;
    void AdaptTextAnnotationLayout(Annotation&) const;
    void AdaptNumberTextAnnotationLayout(Annotation&,TextLayoutContext&) const;
    void AdaptPlainTextAnnotationLayout(Annotation&,TextLayoutContext&) const;
    void OffsetAnnotation(Annotation&,int,int) const;
    int Scale(int n) const {return MulDiv(n,int(dpi_),96);}
    UINT dpi_;
    RECT selection_{};
};
void PaintMosaic(HDC dc,const Annotation& annotation,UINT dpi,int x=0,int y=0);
}
