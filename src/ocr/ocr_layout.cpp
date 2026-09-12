#include "ocr/ocr_service.h"
#include <cwctype>
#include <cmath>
namespace capture {
void AppendOcrTextLines(std::vector<OcrTextLine>& out,const std::vector<OcrTextLine>& lines,int tileTop,int ownTop,int ownBottom) {
    for(auto line:lines) {
        line.y+=tileTop; const double center=line.y+line.height/2;
        if(center<ownTop || center>=ownBottom) continue;
        bool duplicate=std::any_of(out.rbegin(),out.rend(),[&](const auto& p) {
            return p.text==line.text && std::abs(p.y-line.y)<std::max(p.height,line.height)*0.65 && std::abs(p.x-line.x)<std::max(p.height,line.height);
        });
        if(!duplicate) out.push_back(std::move(line));
    }
}
static bool ListStart(const std::wstring& text) {
    size_t i=text.find_first_not_of(L" \t"); if(i==std::wstring::npos) return false;
    if(std::wstring(L"•●▪-—*（(").find(text[i])!=std::wstring::npos) return true;
    size_t end=i; while(end<text.size() && (iswdigit(text[end]) || std::wstring(L"一二三四五六七八九十").find(text[end])!=std::wstring::npos)) ++end;
    return end>i && end<text.size() && std::wstring(L".、．)）").find(text[end])!=std::wstring::npos;
}
OcrDocument GroupOcrLines(int width,int height,std::vector<OcrTextLine> lines) {
    OcrDocument result; result.width=width; result.height=height;
    std::stable_sort(lines.begin(),lines.end(),[](const auto& a,const auto& b){ return a.y!=b.y?a.y<b.y:a.x<b.x; });
    for(auto& line:lines) {
        if(line.text.empty() || line.width<=0 || line.height<=0) continue;
        bool merge=false;
        if(!result.paragraphs.empty()) {
            const auto& previous=result.paragraphs.back().lines.back();
            const double maxHeight=std::max(previous.height,line.height),gap=line.y-previous.y-previous.height;
            merge=!ListStart(line.text) && maxHeight/std::min(previous.height,line.height)<=1.5 &&
                gap>=-maxHeight*0.2 && gap<=maxHeight*0.8 && std::abs(line.x-previous.x)<=maxHeight*0.75 &&
                std::min(previous.x+previous.width,line.x+line.width)>std::max(previous.x,line.x);
        }
        if(!merge) {
            if(!result.text.empty()) result.text+=L"\r\n\r\n";
            result.paragraphs.push_back({result.paragraphs.size()+1,result.text.size(),0,{}});
        } else result.text+=L"\r\n";
        result.text+=line.text; result.paragraphs.back().lines.push_back(std::move(line)); result.paragraphs.back().end=result.text.size();
    }
    return result;
}
}
