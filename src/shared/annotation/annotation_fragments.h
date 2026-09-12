#pragma once
#include "shared/image/image_document.h"

namespace capture {
inline void OffsetAnnotationFragment(Annotation& a,int dx,int dy) {
    a.start.x+=dx;a.start.y+=dy;a.end.x+=dx;a.end.y+=dy;
    for(auto& p:a.points){p.x+=dx;p.y+=dy;}
    a.mosaicGridOrigin.x+=dx;a.mosaicGridOrigin.y+=dy;
    if(a.clip){a.clip->left+=dx;a.clip->right+=dx;a.clip->top+=dy;a.clip->bottom+=dy;}
}
inline void ReplaceAnnotationGroup(std::vector<Annotation>& annotations,size_t index,const Annotation& replacement) {
    if(index>=annotations.size())return;
    const Annotation original=annotations[index];
    if(!original.fragmentGroup){annotations[index]=replacement;return;}
    for(auto& a:annotations)if(a.fragmentGroup==original.fragmentGroup) {
        const int dx=a.start.x-original.start.x,dy=a.start.y-original.start.y;
        auto clip=a.clip;
        if(clip){clip->left+=replacement.start.x-original.start.x;clip->right+=replacement.start.x-original.start.x;clip->top+=replacement.start.y-original.start.y;clip->bottom+=replacement.start.y-original.start.y;}
        a=replacement;OffsetAnnotationFragment(a,dx,dy);a.clip=clip;a.fragmentGroup=original.fragmentGroup;
    }
}
inline void EraseAnnotationGroup(std::vector<Annotation>& annotations,size_t index) {
    if(index>=annotations.size())return;
    const auto group=annotations[index].fragmentGroup;
    if(!group){annotations.erase(annotations.begin()+index);return;}
    annotations.erase(std::remove_if(annotations.begin(),annotations.end(),[group](const auto& a){return a.fragmentGroup==group;}),annotations.end());
}
}
