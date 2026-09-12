#include "long_capture/scroll_annotation_mapping.h"
#include "shared/annotation/annotation_fragments.h"
#include "shared/annotation/annotation_painter.h"
#include <objidl.h>
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>

using namespace capture;
static void Require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
static std::vector<uint32_t> Paint(int width,int height,UINT dpi,const std::vector<Annotation>& annotations,const std::vector<uint32_t>* background=nullptr) {
    BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};
    void* pixels{};auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    Require(bitmap!=nullptr,"DIB allocation");HDC dc=CreateCompatibleDC(nullptr);auto old=SelectObject(dc,bitmap);
    if(background)std::copy(background->begin(),background->end(),static_cast<uint32_t*>(pixels));
    else std::fill_n(static_cast<uint32_t*>(pixels),size_t(width)*height,0xffffffff);
    for(const auto& a:annotations)if(a.tool==Tool::Mosaic)PaintMosaic(dc,a,dpi);else AnnotationPainter(dpi).PaintAnnotation(dc,a);
    GdiFlush();std::vector<uint32_t> result(static_cast<uint32_t*>(pixels),static_cast<uint32_t*>(pixels)+size_t(width)*height);
    for(auto& p:result)p|=0xff000000;
    SelectObject(dc,old);DeleteDC(dc);DeleteObject(bitmap);return result;
}
static void VerifyPixels(const Annotation& a,UINT dpi,int prepend,int total) {
    constexpr int width=320,height=260;const RECT roi{90,75,235,190};
    const auto source=Paint(width,height,dpi,{a});
    const auto mapped=MapScrollAnnotations({a},roi,width,height,prepend,total,dpi);
    const auto actual=Paint(width,height+total,dpi,mapped);
    std::vector<uint32_t> expected(size_t(width)*(height+total),0xffffffff);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const int dy=y>=roi.bottom?total:(y>=roi.top&&x>=roi.left&&x<roi.right?prepend:0);
        expected[size_t(y+dy)*width+x]=source[size_t(y)*width+x];
    }
    for(size_t i=0;i<expected.size();++i)if(expected[i]!=actual[i]) {
        std::cerr<<"tool="<<int(a.tool)<<" dpi="<<dpi<<" prepend="<<prepend<<" mismatch at "<<i%width<<","<<i/width<<" expected="<<std::hex<<expected[i]<<" actual="<<actual[i]<<std::dec<<"\n";
        throw std::runtime_error("partitioned vector raster differs from pixel mapping");
    }
    if(mapped.size()>1){Require(mapped.front().fragmentGroup!=0,"crossing annotation grouped");for(const auto& part:mapped)Require(part.fragmentGroup==mapped.front().fragmentGroup,"all fragments remain one group");}
}
static void VerifyMosaic(UINT dpi) {
    constexpr int width=320,height=260,prepend=43,total=91;
    const RECT roi{90,75,235,190};
    std::vector<uint32_t> source(size_t(width)*height);uint32_t seed=8123;
    for(auto& p:source){seed=seed*1664525+1013904223;p=0xff000000|(seed&0xffffff);}
    std::vector<uint32_t> composed(size_t(width)*(height+total),0xffaa00aa);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){const int dy=y>=roi.bottom?total:(y>=roi.top&&x>=roi.left&&x<roi.right?prepend:0);composed[size_t(y+dy)*width+x]=source[size_t(y)*width+x];}
    Annotation a;a.tool=Tool::Mosaic;a.size=44;a.blurLevel=2;a.points={{232,188}};a.start=a.end=a.points.front();a.mosaicGridOrigin={3,5};
    const auto fragments=MapScrollAnnotations({a},roi,width,height,prepend,total,dpi);
    const auto actual=Paint(width,height+total,dpi,fragments,&composed);auto expected=composed;
    const auto scale=[dpi](int n){return MulDiv(n,dpi,96);};
    const int diameter=std::max(scale(6),a.size),radius=diameter/2;
    const int block=std::min(std::max(scale(2),scale(2+2*a.blurLevel)),std::max(scale(2),diameter/2));
    const auto intersect=[](RECT r,RECT s){return RECT{std::max(r.left,s.left),std::max(r.top,s.top),std::min(r.right,s.right),std::min(r.bottom,s.bottom)};};
    struct Partition{RECT source;int dy;};
    const Partition partitions[]={{{0,0,width,roi.top},0},{{0,roi.top,roi.left,roi.bottom},0},{roi,prepend},{{roi.right,roi.top,width,roi.bottom},0},{{0,roi.bottom,width,height},total}};
    // Independent reference: enumerate source grid cells and map only each
    // partition's visible pixels. A boundary cell samples inside that part.
    for(const auto& part:partitions)for(int gy=-2;gy<(height+2*block)/block;++gy)for(int gx=-2;gx<(width+2*block)/block;++gx) {
        const int left=a.mosaicGridOrigin.x+gx*block,top=a.mosaicGridOrigin.y+gy*block;
        const int centerX=left+block/2,centerY=top+block/2;
        const int dx=centerX-a.points[0].x,dy=centerY-a.points[0].y;
        if(dx*dx+dy*dy>radius*radius)continue;
        const RECT visible=intersect(part.source,{left,top,left+block,top+block});
        if(visible.left>=visible.right||visible.top>=visible.bottom)continue;
        const int sampleX=std::clamp(centerX,int(visible.left),int(visible.right)-1),sampleY=std::clamp(centerY,int(visible.top),int(visible.bottom)-1);
        const auto color=source[size_t(sampleY)*width+sampleX];
        for(int y=visible.top;y<visible.bottom;++y)for(int x=visible.left;x<visible.right;++x)expected[size_t(y+part.dy)*width+x]=color;
    }
    Require(actual==expected,"mosaic preserves source grid and partition-local samples after bidirectional expansion");
    Require(fragments.size()==3,"mosaic crosses body/right/footer exactly once");
    for(const auto& part:fragments){const int dy=part.start.y-a.start.y;Require(part.mosaicGridOrigin.y==a.mosaicGridOrigin.y+dy,"mosaic grid translates with its fragment");}
}
int main() {
    Gdiplus::GdiplusStartupInput startup;ULONG_PTR token{};Gdiplus::GdiplusStartup(&token,&startup,nullptr);
    int result=0;
    try {
        for(UINT dpi:{96u,144u,192u})VerifyMosaic(dpi);
        for(UINT dpi:{96u,144u,192u})for(int prepend:{0,45,90})for(Tool tool:{Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Text,Tool::Number}) {
            Annotation a;a.tool=tool;a.start={40,35};a.end={280,220};a.size=tool==Tool::Text||tool==Tool::Number?14:5;
            a.color=RGB(212,54,77);a.points={{25,30},{280,240},{50,175},{260,80}};
            a.text=L"顶部 FIXED HEADER\n中部 scrolling text with wrapped words\n左侧 SIDEBAR 123456789\n正文 Content\n底部 FIXED FOOTER";
            if(tool==Tool::Number){a.start={80,85};a.end={80,50};a.textBoxSize={170,90};a.number=8;}
            VerifyPixels(a,dpi,prepend,90);
            a.clip=RECT{50,55,255,220};VerifyPixels(a,dpi,prepend,90);
            if(tool==Tool::Number){a.clip.reset();a.textBoxSize.cy=0;VerifyPixels(a,dpi,prepend,90);a.numberTextOnLeft=true;VerifyPixels(a,dpi,prepend,90);}
        }
        Annotation body;body.tool=Tool::Text;body.start={110,100};body.end={200,140};body.text=L"BODY";body.size=14;
        auto mapped=MapScrollAnnotations({body},{90,75,235,190},320,260,45,90);
        Require(mapped.size()==1&&!mapped[0].clip&&mapped[0].start.y==145,"contained body remains editable once");
        Annotation crossing=body;crossing.start={40,40};crossing.end={280,220};
        mapped=MapScrollAnnotations({crossing},{90,75,235,190},320,260,45,90);
        const auto before=mapped;Annotation edited=mapped[2];edited.text=L"edited group";OffsetAnnotationFragment(edited,3,5);
        ReplaceAnnotationGroup(mapped,2,edited);
        for(size_t i=0;i<mapped.size();++i){Require(mapped[i].text==L"edited group","text edit changes logical group");Require(mapped[i].start.x==before[i].start.x+3&&mapped[i].start.y==before[i].start.y+5,"group fragment translations preserved");Require(mapped[i].clip->left==before[i].clip->left+3&&mapped[i].clip->top==before[i].clip->top+5,"group clips move with contents");}
        mapped.push_back(body);EraseAnnotationGroup(mapped,0);Require(mapped.size()==1&&mapped[0].text==body.text,"delete removes exactly selected group");
        const auto unchanged=MapScrollAnnotations({crossing},{90,75,235,190},320,260,0,0);
        Require(unchanged.size()==1&&!unchanged[0].clip,"unexpanded document unchanged");
        std::cout<<"SCROLL ANNOTATIONS PASS: pixel-exact partition mapping at 96/144/192 DPI; up/down expansion; nested clips; grouped text/move/delete; mosaic grid/partition sampling\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<"\n";result=1;}
    Gdiplus::GdiplusShutdown(token);return result;
}
