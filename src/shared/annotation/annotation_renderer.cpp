#include "shared/annotation/annotation_renderer.h"
#include "shared/annotation/annotation_painter.h"
#include "shared/platform/capture_platform.h"
#include <objidl.h>
#include <gdiplus.h>
namespace capture {
AnnotationRenderer::AnnotationRenderer(UINT dpi):dpi_(dpi) { Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&gdiplusToken_,&input,nullptr); }
AnnotationRenderer::~AnnotationRenderer() { if(gdiplusToken_) Gdiplus::GdiplusShutdown(gdiplusToken_); }
void AnnotationRenderer::SetDpi(UINT dpi) { dpi_=dpi; }
void AnnotationRenderer::LayoutText(Annotation& annotation,int width,int height) {
    AnnotationPainter(dpi_).LayoutText(annotation,width,height);
}
void AnnotationRenderer::Draw(HDC dc,const std::vector<Annotation>& annotations,int x,int y) const {
    for(const auto& a:annotations) AnnotationPainter(dpi_).PaintAnnotation(dc,a,x,y);
}
void AnnotationRenderer::Composite(Image& image,const std::vector<Annotation>& annotations) const {
    if(annotations.empty()) return;
    HBITMAP bitmap=ToBitmap(image); HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
    Draw(dc,annotations); SelectObject(dc,old); DeleteDC(dc);
    try { image=FromBitmap(bitmap); } catch(...) { DeleteObject(bitmap); throw; } DeleteObject(bitmap);
}
int AnnotationRenderer::HitSelection(RECT r,POINT p,RECT monitor) {
    (void)monitor;
    if(r.right-r.left<MulDiv(4,dpi_,96) || r.bottom-r.top<MulDiv(4,dpi_,96)) return 0;
    const int hit=MulDiv(7,dpi_,96);
    const bool l=std::abs(p.x-r.left)<=hit, right=std::abs(p.x-r.right)<=hit;
    const bool t=std::abs(p.y-r.top)<=hit,b=std::abs(p.y-r.bottom)<=hit;
    if(l&&t) return 5; if(right&&t) return 6; if(l&&b) return 7; if(right&&b) return 8;
    if(p.y>=r.top-hit && p.y<=r.bottom+hit) {if(l)return 1;if(right)return 3;}
    if(p.x>=r.left-hit && p.x<=r.right+hit) {if(t)return 2;if(b)return 4;}
    return 0;
}
RECT AnnotationRenderer::AdjustSelection(RECT r,POINT down,POINT p,int handle,RECT monitor) {
    const LONG minimum=MulDiv(4,dpi_,96);
    if(!handle) OffsetRect(&r,std::clamp(p.x-down.x,monitor.left-r.left,monitor.right-r.right),std::clamp(p.y-down.y,monitor.top-r.top,monitor.bottom-r.bottom));
    if(handle==1||handle==5||handle==7) r.left=std::clamp(p.x,monitor.left,std::max(monitor.left,r.right-minimum));
    if(handle==3||handle==6||handle==8) r.right=std::clamp(p.x,std::min(monitor.right,r.left+minimum),monitor.right);
    if(handle==2||handle==5||handle==6) r.top=std::clamp(p.y,monitor.top,std::max(monitor.top,r.bottom-minimum));
    if(handle==4||handle==7||handle==8) r.bottom=std::clamp(p.y,std::min(monitor.bottom,r.top+minimum),monitor.bottom);
    return r;
}
void DrawLaser(HDC dc,const LaserPointer& pointer,int x,int y) {
    if(!pointer.visible) return;
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const auto color=pointer.color;
    Gdiplus::SolidBrush halo(Gdiplus::Color(65,GetRValue(color),GetGValue(color),GetBValue(color))),dot(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)));
    const float radius=float(std::max(1,pointer.radius));
    const float cx=float(pointer.point.x+x),cy=float(pointer.point.y+y);
    g.FillEllipse(&halo,cx-2*radius,cy-2*radius,4*radius,4*radius);
    g.FillEllipse(&dot,cx-radius,cy-radius,2*radius,2*radius);
}
static void DrawRecording(Image& image,const RecordingAnnotations& snapshot,AnnotationRenderer& renderer,int x,int y,const LaserPointer* liveLaser=nullptr) {
    const auto& laser=liveLaser?*liveLaser:snapshot.laser;
    if(snapshot.shapes.empty() && !laser.visible) return;
    renderer.SetDpi(snapshot.dpi);
    HBITMAP bitmap=ToBitmap(image); if(!bitmap) throw std::bad_alloc();
    HDC dc=CreateCompatibleDC(nullptr); auto old=SelectObject(dc,bitmap);
    renderer.Draw(dc,snapshot.shapes,x,y); DrawLaser(dc,laser,x,y);
    SelectObject(dc,old); DeleteDC(dc);
    try { image=FromBitmap(bitmap); } catch(...) { DeleteObject(bitmap); throw; } DeleteObject(bitmap);
}
AnnotationSurface RenderRecordingSurface(int width,int height,const RecordingAnnotations& snapshot,AnnotationRenderer& renderer,int offsetX,int offsetY) {
    // Recover premultiplied coverage from the shared renderer's RGB output.
    // The desktop compositor must blend edge pixels, never color-key them.
    Image black(width,height),white(width,height);
    std::fill(black.pixels.begin(),black.pixels.end(),0xff000000);
    std::fill(white.pixels.begin(),white.pixels.end(),0xffffffff);
    DrawRecording(black,snapshot,renderer,offsetX,offsetY); DrawRecording(white,snapshot,renderer,offsetX,offsetY);
    AnnotationSurface result{width,height,std::move(black.pixels)};
    for(size_t i=0;i<result.pixels.size();++i) {
        const auto b=result.pixels[i],w=white.pixels[i]; int difference=0;
        for(int shift:{0,8,16}) difference=std::max(difference,int((w>>shift)&255)-int((b>>shift)&255));
        const unsigned alpha=255-unsigned(difference); uint32_t pixel=alpha<<24;
        for(int shift:{0,8,16}) pixel|=std::min(alpha,(b>>shift)&255)<<shift;
        result.pixels[i]=pixel;
    }
    return result;
}
void CompositeRecording(Image& image,const RecordingAnnotations& snapshot,AnnotationRenderer& renderer,int64_t /*now*/,const LaserPointer* liveLaser) {
    DrawRecording(image,snapshot,renderer,0,0,liveLaser);
}
}



