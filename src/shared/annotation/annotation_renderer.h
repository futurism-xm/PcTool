#pragma once
#include "shared/image/image_document.h"
namespace capture {
// Each UI/encoder thread owns its renderer. No HWND or mutable document is shared.
class AnnotationRenderer {
public:
    explicit AnnotationRenderer(UINT dpi=96);
    ~AnnotationRenderer();
    AnnotationRenderer(const AnnotationRenderer&)=delete;
    AnnotationRenderer& operator=(const AnnotationRenderer&)=delete;
    void SetDpi(UINT dpi);
    void Draw(HDC dc,const std::vector<Annotation>& annotations,int x=0,int y=0) const;
    void Composite(Image& image,const std::vector<Annotation>& annotations) const;
    void LayoutText(Annotation& annotation,int width,int height);
    int HitSelection(RECT region,POINT point,RECT monitor);
    RECT AdjustSelection(RECT original,POINT down,POINT point,int handle,RECT monitor);
private:
    UINT dpi_{96};
    ULONG_PTR gdiplusToken_{};
};
struct LaserPointer { POINT point{}; bool visible{}; COLORREF color{RGB(255,45,45)}; int radius{4}; };
struct RecordingAnnotations {
    uint64_t version{};
    UINT dpi{96};
    std::vector<Annotation> shapes;
    LaserPointer laser;
};
void DrawLaser(HDC dc,const LaserPointer& pointer,int x=0,int y=0);
// Unlike Image's opaque pixels, this surface contains premultiplied BGRA for the desktop compositor.
struct AnnotationSurface { int width{},height{}; std::vector<uint32_t> pixels; };
AnnotationSurface RenderRecordingSurface(int width,int height,const RecordingAnnotations& snapshot,AnnotationRenderer& renderer,int offsetX=0,int offsetY=0);
void CompositeRecording(Image& image,const RecordingAnnotations& snapshot,AnnotationRenderer& renderer,int64_t now,const LaserPointer* liveLaser=nullptr);
}

