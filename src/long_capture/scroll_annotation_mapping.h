#pragma once
#include "shared/image/image_document.h"
#include <array>

namespace capture {
namespace scroll_annotation_detail {
inline RECT Intersect(RECT a, RECT b) {
    return {std::max(a.left,b.left),std::max(a.top,b.top),
        std::min(a.right,b.right),std::min(a.bottom,b.bottom)};
}
inline bool Empty(RECT r) {return r.left>=r.right || r.top>=r.bottom;}
inline void Offset(Annotation& a,int dy) {
    a.start.y+=dy;a.end.y+=dy;
    a.mosaicGridOrigin.y+=dy;
    for(auto& p:a.points)p.y+=dy;
    if(a.clip){a.clip->top+=dy;a.clip->bottom+=dy;}
}
// Conservative bounds include stroke caps, arrow wings and number callouts;
// clipping, not these bounds, decides which actual painted pixels survive.
inline RECT Bounds(const Annotation& a,UINT dpi) {
    RECT r{std::min(a.start.x,a.end.x),std::min(a.start.y,a.end.y),
        std::max(a.start.x,a.end.x),std::max(a.start.y,a.end.y)};
    const auto scale=[dpi](int n){return int((int64_t(n)*dpi+48)/96);};
    if((a.tool==Tool::Pen || a.tool==Tool::Mosaic)&&!a.points.empty()) {
        r={a.points.front().x,a.points.front().y,a.points.front().x,a.points.front().y};
        for(auto p:a.points){r.left=std::min(r.left,p.x);r.top=std::min(r.top,p.y);r.right=std::max(r.right,p.x);r.bottom=std::max(r.bottom,p.y);}
    }
    if(a.tool==Tool::Number) {
        const int diameter=std::max(scale(18),a.size+scale(8)),radius=(diameter+1)/2;
        r={a.start.x-radius,a.start.y-radius,a.start.x+radius,a.start.y+radius};
        if(a.textBoxSize.cx>0) {
            const int naturalHeight=int(std::min<int64_t>(0x1fffffff,
                int64_t(std::max(1,a.size)+scale(6))*std::max<size_t>(1,a.text.size())+scale(10)));
            const int height=std::max(diameter,a.textBoxSize.cy>0?int(a.textBoxSize.cy):naturalHeight);
            const int tip=a.start.x+(a.numberTextOnLeft?-1:1)*(diameter/2+scale(3));
            r.left=std::min<LONG>(r.left,a.numberTextOnLeft?tip-a.textBoxSize.cx:tip);
            r.right=std::max<LONG>(r.right,a.numberTextOnLeft?tip:tip+a.textBoxSize.cx);
            const int top=a.textBoxSize.cy>0?a.end.y:a.start.y-height/2;
            r.top=std::min<LONG>(r.top,top);r.bottom=std::max<LONG>(r.bottom,top+height);
        }
    }
    int padding=a.tool==Tool::Text?0:std::max(1,(a.size+1)/2)+1;
    if(a.tool==Tool::Arrow)padding+=std::max(scale(10),a.size*3);
    if(a.tool==Tool::Mosaic)padding+=std::max(scale(6),a.size)/2;
    r.left-=padding;r.top-=padding;r.right+=padding;r.bottom+=padding;
    if(a.clip)r=Intersect(r,*a.clip);
    return r;
}
}

// Call once with the original annotations. The five source rectangles are
// disjoint, so crossing strokes/text cannot be duplicated or stretched.
inline std::vector<Annotation> MapScrollAnnotations(const std::vector<Annotation>& original,
    RECT region,int width,int height,int prepend,int total,UINT dpi=96) {
    using namespace scroll_annotation_detail;
    if(total<=0 || width<=0 || height<=0 || Empty(region))return original;
    prepend=std::clamp(prepend,0,total);
    region=Intersect(region,{0,0,width,height});
    if(Empty(region))return original;
    struct Part{RECT source;int dy;};
    const std::array<Part,5> parts{{
        {{0,0,width,region.top},0},
        {{0,region.top,region.left,region.bottom},0},
        {region,prepend},
        {{region.right,region.top,width,region.bottom},0},
        {{0,region.bottom,width,height},total}}};
    std::vector<Annotation> mapped;
    mapped.reserve(original.size());
    uint64_t nextGroup=1;
    for(const auto& a:original)nextGroup=std::max(nextGroup,a.fragmentGroup+1);
    for(const auto& a:original) {
        const RECT bounds=Bounds(a,dpi);
        const size_t begin=mapped.size();
        for(const auto& part:parts) {
            if(Empty(Intersect(bounds,part.source)))continue;
            Annotation fragment=a;
            const bool contained=bounds.left>=part.source.left&&bounds.top>=part.source.top&&bounds.right<=part.source.right&&bounds.bottom<=part.source.bottom;
            if(!contained)fragment.clip=a.clip?Intersect(*a.clip,part.source):part.source;
            Offset(fragment,part.dy);
            mapped.push_back(std::move(fragment));
        }
        if(mapped.size()-begin>1) {
            const uint64_t group=a.fragmentGroup?a.fragmentGroup:nextGroup++;
            for(size_t i=begin;i<mapped.size();++i)mapped[i].fragmentGroup=group;
        }
    }
    return mapped;
}
}
