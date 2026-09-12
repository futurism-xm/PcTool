#include "ocr/ocr_text_merge.h"
namespace capture {
void AppendOcrLines(std::vector<OcrLine>& out,const std::vector<OcrLine>& lines,int tileTop,int ownTop,int ownBottom) {
    for(auto line:lines) {
        line.y+=tileTop;
        const double center=line.y+line.height/2;
        if(center<ownTop || center>=ownBottom) continue;
        const bool duplicate=std::any_of(out.rbegin(),out.rend(),[&](const OcrLine& p) {
            return p.text==line.text && std::abs(p.y-line.y)<std::max(p.height,line.height)*0.65;
        });
        if(!duplicate) out.push_back(std::move(line));
    }
}
}
