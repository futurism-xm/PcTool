#include "ocr/ocr_text_merge.h"
#include "recording/recording_timeline.h"
#include "long_capture/scroll_stitcher.h"
#include "shared/async/cancellation.h"
#include "shared/image/image_document.h"
#include <iostream>
#include <stdexcept>

using namespace capture;
static void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
static Image Pattern(int width,int height) {
    Image result(width,height); uint32_t seed=12345;
    for(auto& p:result.pixels) { seed=seed*1664525+1013904223; p=0xff000000|(seed&0xffffff); }
    return result;
}
int main() {
    try {
        // Real rasterized text has repeated line prefixes and large white gaps;
        // random-noise fixtures cannot expose those first-scroll failures.
        {
            Image text(900,2000);BITMAPINFO bi{};bi.bmiHeader={sizeof(BITMAPINFOHEADER),900,-2000,1,32,BI_RGB};
            void* bits{};HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
            auto old=SelectObject(dc,bitmap);RECT fill{0,0,900,2000};FillRect(dc,&fill,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            HFONT font=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Microsoft YaHei UI");auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
            for(int row=0;row<66;++row){const auto line=L"第 "+std::to_wstring(row)+L" 行 "+std::to_wstring(row*7919)+L"：长截图测试，文本内容与滚动位置。 Request "+std::to_wstring(row*7919)+L" completed successfully.";TextOutW(dc,20,row*30,line.c_str(),int(line.size()));}
            GdiFlush();std::copy_n(static_cast<uint32_t*>(bits),text.pixels.size(),text.pixels.data());SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
            for(int delta:{15,30,60}){ScrollStitcher actualText;actualText.Push(text.Crop(0,0,900,660));const auto result=actualText.Push(text.Crop(0,delta,900,660),POINT{450,330});
                std::cerr<<"raster text delta="<<delta<<" result="<<int(result)<<" shift="<<actualText.Analysis().displacement<<"\n";
                Require(result==StitchResult::Appended&&actualText.Analysis().displacement==delta,"real text first scroll must match its actual displacement");}
            auto textList=[&](int offset){Image f(900,740);std::fill(f.pixels.begin(),f.pixels.end(),0xffeceef0);for(int y=0;y<360;++y)std::copy_n(text.pixels.data()+size_t(y+offset)*900,450,f.pixels.data()+size_t(y+240)*900+300);return f;};
            ScrollStitcher textSteps;textSteps.Push(textList(600));for(int offset:{550,500,450,400,350,300,250,200,150,100,50,0}){
                const auto region=textSteps.Analysis().region;
                const POINT point=textSteps.Analysis().locked?POINT{(region.left+region.right)/2,(region.top+region.bottom)/2}:POINT{500,400};
                const auto r=textSteps.Push(textList(offset),point);
                std::cerr<<"text list offset="<<offset<<" result="<<int(r)<<" current="<<textSteps.Analysis().currentY<<"\n";
                Require(r==StitchResult::Prepended&&textSteps.Analysis().currentY==offset-600,"text line padding must not be treated as a resized viewport");}
            const auto textOutput=textSteps.Flatten();for(int y=0;y<960;++y)for(int x=0;x<450;++x)
                if((textOutput.pixels[size_t(y+240)*900+x+300]&0xffffff)!=(text.pixels[size_t(y)*900+x]&0xffffff)){
                    const auto r=textSteps.Analysis().region;std::cerr<<"text mismatch "<<x<<","<<y<<" region="<<r.left<<","<<r.top<<","<<r.right<<","<<r.bottom<<"\n";
                    throw std::runtime_error("entire text line moves together, including repeated suffixes");}
        }
        Image page=Pattern(180,1000);
        ScrollStitcher stitch;
        auto first=page.Crop(0,0,180,200),second=page.Crop(0,65,180,200);
        Require(stitch.Push(first)==StitchResult::First,"first frame");
        Require(stitch.Push(first)==StitchResult::Unchanged,"duplicate frame");
        Require(stitch.Push(second)==StitchResult::Appended,"scroll append");
        Require(stitch.Height()==265,"correct stitched height");
        Require(stitch.Flatten().pixels==page.Crop(0,0,180,265).pixels,"lossless seam");
        Require(stitch.Push(first)==StitchResult::Revisited,"reverse revisits captured content");
        Require(stitch.Height()==265&&stitch.Analysis().currentY==0,"reverse preserves output and tracks current position");
        ScrollStitcher animated; animated.Push(first);
        auto noisy=second;
        for(int y=0;y<noisy.height;++y) for(int x=130;x<180;++x) noisy.pixels[size_t(y)*180+x]=0xffaabbcc;
        Require(animated.Push(noisy)==StitchResult::Appended,"animated sidebar does not break alignment");
        Require(stitch.Push(page.Crop(0,800,180,200))==StitchResult::Unmatched,"no overlap rejected");
        auto preview=stitch.Preview(90,100);
        Require(preview.width<=90 && preview.height<=100,"bounded preview");
        ScrollStitcher limited(180*240*4);
        limited.Push(first); Require(limited.Push(second)==StitchResult::Limit,"memory limit");
        Require(limited.Height()==200,"limit preserves last good image");
        auto header=Pattern(180,20),footer=Pattern(180,16);
        auto viewport=[&](int offset) {
            Image image(180,200);
            auto body=page.Crop(0,offset,180,164);
            std::copy(header.pixels.begin(),header.pixels.end(),image.pixels.begin());
            std::copy(body.pixels.begin(),body.pixels.end(),image.pixels.begin()+180*20);
            std::copy(footer.pixels.begin(),footer.pixels.end(),image.pixels.begin()+180*184);
            return image;
        };
        ScrollStitcher sticky;
        sticky.Push(viewport(0));
        Require(sticky.Push(viewport(60))==StitchResult::Appended,"sticky frame append");
        Require(sticky.Push(viewport(120))==StitchResult::Appended,"second sticky frame append");
        Image expected(180,320);
        auto body=page.Crop(0,0,180,284);
        std::copy(header.pixels.begin(),header.pixels.end(),expected.pixels.begin());
        std::copy(body.pixels.begin(),body.pixels.end(),expected.pixels.begin()+180*20);
        std::copy(footer.pixels.begin(),footer.pixels.end(),expected.pixels.begin()+180*304);
        Require(sticky.Flatten().pixels==expected.pixels,"sticky header/footer appear exactly once");
        Image flat(180,200); ScrollStitcher blank; blank.Push(flat);
        std::fill(flat.pixels.begin(),flat.pixels.end(),0xffffffff);
        Require(blank.Push(flat)==StitchResult::Unmatched,"blank ambiguity rejected");

        // A narrow nested viewport surrounded by a majority of fixed pixels.
        const RECT nested{320,330,500,530};const POINT anchor{410,410};
        auto nestedFrame=[&](int offset){Image image(900,740);std::fill(image.pixels.begin(),image.pixels.end(),0xffeceef0);
            for(int y=20;y<300;y+=45)for(int yy=y;yy<y+14;++yy)for(int x=20;x<120;++x)image.pixels[size_t(yy)*900+x]=0xff123456;
            for(int y=345;y<515;++y)for(int x=30;x<90;++x)image.pixels[size_t(y)*900+x]=0xff123456;
            for(int y=0;y<200;++y)std::copy_n(page.pixels.data()+size_t(y+offset)*180,180,image.pixels.data()+size_t(y+330)*900+320);
            for(int x=0;x<900;++x)image.pixels[size_t(700)*900+x]=0xff556677;return image;};
        ScrollStitcher partial;auto base=nestedFrame(0);partial.Push(base);
        Require(partial.Push(nestedFrame(65),anchor)==StitchResult::Appended,"narrow nested scroll detected");
        Require(EqualRect(&partial.Analysis().region,&nested),"nested region exact bounds");
        Require(partial.Push(nestedFrame(130),anchor)==StitchResult::Appended,"nested next append");
        const auto lastNested=nestedFrame(130);auto nestedOutput=partial.Flatten();Require(nestedOutput.width==900&&nestedOutput.height==870,"whole selection retained");
        for(int y=0;y<nestedOutput.height;++y)for(int x=0;x<900;++x){uint32_t expectedPixel;
            if(y<530)expectedPixel=base.pixels[size_t(y)*900+x];
            else if(y>=660)expectedPixel=lastNested.pixels[size_t(y-130)*900+x];
            else if(x>=320&&x<500)expectedPixel=page.pixels[size_t(y-330)*180+x-320];
            else expectedPixel=0xffeceef0;
            Require(nestedOutput.pixels[size_t(y)*900+x]==expectedPixel,"nested body/static/background composition");}
        Require(partial.Push(nestedFrame(65),anchor)==StitchResult::Revisited,"nested reverse revisits known interval");
        Require(partial.Push(nestedFrame(130),anchor)==StitchResult::Revisited,"return to lower accepted frame");
        Require(partial.Push(nestedFrame(195),POINT{100,410})==StitchResult::RegionChanged,"different panel cannot replace locked target");
        Require(partial.Height()==870,"rejected target preserves image");
        Require(partial.Push(nestedFrame(195),anchor)==StitchResult::Appended,"resume original panel");
        std::vector<Annotation> marks(2);marks[0].start={30,700};marks[0].end={90,720};marks[1].start={330,350};marks[1].end={390,370};partial.MapAnnotations(marks);
        Require(marks[0].start.y==895&&marks[1].start.y==350,"static footer annotations mapped once");
        ScrollStitcher lowBudget(size_t(900)*800*4);lowBudget.Push(base);Require(lowBudget.Push(nestedFrame(65),anchor)==StitchResult::Limit,"budget counts full output width");
        auto tile=Pattern(180,16);
        auto repeated=[&](int offset){Image f(180,200);for(int y=0;y<200;++y)std::copy_n(tile.pixels.data()+size_t((y+offset)%16)*180,180,f.pixels.data()+size_t(y)*180);return f;};
        ScrollStitcher ambiguous;ambiguous.Push(repeated(0));Require(ambiguous.Push(repeated(7))==StitchResult::Unmatched,"periodic ambiguity cannot append");Require(ambiguous.Analysis().ambiguous,"periodic failure identifies the reason");
        ScrollStitcher outsideAnimation;outsideAnimation.Push(base);Require(outsideAnimation.Push(nestedFrame(65),anchor)==StitchResult::Appended,"animation target locks");
        auto animatedOutside=nestedFrame(65);for(int y=30;y<90;++y)for(int x=650;x<750;++x)animatedOutside.pixels[size_t(y)*900+x]^=0xffffff;
        Require(outsideAnimation.Push(animatedOutside,anchor)==StitchResult::Unchanged,"outside animation ignored after lock");
        auto changedLayout=nestedFrame(130);for(int y=500;y<530;++y)for(int x=320;x<500;++x)changedLayout.pixels[size_t(y)*900+x]=0xffeceef0;
        const auto resized=outsideAnimation.Push(changedLayout,anchor);Require(resized==StitchResult::RegionChanged||resized==StitchResult::Unmatched,"resized inner viewport rejected");
        Require(outsideAnimation.Height()==805&&outsideAnimation.Push(nestedFrame(130),anchor)==StitchResult::Appended,"layout recovery preserves accumulated content");
        for(bool leftSide:{false,true}){
            ScrollStitcher horizontalLayout;horizontalLayout.Push(base);horizontalLayout.Push(nestedFrame(65),anchor);
            auto narrower=nestedFrame(130);for(int y=330;y<530;++y)for(int x=leftSide?320:440;x<(leftSide?380:500);++x)narrower.pixels[size_t(y)*900+x]=0xffeceef0;
            Require(horizontalLayout.Push(narrower,anchor)==StitchResult::RegionChanged,"horizontal body shrink cannot append new sidebar background");
            Require(horizontalLayout.Height()==805&&horizontalLayout.Analysis().currentY==65,"horizontal layout failure preserves coordinates");
            Require(horizontalLayout.Push(nestedFrame(130),anchor)==StitchResult::Appended,"restore horizontal layout resumes original body");
        }
        auto twoPanels=[&](int offset){auto f=nestedFrame(offset);for(int y=0;y<200;++y)std::copy_n(page.pixels.data()+size_t(y+offset)*180,180,f.pixels.data()+size_t(y+330)*900+600);return f;};
        ScrollStitcher singleTarget;singleTarget.Push(twoPanels(0));Require(singleTarget.Push(twoPanels(65),anchor)==StitchResult::Appended,"one of two moving panels selected");
        Require(EqualRect(&singleTarget.Analysis().region,&nested),"same-shift adjacent list not merged into selected target");
        // A narrow scrollbar shares the detector's last band with the body.
        // On upward motion its thumb can sit entirely in the new top strip;
        // it must still be excluded rather than copied at every prepend seam.
        auto textPage=Pattern(363,1440);
        for(int y=0;y<1440;++y)for(int x=0;x<363;++x){
            const bool ink=y%48>=10&&y%48<24&&x>=8&&x<220;
            textPage.pixels[size_t(y)*363+x]=y%48==47?0xffdddddd:ink?((textPage.pixels[size_t(y)*363+x]>>16)&1?0xff303030:0xffffffff):0xffffffff;}
        auto withScrollbar=[&](int offset){Image f(1280,720);std::fill(f.pixels.begin(),f.pixels.end(),0xffeceef0);
            for(int y=290;y<341;++y)for(int x=340;x<720;++x)f.pixels[size_t(y)*1280+x]=0xffbbbbbb;
            for(int y=341;y<579;++y){std::copy_n(textPage.pixels.data()+size_t(offset+y-341)*363,363,f.pixels.data()+size_t(y)*1280+341);
                for(int x=704;x<719;++x)f.pixels[size_t(y)*1280+x]=0xfffcfcfc;}
            for(int y=355+offset/6;y<385+offset/6;++y)for(int x=707;x<716;++x)f.pixels[size_t(y)*1280+x]=0xff888888;
            for(int y:{345,573})for(int x=707;x<716;++x)f.pixels[size_t(y)*1280+x]=0xff888888;
            return f;};
        ScrollStitcher scrollbar;const auto scrollbarInitial=withScrollbar(144);scrollbar.Push(scrollbarInitial);
        Require(scrollbar.Push(withScrollbar(72),POINT{500,450})==StitchResult::Prepended,"text body prepends beside independently moving thumb");
        Require(scrollbar.Analysis().region.right<=705,"scrollbar thumb excluded from scrolling body");
        const auto scrollbarResult=scrollbar.Flatten();
        for(int y=341;y<579;++y)for(int x=707;x<719;++x)Require(scrollbarResult.pixels[size_t(y)*1280+x]==scrollbarInitial.pixels[size_t(y)*1280+x],"fixed scrollbar remains once at original position");
        // Begin midway through a document, prepend above the initial viewport,
        // revisit in both directions, then cross either previous boundary.
        ScrollStitcher bidirectional;bidirectional.Push(page.Crop(0,300,180,200));
        Require(bidirectional.Push(page.Crop(0,235,180,200))==StitchResult::Prepended,"first upward scroll locks and prepends");
        Require(bidirectional.Analysis().currentY==-65&&bidirectional.Analysis().minY==-65&&bidirectional.Analysis().maxY==200,"upward global coordinates");
        Require(bidirectional.Push(page.Crop(0,170,180,200))==StitchResult::Prepended,"second upward prepend");
        for(int offset:{235,300})Require(bidirectional.Push(page.Crop(0,offset,180,200))==StitchResult::Revisited,"return through known body does not duplicate");
        Require(bidirectional.Height()==330,"revisit retains accumulated interval");
        for(int offset:{365,430})Require(bidirectional.Push(page.Crop(0,offset,180,200))==StitchResult::Appended,"lower growth after upper capture");
        for(int offset:{365,300,235,170})Require(bidirectional.Push(page.Crop(0,offset,180,200))==StitchResult::Revisited,"downward range revisited upward");
        Require(bidirectional.Push(page.Crop(0,105,180,200))==StitchResult::Prepended,"upper boundary extends again");
        Require(bidirectional.Height()==525&&bidirectional.Flatten().pixels==page.Crop(0,105,180,525).pixels,"bidirectional output is complete with no duplicate or lost rows");
        Require(bidirectional.Analysis().currentY==-195&&bidirectional.Analysis().minY==-195&&bidirectional.Analysis().maxY==330&&bidirectional.Analysis().viewportHeight==200,"complete range coordinates");
        Require(bidirectional.Analysis().upAdded==195&&bidirectional.Analysis().totalAdded==325,"mapping offsets expose both ends");
        const auto acceptedHeight=bidirectional.Height();
        Require(bidirectional.Push(page.Crop(0,800,180,200))==StitchResult::Unmatched,"unsafe fast jump does not invent rows");
        Require(bidirectional.Height()==acceptedHeight&&bidirectional.Analysis().currentY==-195,"failed jump preserves coordinates");
        Require(bidirectional.Push(page.Crop(0,170,180,200))==StitchResult::Revisited,"return to matching position recovers after jump");
        ScrollStitcher upperLimit(180*240*4);upperLimit.Push(page.Crop(0,300,180,200));
        Require(upperLimit.Push(page.Crop(0,235,180,200))==StitchResult::Limit&&upperLimit.Height()==200,"prepend respects full-image budget before locking");

        auto changingFooter=[&](int offset){auto f=nestedFrame(offset);for(int y=705;y<725;++y)for(int x=50;x<850;++x)f.pixels[size_t(y)*900+x]=0xff112200|uint32_t(offset&255);return f;};
        ScrollStitcher nestedBoth;const auto middle=changingFooter(300);nestedBoth.Push(middle);
        Require(nestedBoth.Push(changingFooter(235),anchor)==StitchResult::Prepended,"nested first upward locks correct region");
        Require(EqualRect(&nestedBoth.Analysis().region,&nested),"nested upward exact region");
        for(int offset:{170,235,300,365,430,365,300,235,170,105}){
            const auto r=nestedBoth.Push(changingFooter(offset),anchor);Require(r==StitchResult::Appended||r==StitchResult::Prepended||r==StitchResult::Revisited,"mixed nested movement accepted");}
        const auto both=nestedBoth.Flatten(),bottom=changingFooter(430);Require(both.width==900&&both.height==1065,"nested two-ended full-width extent");
        for(int y=0;y<both.height;++y)for(int x=0;x<900;++x){uint32_t p;
            if(y<330)p=middle.pixels[size_t(y)*900+x];
            else if(y>=855)p=bottom.pixels[size_t(y-325)*900+x];
            else if(x>=320&&x<500)p=page.pixels[size_t(y-330+105)*180+x-320];
            else p=y<530?middle.pixels[size_t(y)*900+x]:0xffeceef0;
            Require(both.pixels[size_t(y)*900+x]==p,"nested prepend composition: original static sides once, contiguous body, bottommost footer");}
        ScrollEndDetector end;
        Require(!end.Ready(9000,0),"no scroll action never infers boundary");
        end.Observe(1,1000,-120,StitchResult::Unchanged,true);
        Require(!end.Ready(1599,1000)&&end.Ready(1600,1000)&&end.Direction()==-1,"single completed stable downward action detects bottom at 600ms");
        Require(!end.Ready(1700,1500),"recent animation blocks boundary inference");
        end.Notified();Require(!end.Ready(3000,1000),"one observation displays hint once");
        end.Observe(1,3000,-120,StitchResult::Unchanged,true);Require(!end.Ready(5000,1000),"duplicate observation cannot retrigger");
        end.Observe(2,3000,-120,StitchResult::Unchanged,true);Require(end.Ready(3600,3000),"fresh same-direction action rearms hint");
        end.Observe(3,4000,120,StitchResult::Unchanged,true);Require(end.Ready(4600,4000)&&end.Direction()==1,"upward no-motion detects top");
        end.Observe(2,4700,-120,StitchResult::Unchanged,true);Require(end.Direction()==1,"stale completed action cannot replace current direction");
        end.Observe(4,5000,120,StitchResult::Prepended,true);Require(!end.Ready(9000,5000),"new upper content clears boundary");
        end.Observe(5,6000,-120,StitchResult::Unmatched,true);Require(!end.Ready(9000,6000),"unmatched is never mistaken for boundary");
        end.Observe(6,7000,-120,StitchResult::Unchanged,false);Require(!end.Ready(9000,7000),"unstable action cannot confirm boundary");
        end.Observe(7,8000,-120,StitchResult::Unchanged,true);end.Reset();Require(!end.Ready(9000,8000),"cancel clears pending boundary");
        for(double zoom:{0.1,0.5,1.0,1.5,2.0}) {
            Viewport v{zoom,31,4531}; POINT original{123,5432};
            auto roundtrip=v.ToImage(v.ToClient(original,88),88);
            Require(std::abs(roundtrip.x-original.x)<=1/zoom+1 && std::abs(roundtrip.y-original.y)<=1/zoom+1,"viewport transform");
        }
        Timeline timeline; timeline.Start(100); timeline.Pause(200); timeline.Pause(250);
        Require(timeline.Elapsed(800)==100,"pause freezes clock");
        timeline.Resume(900); Require(timeline.Elapsed(1000)==200,"resume excludes pause");
        timeline.Pause(1100); timeline.Resume(1200); Require(timeline.Elapsed(1300)==400,"multiple pauses");
        Require(MixSample(0.7f,0.6f)==1.0f && MixSample(-0.8f,-0.5f)==-1.0f,"mix limiter");
        Require(std::abs(MixSample(0.2f,-0.1f)-0.1f)<0.001f,"mix preserves waveform");
        std::vector<OcrLine> lines;
        AppendOcrLines(lines,{{90,20,L"first"},{180,20,L"boundary"}},0,0,180);
        AppendOcrLines(lines,{{20,20,L"boundary"},{80,20,L"last"}},160,180,300);
        Require(lines.size()==3 && lines[1].text==L"boundary","OCR overlap ownership");
        Cancellation cancel; cancel.generation=7;
        Require(cancel.Accepts(7) && !cancel.Accepts(6),"generation rejects stale work");
        cancel.requested=true; Require(!cancel.Accepts(7),"closed session rejects results");
        std::cout<<"All capture core tests passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
