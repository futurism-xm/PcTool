#include "long_capture/scroll_stitcher.h"
#include "long_capture/scroll_annotation_mapping.h"
namespace capture {
static int Gray(uint32_t p) { return int(((p>>16)&255)*77+((p>>8)&255)*150+(p&255)*29)>>8; }
double FrameDifference(const Image& a,const Image& b) {
    if (a.Empty() || a.width!=b.width || a.height!=b.height) return 255;
    double error[3]{}; int count[3]{};
    for(int y=0;y<a.height;y+=3) for(int x=0;x<a.width;x+=std::max(1,a.width/160)) {
        const int band=std::min(2,x*3/a.width);
        error[band]+=std::abs(Gray(a.pixels[size_t(y)*a.width+x])-Gray(b.pixels[size_t(y)*a.width+x])); ++count[band];
    }
    for(int i=0;i<3;++i) error[i]/=std::max(1,count[i]);
    std::sort(std::begin(error),std::end(error));
    return error[1];
}

namespace {
struct BandMotion {int left{},right{},top{},bottom{},shift{},weight{};double error{};};
struct Match {RECT region{};int shift{};double confidence{};bool valid{},ambiguous{};};
int Delta(uint32_t a,uint32_t b){return std::abs(Gray(a)-Gray(b));}
// Several narrow bands vote independently, so a small table is not outvoted by
// fixed navigation, or by an unrelated animated panel.
std::vector<BandMotion> MatchVerticalBands(const Image& a,const Image& b,RECT bounds,int& ambiguousBands) {
    std::vector<BandMotion> bands;
    const int bandWidth=std::max<int>(8,(bounds.right-bounds.left)/64);
    for(int left=bounds.left;left<bounds.right;left+=bandWidth){
        const int right=std::min(int(bounds.right),left+bandWidth);
        // Disjoint changes in a footer/counter must not enlarge a small body
        // into one enormous motion band. Keep nearby text rows together, but
        // separate runs across a substantial unchanged vertical gap.
        struct ChangedRun{int top,bottom,weight,left,right;};
        std::vector<ChangedRun> runs;
        for(int y=bounds.top;y<bounds.bottom;++y)for(int x=left;x<right;x+=std::max(1,bandWidth/4)){
            if(Delta(a.pixels[size_t(y)*a.width+x],b.pixels[size_t(y)*a.width+x])>10){
                if(runs.empty()||y-runs.back().bottom>48)runs.push_back({y,y+1,0,right,left});
                auto& run=runs.back();run.bottom=y+1;run.left=std::min(run.left,x);run.right=std::max(run.right,x+1);++run.weight;}
        }
        for(const auto& run:runs){const int top=run.top,bottom=run.bottom,weight=run.weight,changedLeft=run.left,changedRight=run.right;
        if(weight<16||bottom-top<24)continue;
        int best=0;double bestError=256;std::vector<std::pair<int,double>> scores;
        const int minimum=std::max(16,(bottom-top)/5),limit=bottom-top-minimum;
        for(int d=-limit;d<=limit;++d){
            if(!d)continue;double error=0;int samples=0;double texture=0;
            const int begin=top+std::max(0,-d),end=bottom-std::max(0,d);
            for(int y=begin;y<end;y+=std::max(1,(end-begin)/64))for(int x=left;x<right;x+=std::max(1,bandWidth/3)){
                const auto p=b.pixels[size_t(y)*b.width+x],q=a.pixels[size_t(y+d)*a.width+x];
                const int energy=std::abs(Gray(p)-Gray(b.pixels[size_t(std::min(y+1,int(bounds.bottom)-1))*b.width+x]));
                // White padding contributes neither artificial matches nor confidence.
                if(energy<3&&Delta(a.pixels[size_t(y)*a.width+x],p)<3)continue;
                error+=Delta(p,q);texture+=energy;++samples;
            }
            const double score=samples>=12&&texture/samples>1?error/samples:256;
            scores.emplace_back(d,score);if(score<bestError){bestError=score;best=d;}
        }
        if(!best||bestError>5)continue;
        bool ambiguous=false;for(auto [d,error]:scores)if(std::abs(d-best)>3&&error<bestError+0.7){ambiguous=true;break;}
        if(!ambiguous)bands.push_back({changedLeft,changedRight,top,bottom,best,weight,bestError});
        else ++ambiguousBands;
        }
    }
    return bands;
}
Match Detect(const Image& a,const Image& b,RECT bounds,std::optional<POINT> anchor) {
    int ambiguousBands=0;const auto bands=MatchVerticalBands(a,b,bounds,ambiguousBands);
    Match match;match.ambiguous=ambiguousBands>=2;if(bands.empty())return match;
    int chosen=0;double bestVote=-1;
    for(size_t i=0;i<bands.size();++i){
        double vote=0;
        for(const auto& v:bands)if(v.shift==bands[i].shift){
            double proximity=1;
            if(anchor){const int dx=std::max({v.left-int(anchor->x),int(anchor->x)-v.right,0});const int dy=std::max({v.top-int(anchor->y),int(anchor->y)-v.bottom,0});proximity=1.0/(1.0+double(dx+dy)/100);}
            vote+=std::min(v.weight,2000)*proximity;
        }
        if(vote>bestVote){bestVote=vote;chosen=int(i);}
    }
    if(anchor){const int shift=bands[chosen].shift;int distance=INT_MAX;for(size_t i=0;i<bands.size();++i)if(bands[i].shift==shift){const auto& v=bands[i];const int d=std::max({v.left-int(anchor->x),int(anchor->x)-v.right,0})+std::max({v.top-int(anchor->y),int(anchor->y)-v.bottom,0});if(d<distance){distance=d;chosen=int(i);}}}
    const auto& seed=bands[chosen];RECT r{seed.left,seed.top,seed.right,seed.bottom};int count=0;double error=0;
    // Separate adjacent panels at a stationary border/background gutter. Blank
    // padding matching the body's own background does not split text columns.
    auto separator=[&](const BandMotion& left,const BandMotion& right){
        if(right.left-left.right<8)return false;
        const int top=std::max(left.top,right.top),bottom=std::min(left.bottom,right.bottom);
        for(int x=left.right;x<right.left;++x){const auto color=b.pixels[size_t((top+bottom)/2)*b.width+x];int same=0,n=0,bodySame=0;
            for(int y=top;y<bottom;y+=3){++n;if(Delta(color,b.pixels[size_t(y)*b.width+x])<5&&Delta(a.pixels[size_t(y)*a.width+x],b.pixels[size_t(y)*b.width+x])<3)++same;
                if(Delta(color,b.pixels[size_t(y)*b.width+(seed.left+seed.right)/2])<8)++bodySame;}
            if(n&&same>n*98/100&&bodySame<n/3)return true;
        }return false;};
    std::vector<BandMotion> candidates;
    for(const auto& v:bands)if(v.shift==seed.shift&&std::min(v.bottom,seed.bottom)-std::max(v.top,seed.top)>=std::min(v.bottom-v.top,seed.bottom-seed.top)/2)candidates.push_back(v);
    size_t first=0;while(first<candidates.size()&&candidates[first].left!=seed.left)++first;size_t last=first;
    while(first>0&&!separator(candidates[first-1],candidates[first]))--first;
    while(last+1<candidates.size()&&!separator(candidates[last],candidates[last+1]))++last;
    for(size_t i=first;i<=last;++i){const auto& v=candidates[i];
        r.left=std::min<LONG>(r.left,v.left);r.right=std::max<LONG>(r.right,v.right);r.top=std::min<LONG>(r.top,v.top);r.bottom=std::max<LONG>(r.bottom,v.bottom);++count;error+=v.error;
    }
    if(count<2||r.right-r.left<12)return match;
    // Refine the changed edges at pixel resolution, including the remainder of
    // sampled bands. This retains exact edges on textured source images.
    auto columnChanged=[&](int x){int n=0;for(int y=r.top;y<r.bottom;y+=2)if(Delta(a.pixels[size_t(y)*a.width+x],b.pixels[size_t(y)*b.width+x])>10)++n;return n>=3;};
    while(r.left>bounds.left&&columnChanged(r.left-1))--r.left;
    while(r.right<bounds.right&&columnChanged(r.right))++r.right;
    // Extend through blank padding, stopping at fixed text/borders rather than
    // assuming headers/footers are less than one quarter of the selection.
    int histogram[4096]{};int totalBackground=0,bestBackground=0;
    for(int y=r.top;y<r.bottom;y+=3)for(int x=r.left;x<r.right;x+=3){auto pixel=b.pixels[size_t(y)*b.width+x];int bin=((pixel>>12)&0xf00)|((pixel>>8)&0xf0)|((pixel>>4)&0xf);++totalBackground;if(++histogram[bin]>histogram[bestBackground])bestBackground=bin;}
    const bool hasBackground=histogram[bestBackground]>totalBackground/3;
    uint64_t redSum=0,greenSum=0,blueSum=0;int backgroundCount=0;
    for(int y=r.top;y<r.bottom;y+=3)for(int x=r.left;x<r.right;x+=3){const auto p=b.pixels[size_t(y)*b.width+x];const int bin=((p>>12)&0xf00)|((p>>8)&0xf0)|((p>>4)&0xf);if(bin==bestBackground){redSum+=(p>>16)&255;greenSum+=(p>>8)&255;blueSum+=p&255;++backgroundCount;}}
    const uint32_t background=0xff000000|(uint32_t(redSum/std::max(1,backgroundCount))<<16)|(uint32_t(greenSum/std::max(1,backgroundCount))<<8)|uint32_t(blueSum/std::max(1,backgroundCount));
    // Rasterized text can disturb pixels just outside a viewport (antialiasing
    // or compressed frame sources). Those fringes are not scrolling content.
    // Require background coverage or actual displaced detail at the edges.
    auto bodyRow=[&](int y){int plain=0,moving=0,total=0;double error=0;
        for(int x=r.left;x<r.right;x+=2){auto p=b.pixels[size_t(y)*b.width+x];++total;if(Delta(p,background)<9)++plain;if(Delta(p,a.pixels[size_t(y)*a.width+x])>10)++moving;
            if(y+seed.shift>=bounds.top&&y+seed.shift<bounds.bottom)error+=Delta(p,a.pixels[size_t(y+seed.shift)*a.width+x]);else error+=255;}
        return plain>total/2||moving>total/2||(moving>total/20&&error/std::max(1,total)<3);};
    if(hasBackground){
        const LONG topLimit=std::min(r.top+8,r.bottom-24),bottomLimit=std::max(r.bottom-8,r.top+24);
        while(r.top<topLimit&&!bodyRow(r.top))++r.top;
        while(r.bottom>bottomLimit&&!bodyRow(r.bottom-1))--r.bottom;
    }
    auto blankRow=[&](int y){if(!hasBackground)return false;const int x=(r.left+r.right)/2;const auto base=b.pixels[size_t(y)*b.width+x];if(Delta(base,background)>6)return false;int different=0,total=0;for(int xx=r.left;xx<r.right;xx+=2){++total;if(Delta(base,b.pixels[size_t(y)*b.width+xx])>6||Delta(a.pixels[size_t(y)*a.width+xx],b.pixels[size_t(y)*b.width+xx])>6)++different;}return different<=total/100;};
    while(r.top>bounds.top&&blankRow(r.top-1))--r.top;
    while(r.bottom<bounds.bottom&&blankRow(r.bottom))++r.bottom;
    auto blankColumn=[&](int x){if(!hasBackground)return false;const auto base=b.pixels[size_t((r.top+r.bottom)/2)*b.width+x];if(Delta(base,background)>6)return false;int different=0,total=0;for(int y=r.top;y<r.bottom;y+=2){++total;if(Delta(base,b.pixels[size_t(y)*b.width+x])>6||Delta(a.pixels[size_t(y)*a.width+x],b.pixels[size_t(y)*b.width+x])>6)++different;}return different<=total/100;};
    while(r.left>bounds.left&&blankColumn(r.left-1))--r.left;
    while(r.right<bounds.right&&blankColumn(r.right))++r.right;
    // Unique row numbers establish displacement, but repeated text to their
    // right cannot cast an unambiguous band vote on its own. Include that text
    // when its pixels agree with the established displacement across the same
    // background. Otherwise a sentence is split into moving and fixed columns.
    auto coherentColumn=[&](int x){
        if(!hasBackground)return false;int plain=0,total=0,evidence=0,bad=0;double error=0;
        for(int y=r.top+std::max(0,-seed.shift);y<r.bottom-std::max(0,seed.shift);++y){
            const auto p=b.pixels[size_t(y)*b.width+x],q=a.pixels[size_t(y+seed.shift)*a.width+x];
            ++total;if(Delta(p,background)<9)++plain;
            if(Delta(p,background)>10||Delta(q,background)>10){const int d=Delta(p,q);error+=d;++evidence;if(d>10)++bad;}
        }
        // A new longer/shorter line can introduce ink only in the newly exposed
        // strip. A wholly blank, matching overlap still connects that column.
        return total>=16&&plain>total/5&&(!evidence||(error/evidence<3&&bad<=evidence/50));
    };
    while(r.left>bounds.left&&(blankColumn(r.left-1)||coherentColumn(r.left-1)))--r.left;
    while(r.right<bounds.right&&(blankColumn(r.right)||coherentColumn(r.right)))++r.right;
    // The first changed glyph may start below the real viewport edge. Recover
    // exposed text rows by checking the opposite frame at the known shift;
    // requiring a blank row here would freeze the top of the first text line.
    auto coherentRow=[&](int y){
        if(!hasBackground)return false;int other=0;bool forward=false;
        if(y+seed.shift>=r.top&&y+seed.shift<r.bottom){other=y+seed.shift;forward=true;}
        else if(y-seed.shift>=r.top&&y-seed.shift<r.bottom)other=y-seed.shift;
        else return false;
        int samples=0,bad=0;double error=0;
        for(int x=r.left;x<r.right;++x){const int d=forward?Delta(b.pixels[size_t(y)*b.width+x],a.pixels[size_t(other)*a.width+x]):Delta(a.pixels[size_t(y)*a.width+x],b.pixels[size_t(other)*b.width+x]);error+=d;++samples;if(d>10)++bad;}
        return samples>=16&&error/samples<3&&bad<=samples/50;
    };
    while(r.top>bounds.top&&coherentRow(r.top-1))--r.top;
    while(r.bottom<bounds.bottom&&coherentRow(r.bottom))++r.bottom;
    // A band straddling the body and a narrow scrollbar may vote for the body
    // while its changed-pixel extent includes the thumb. Validate its edge
    // columns against the same displacement, including detail that exists in
    // only one overlapped frame (e.g. a thumb confined to the prepended strip).
    // This prevents repeated scrollbars without narrowing ordinary white text
    // padding. Static rails whose colour differs from the body are trimmed too.
    if(hasBackground){
        auto columnEvidence=[&](int x){double error=0;int evidence=0;
            for(int y=r.top+std::max(0,-seed.shift);y<r.bottom-std::max(0,seed.shift);++y){
                const auto p=b.pixels[size_t(y)*b.width+x],q=a.pixels[size_t(y+seed.shift)*a.width+x];
                if(Delta(p,background)>10||Delta(q,background)>10){error+=Delta(p,q);++evidence;}}
            return std::pair<int,double>{evidence,error/std::max(1,evidence)};};
        auto rail=[&](int x){int plain=0,n=0;for(int y=r.top;y<r.bottom;++y){++n;if(Delta(b.pixels[size_t(y)*b.width+x],background)<=2)++plain;}return plain<n*9/10;};
        const int edgeWidth=std::min(32,int(r.right-r.left)/6);
        int rightBad=r.right,rightCount=0;
        // Coherent expansion can stop immediately before the thumb. Include
        // its first two columns when deciding whether to trim the adjacent rail.
        for(int x=r.right-edgeWidth;x<std::min<int>(bounds.right,r.right+2);++x){const auto e=columnEvidence(x);if(e.first>=4&&e.second>12){rightBad=std::min(rightBad,x);++rightCount;}}
        if(rightCount>=2){r.right=rightBad;while(r.right>r.left+12&&r.right>bounds.left){const auto e=columnEvidence(r.right-1);if((e.first>=4&&e.second<5)||!rail(r.right-1))break;--r.right;}}
        int leftBad=r.left-1,leftCount=0;
        for(int x=r.left;x<std::min<int>(r.right,r.left+edgeWidth);++x){const auto e=columnEvidence(x);if(e.first>=4&&e.second>12){leftBad=std::max(leftBad,x);++leftCount;}}
        if(leftCount>=2){r.left=leftBad+1;while(r.left<r.right-12){const auto e=columnEvidence(r.left);if((e.first>=4&&e.second<5)||!rail(r.left))break;++r.left;}}
    }
    match={r,seed.shift,std::clamp(1-error/count/6,0.0,1.0),true};return match;
}
uint32_t SideColor(const Image& image,RECT side){
    int counts[4096]{};uint64_t red[4096]{},green[4096]{},blue[4096]{};int best=0;
    for(int y=side.top;y<side.bottom;y+=3)for(int x=side.left;x<side.right;x+=3){const auto p=image.pixels[size_t(y)*image.width+x];const int bin=((p>>12)&0xf00)|((p>>8)&0xf0)|((p>>4)&0xf);red[bin]+=(p>>16)&255;green[bin]+=(p>>8)&255;blue[bin]+=p&255;if(++counts[bin]>counts[best])best=bin;}
    const auto n=std::max(1,counts[best]);return 0xff000000|(uint32_t(red[best]/n)<<16)|(uint32_t(green[best]/n)<<8)|uint32_t(blue[best]/n);
}
// Preserve continuous vertical boundaries. Non-uniform columns may contain
// large icons as well as letters; extend the surrounding background instead.
uint32_t Background(const Image& image,int x,RECT region,uint32_t fallback){
    std::vector<int> red,green,blue;
    for(int y=region.top;y<region.bottom;y+=std::max(1,int(region.bottom-region.top)/101)){auto p=image.pixels[size_t(y)*image.width+x];red.push_back((p>>16)&255);green.push_back((p>>8)&255);blue.push_back(p&255);}
    auto median=[](std::vector<int>& c){auto mid=c.begin()+c.size()/2;std::nth_element(c.begin(),mid,c.end());return *mid;};
    const auto color=0xff000000|(median(red)<<16)|(median(green)<<8)|median(blue);
    int same=0,n=0;for(int y=region.top;y<region.bottom;y+=std::max(1,int(region.bottom-region.top)/101)){++n;if(Delta(color,image.pixels[size_t(y)*image.width+x])<6)++same;}
    return same>=n*95/100?color:fallback;
}
}
StitchResult ScrollStitcher::Push(const Image& frame,std::optional<POINT> anchor) {
    analysis_={StitchResult::Unchanged,region_,0,0,locked_};
    auto result=[&](StitchResult r){analysis_.result=r;analysis_.region=region_;analysis_.locked=locked_;
        analysis_.currentY=currentY_;analysis_.minY=minY_;analysis_.maxY=maxY_;analysis_.viewportHeight=viewportHeight_;
        analysis_.upAdded=-minY_;analysis_.totalAdded=maxY_-minY_-viewportHeight_;return r;};
    if(frame.Empty())return result(StitchResult::Unmatched);
    if(first_.Empty()){
        if(frame.pixels.size()*4>budget_)return result(StitchResult::Limit);
        first_=previous_=frame;maxY_=viewportHeight_=frame.height;return result(StitchResult::First);
    }
    if(frame.width!=first_.width||frame.height!=first_.height)return result(StitchResult::RegionChanged);
    if(locked_&&anchor&&!PtInRect(&region_,*anchor))return result(StitchResult::RegionChanged);
    if(locked_&&region_.left>0&&region_.right<frame.width){
        // A resized panel can leave a perfectly matchable overlap while its new
        // lower edge is already outside the body. Do not append that fixed fill.
        auto encroached=[&](int y){const auto left=frame.pixels[size_t(y)*frame.width+region_.left-1],right=frame.pixels[size_t(y)*frame.width+region_.right];if(Delta(left,right)>3)return false;
            int oldDetail=0,background=0,n=0;for(int x=region_.left;x<region_.right;x+=2){++n;if(Delta(frame.pixels[size_t(y)*frame.width+x],left)<3)++background;if(Delta(previous_.pixels[size_t(y)*frame.width+x],left)>10)++oldDetail;}return background>n*98/100&&oldDetail>n/3;};
        // A separator line scrolling away exposes white padding and can make
        // isolated rows resemble the outside background. A resized viewport
        // must replace a continuous edge strip, not just two sampled text rows.
        bool bottomReplaced=region_.bottom-region_.top>=12;
        for(int y=region_.bottom-12;bottomReplaced&&y<region_.bottom;++y)bottomReplaced=encroached(y);
        if(bottomReplaced)return result(StitchResult::RegionChanged);
        // Horizontal shrink can preserve a perfect vertical match in the other
        // columns. Require two nearly complete new background columns, each
        // replacing substantial prior body detail, before treating it as resize.
        // Ordinary short lines/blank text padding do not meet that requirement.
        auto columnEncroached=[&](int x,int outside){int oldDetail=0,background=0,n=0;
            for(int y=region_.top;y<region_.bottom;y+=2){++n;
                const auto edge=frame.pixels[size_t(y)*frame.width+outside];
                if(Delta(frame.pixels[size_t(y)*frame.width+x],edge)<3)++background;
                if(Delta(previous_.pixels[size_t(y)*frame.width+x],previous_.pixels[size_t(y)*frame.width+outside])>10)++oldDetail;}
            return background>n*98/100&&oldDetail>n/3;};
        if((columnEncroached(region_.left,region_.left-1)&&columnEncroached(region_.left+4,region_.left-1))||
            (columnEncroached(region_.right-1,region_.right)&&columnEncroached(region_.right-5,region_.right)))return result(StitchResult::RegionChanged);
    }
    if(Difference(previous_,frame)<0.6)return result(StitchResult::Unchanged);
    const RECT bounds=locked_?region_:RECT{0,0,frame.width,frame.height};
    const auto match=Detect(previous_,frame,bounds,anchor);
    if(!match.valid){analysis_.ambiguous=match.ambiguous;return result(StitchResult::Unmatched);}
    // Once locked, never silently splice a newly resized inner viewport into
    // the old one. Small raster/blank-padding differences are tolerated.
    if(locked_&&(match.region.top>region_.top+8||match.region.bottom<region_.bottom-8)){
        // Moving text/blank rows can shorten the changed-pixel footprint without
        // resizing the viewport. Validate omitted margins against this same
        // displacement in both directions before retaining the locked bounds.
        double error=0;int samples=0,bad=0;
        auto compare=[&](uint32_t a,uint32_t b){const int d=Delta(a,b);error+=d;++samples;if(d>10)++bad;};
        for(int y=region_.top;y<region_.bottom;++y){
            if(y>=match.region.top&&y<match.region.bottom)continue;
            for(int x=region_.left;x<region_.right;x+=2){
                if(y+match.shift>=region_.top&&y+match.shift<region_.bottom)compare(frame.pixels[size_t(y)*frame.width+x],previous_.pixels[size_t(y+match.shift)*frame.width+x]);
                if(y-match.shift>=region_.top&&y-match.shift<region_.bottom)compare(previous_.pixels[size_t(y)*frame.width+x],frame.pixels[size_t(y-match.shift)*frame.width+x]);
            }
        }
        if(samples<32||error/samples>4||bad>samples/50)return result(StitchResult::RegionChanged);
    }
    analysis_.displacement=match.shift;analysis_.confidence=match.confidence;
    const RECT body=locked_?region_:match.region;
    const int bodyHeight=body.bottom-body.top;
    if(std::abs(match.shift)>=bodyHeight)return result(StitchResult::Unmatched);
    const int nextY=currentY_+match.shift,nextMin=std::min(minY_,nextY),nextMax=std::max(locked_?maxY_:bodyHeight,nextY+bodyHeight);
    if(uint64_t(first_.height+nextMax-nextMin-bodyHeight)*first_.width*4>budget_)return result(StitchResult::Limit);
    if(!locked_){
        region_=match.region;locked_=true;maxY_=viewportHeight_=bodyHeight;background_.resize(first_.width);
        if(region_.bottom<first_.height)footer_=first_.Crop(0,region_.bottom,first_.width,first_.height-region_.bottom);
        const auto left=SideColor(first_,{0,region_.top,region_.left,region_.bottom}),right=SideColor(first_,{region_.right,region_.top,first_.width,region_.bottom});
        for(int x=0;x<first_.width;++x)background_[x]=Background(first_,x,region_,x<region_.left?left:right);
    }
    // Save only genuinely new body intervals. Repeated visits move the current
    // viewport without growing the image or replacing previously captured rows.
    StitchResult accepted=StitchResult::Revisited;
    if(nextMin<minY_){
        strips_.insert(strips_.begin(),{nextMin,frame.Crop(region_.left,region_.top,region_.right-region_.left,minY_-nextMin)});
        accepted=StitchResult::Prepended;
    }else if(nextMax>maxY_){
        strips_.push_back({maxY_,frame.Crop(region_.left,region_.top+maxY_-nextY,region_.right-region_.left,nextMax-maxY_)});
        accepted=StitchResult::Appended;
    }
    if(nextY+bodyHeight>=maxY_&&region_.bottom<frame.height)footer_=frame.Crop(0,region_.bottom,frame.width,frame.height-region_.bottom);
    currentY_=nextY;minY_=nextMin;maxY_=nextMax;previous_=frame;
    return result(accepted);
}
uint32_t ScrollStitcher::Pixel(int x,int y)const{
    if(!locked_||y<region_.top)return first_.pixels[size_t(y)*first_.width+x];
    const int added=maxY_-minY_-viewportHeight_,footerTop=region_.bottom+added;
    if(y>=footerTop)return footer_.pixels[size_t(y-footerTop)*first_.width+x];
    if(x<region_.left||x>=region_.right)return y<region_.bottom?first_.pixels[size_t(y)*first_.width+x]:background_[x];
    const int bodyY=minY_+y-region_.top;
    if(bodyY>=0&&bodyY<viewportHeight_)return first_.pixels[size_t(region_.top+bodyY)*first_.width+x];
    const auto it=std::upper_bound(strips_.begin(),strips_.end(),bodyY,[](int yy,const Strip& strip){return yy<strip.begin;});
    const auto& strip=*(it-1);return strip.image.pixels[size_t(bodyY-strip.begin)*strip.image.width+x-region_.left];
}
Image ScrollStitcher::Flatten()const{
    if(first_.Empty())return {};Image out(Width(),Height());
    for(int y=0;y<out.height;++y)for(int x=0;x<out.width;++x)out.pixels[size_t(y)*out.width+x]=Pixel(x,y);return out;
}
Image ScrollStitcher::Preview(int maxWidth,int maxHeight)const{
    if(first_.Empty())return {};const double scale=std::min({1.0,double(maxWidth)/Width(),double(maxHeight)/Height()});Image out(std::max(1,int(Width()*scale)),std::max(1,int(Height()*scale)));
    for(int y=0;y<out.height;++y)for(int x=0;x<out.width;++x)out.pixels[size_t(y)*out.width+x]=Pixel(std::min(Width()-1,int(x/scale)),std::min(Height()-1,int(y/scale)));return out;
}
double ScrollStitcher::Difference(const Image& a,const Image& b,std::optional<POINT> anchor)const{
    if(a.Empty()||a.width!=b.width||a.height!=b.height)return 255;
    RECT r=locked_?region_:RECT{0,0,a.width,a.height};
    if(anchor){RECT local{std::max(r.left,anchor->x-160),std::max(r.top,anchor->y-160),std::min(r.right,anchor->x+160),std::min(r.bottom,anchor->y+160)};if(local.right>local.left&&local.bottom>local.top)r=local;}
    double maximum=0;
    for(int yy=r.top;yy<r.bottom;yy+=64)for(int xx=r.left;xx<r.right;xx+=64){double error=0;int n=0;for(int y=yy;y<std::min<int>(yy+64,r.bottom);y+=3)for(int x=xx;x<std::min<int>(xx+64,r.right);x+=3){error+=Delta(a.pixels[size_t(y)*a.width+x],b.pixels[size_t(y)*b.width+x]);++n;}maximum=std::max(maximum,error/std::max(1,n));}return maximum;
}
void ScrollStitcher::MapAnnotations(std::vector<Annotation>& annotations,UINT dpi)const{
    if(!locked_)return;
    annotations=MapScrollAnnotations(annotations,region_,Width(),first_.height,-minY_,maxY_-minY_-viewportHeight_,dpi);
}
void ScrollEndDetector::Observe(uint64_t id,int64_t time,int direction,StitchResult result,bool stable){
    if(!id||id<=lastId_)return;lastId_=id;
    Reset();
    if(!direction||result!=StitchResult::Unchanged||!stable)return;
    time_=time;direction_=direction>0?1:-1;active_=true;
}
bool ScrollEndDetector::Ready(int64_t now,int64_t stableSince)const{return active_&&!notified_&&now-time_>=600&&now-stableSince>=300;}
}
