#include "long_capture/scroll_stitcher.h"
#ifdef PCTOOL_SCROLL_BASELINE
#include "baseline_stitcher.h"
using ReferenceStitcher=capture::BaselineScrollStitcher;
#else
using ReferenceStitcher=capture::ScrollStitcher;
#endif
#include <chrono>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <functional>

using namespace capture;
using Clock=std::chrono::steady_clock;
static void Require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
static Image Pattern(int width,int height,uint32_t seed=12345) {
    Image image(width,height);
    for(auto& pixel:image.pixels){seed=seed*1664525+1013904223;pixel=0xff000000|(seed&0xffffff);}
    return image;
}
static Image TextPage(int width,int height) {
    Image image(width,height);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};
    void* bits{};HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);throw std::runtime_error("Cannot allocate offscreen text fixture");}
    auto old=SelectObject(dc,bitmap);RECT fill{0,0,width,height};FillRect(dc,&fill,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei UI");
    auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
    for(int row=0;row*30<height;++row){const auto line=L"第 "+std::to_wstring(row)+L" 行 "+std::to_wstring(row*7919)+L"：长截图测试，文本内容与滚动位置。 Request "+std::to_wstring(row*7919)+L" completed successfully.";TextOutW(dc,20,row*30,line.c_str(),int(line.size()));}
    GdiFlush();std::copy_n(static_cast<uint32_t*>(bits),image.pixels.size(),image.pixels.begin());
    SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    for(auto& pixel:image.pixels)pixel|=0xff000000;
    return image;
}
struct Fixture {
    const char* name;int width,height,step;RECT body;Image page;
    Image Frame(int offset)const {
        Image frame(width,height);std::fill(frame.pixels.begin(),frame.pixels.end(),0xffeceef0);
        for(int y=0;y<body.bottom-body.top;++y)
            std::copy_n(page.pixels.data()+size_t(y+offset)*page.width,page.width,frame.pixels.data()+size_t(y+body.top)*width+body.left);
        // Distinct fixed header and bottommost footer make accidental duplication visible.
        for(int y=0;y<body.top;++y)for(int x=0;x<width;++x)frame.pixels[size_t(y)*width+x]=0xff224466;
        for(int y=body.bottom;y<height;++y)for(int x=0;x<width;++x)frame.pixels[size_t(y)*width+x]=0xff553300|uint32_t(offset&255);
        return frame;
    }
    Image Expected(int initial,int minimum,int maximum)const {
        const int added=maximum-minimum;Image image(width,height+added);
        const auto first=Frame(initial),last=Frame(maximum);
        for(int y=0;y<image.height;++y)for(int x=0;x<width;++x){uint32_t pixel;
            if(y<body.top)pixel=first.pixels[size_t(y)*width+x];
            else if(y>=body.bottom+added)pixel=last.pixels[size_t(y-added)*width+x];
            else if(x>=body.left&&x<body.right)pixel=page.pixels[size_t(y-body.top+minimum)*page.width+x-body.left];
            else pixel=y<body.bottom?first.pixels[size_t(y)*width+x]:0xffeceef0;
            image.pixels[size_t(y)*width+x]=pixel;
        }
        return image;
    }
};
static Fixture MakeFixture(const std::string& name) {
    if(name=="pattern1080")return {"pattern1080",1920,1080,144,{0,0,1920,1080},Pattern(1920,1080+144*5)};
    if(name=="pattern4k")return {"pattern4k",3840,2160,288,{0,0,3840,2160},Pattern(3840,2160+288*5)};
    if(name=="text")return {"text",900,660,60,{0,0,900,660},TextPage(900,1200)};
    return {"nested",900,740,48,{320,330,500,530},Pattern(180,200+48*5)};
}
struct Pair { ReferenceStitcher reference;ScrollStitcher current; };
static void SameAnalysis(const Pair& pair) {
    const auto& a=pair.reference.Analysis();const auto& b=pair.current.Analysis();
    Require(int(a.result)==int(b.result)&&EqualRect(&a.region,&b.region)&&a.displacement==b.displacement&&a.confidence==b.confidence&&
        a.locked==b.locked&&a.ambiguous==b.ambiguous&&a.currentY==b.currentY&&a.minY==b.minY&&a.maxY==b.maxY&&
        a.viewportHeight==b.viewportHeight&&a.upAdded==b.upAdded&&a.totalAdded==b.totalAdded,"Stitch analysis differs from baseline");
    Require(pair.reference.Width()==pair.current.Width()&&pair.reference.Height()==pair.current.Height(),"Stitch dimensions differ from baseline");
}
static void SameImage(const Image& a,const Image& b) {
    Require(a.width==b.width&&a.height==b.height&&a.pixels==b.pixels,"Stitch image pixels differ");
}
static void CheckPreview(const ScrollStitcher& stitcher,const Image& flat,int width,int height) {
    const auto preview=stitcher.Preview(width,height);
    const double scale=std::min({1.0,double(width)/flat.width,double(height)/flat.height});
    Require(preview.width==std::max(1,int(flat.width*scale))&&preview.height==std::max(1,int(flat.height*scale)),"Preview dimensions changed");
    for(int y=0;y<preview.height;++y)for(int x=0;x<preview.width;++x)
        Require(preview.pixels[size_t(y)*preview.width+x]==flat.pixels[size_t(std::min(flat.height-1,int(y/scale)))*flat.width+std::min(flat.width-1,int(x/scale))],"Preview differs from flattened nearest-neighbor reference");
}
static void Verify(const Pair& pair,const Image* expected=nullptr) {
    SameAnalysis(pair);const auto flat=pair.current.Flatten();SameImage(pair.reference.Flatten(),flat);
    if(expected)SameImage(flat,*expected);
    for(auto size:{SIZE{450,450},SIZE{91,137},SIZE{1,1}}){SameImage(pair.reference.Preview(size.cx,size.cy),pair.current.Preview(size.cx,size.cy));CheckPreview(pair.current,flat,size.cx,size.cy);}
}
static void Push(Pair& pair,const Image& frame,std::optional<POINT> anchor={}) {
    const auto original=pair.reference.Push(frame,anchor);const auto current=pair.current.Push(frame,anchor);
    Require(int(original)==int(current),"Stitch decision differs from baseline");SameAnalysis(pair);
}
static Pair Seed(const Fixture& fixture,const std::vector<int>& offsets) {
    Pair pair;for(int offset:offsets)Push(pair,fixture.Frame(offset));return pair;
}
struct Measurement {
    std::vector<double> values[2];
    void Print(const char* scenario,const char* phase,int round)const {
        for(int mode=0;mode<2;++mode){auto sorted=values[mode];std::sort(sorted.begin(),sorted.end());
            std::cout<<scenario<<','<<phase<<','<<(mode?"current":"baseline")<<','<<round<<','<<sorted.size()<<','
                <<sorted[sorted.size()/2]<<','<<sorted[(sorted.size()*95+99)/100-1]<<'\n';}
    }
};
template<class Setup,class Run,class Check>static void Measure(const char* scenario,const char* phase,int rounds,int samples,Setup setup,Run run,Check check) {
    Measurement total;
    for(int round=0;round<rounds;++round){Measurement result;
        for(int sample=-3;sample<samples;++sample){auto state=setup();
            for(int order=0;order<2;++order){const int mode=(sample+3+round+order)%2;
                const auto started=Clock::now();run(state,mode);
                const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
                if(sample>=0){result.values[mode].push_back(elapsed);total.values[mode].push_back(elapsed);}
            }
            check(state);
        }
        result.Print(scenario,phase,round+1);
    }
    total.Print(scenario,phase,0);
}
static void FixtureCases(const Fixture& fixture,bool benchmark,int rounds,int samples) {
    const int d=fixture.step;const auto first=fixture.Frame(d),second=fixture.Frame(2*d),third=fixture.Frame(3*d),top=fixture.Frame(0);
    const auto invalid=Pattern(fixture.width,fixture.height,987654321);
    const auto unlatched=Seed(fixture,{d}),locked=Seed(fixture,{d,2*d}),extended=Seed(fixture,{d,2*d,3*d});
    auto pushCase=[&](const char* phase,const Pair& seed,const Image& frame,StitchResult result,int minimum,int maximum){
        const auto expected=fixture.Expected(d,minimum,maximum);Pair check=seed;Push(check,frame);
        Require(check.current.Analysis().result==result,"Fixture did not exercise intended stitch result");Verify(check,&expected);
        if(benchmark)Measure(fixture.name,phase,rounds,samples,[&]{return seed;},[&](Pair& pair,int mode){if(mode)pair.current.Push(frame);else pair.reference.Push(frame);},[&](const Pair& pair){Verify(pair,&expected);});
    };
    pushCase("first_lock",unlatched,second,StitchResult::Appended,d,2*d);
    pushCase("append",locked,third,StitchResult::Appended,d,3*d);
    pushCase("prepend",locked,top,StitchResult::Prepended,0,2*d);
    pushCase("revisit",extended,second,StitchResult::Revisited,d,3*d);
    pushCase("unchanged",locked,second,StitchResult::Unchanged,d,2*d);
    pushCase("reject",locked,invalid,StitchResult::Unmatched,d,2*d);
    if(benchmark){
        struct Differences{double value[2]{};};
        Measure(fixture.name,"difference",rounds,samples,[]{return Differences{};},[&](Differences& result,int mode){result.value[mode]=mode?locked.current.Difference(first,second):locked.reference.Difference(first,second);},[](const Differences& result){Require(result.value[0]==result.value[1],"Difference changed");});
        for(bool preview:{true,false}){
            struct Outputs{Image image[2];};
            Measure(fixture.name,preview?"preview":"flatten",rounds,samples,[]{return Outputs{};},[&](Outputs& result,int mode){result.image[mode]=mode?(preview?extended.current.Preview(450,450):extended.current.Flatten()):(preview?extended.reference.Preview(450,450):extended.reference.Flatten());},[](const Outputs& result){SameImage(result.image[0],result.image[1]);});
        }
    }
    std::cerr<<"VERIFY "<<fixture.name<<" first/append/prepend/revisit/unchanged/reject, full pixels and previews PASS\n";
}
static void StripCases(bool benchmark,int rounds,int samples) {
    Fixture fixture{"strips",180,200,8,{0,0,180,200},Pattern(180,200+8*130)};
    for(int count:{1,16,128}){
        Pair pair;Push(pair,fixture.Frame(0));for(int i=1;i<=count;++i)Push(pair,fixture.Frame(i*8));
        const auto expected=fixture.Expected(0,0,count*8);Verify(pair,&expected);
        const auto name="strips"+std::to_string(count);
        if(benchmark)for(bool preview:{true,false}){
            struct Outputs{Image image[2];};
            Measure(name.c_str(),preview?"preview":"flatten",rounds,samples,[]{return Outputs{};},[&](Outputs& result,int mode){result.image[mode]=mode?(preview?pair.current.Preview(450,450):pair.current.Flatten()):(preview?pair.reference.Preview(450,450):pair.reference.Flatten());},[](const Outputs& result){SameImage(result.image[0],result.image[1]);});
        }
        std::cerr<<"VERIFY "<<name<<" full pixels and previews PASS\n";
    }
}
int main(int argc,char** argv) {
    try {
        bool benchmark=false;int samples=30,rounds=3;std::string selected="all";
        for(int i=1;i<argc;++i){const std::string argument=argv[i];
            if(argument=="--benchmark")benchmark=true;
            else if(argument=="--verify")benchmark=false;
            else if(argument=="--samples"&&i+1<argc)samples=std::stoi(argv[++i]);
            else if(argument=="--rounds"&&i+1<argc)rounds=std::stoi(argv[++i]);
            else if(argument=="--case"&&i+1<argc)selected=argv[++i];
            else throw std::runtime_error("Unknown or incomplete benchmark argument");
        }
        Require(samples>0&&rounds>0,"Samples and rounds must be positive");
        Require(selected=="all"||selected=="pattern1080"||selected=="pattern4k"||selected=="text"||selected=="nested"||selected=="strips","Unknown benchmark case");
#ifndef PCTOOL_SCROLL_BASELINE
        Require(!benchmark,"Build the paired benchmark with run_scroll_stitcher_performance.ps1 first");
#endif
        if(benchmark)std::cout<<std::fixed<<std::setprecision(6)<<"scenario,phase,implementation,round,samples,median_ms,p95_ms\n";
        for(const char* name:{"pattern1080","pattern4k","text","nested"})if(selected=="all"||selected==name)FixtureCases(MakeFixture(name),benchmark,rounds,samples);
        if(selected=="all"||selected=="strips")StripCases(benchmark,rounds,samples);
        std::cerr<<"SCROLL STITCHER VERIFICATION PASS\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
