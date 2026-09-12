#include "shared/async/cancellation.h"
#include "ocr/ocr_service.h"
#include "shared/platform/capture_platform.h"
#include <winrt/base.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cwctype>
#include <future>
#include <chrono>

namespace {
std::wstring Compact(std::wstring value) {value.erase(std::remove_if(value.begin(),value.end(),[](wchar_t c){return iswspace(c)!=0;}),value.end());return value;}
size_t Distance(std::wstring a,std::wstring b) {
    a=Compact(a);b=Compact(b);std::vector<size_t> row(b.size()+1);for(size_t i=0;i<row.size();++i)row[i]=i;
    for(size_t i=1;i<=a.size();++i){size_t previous=row[0];row[0]=i;for(size_t j=1;j<=b.size();++j){size_t old=row[j];row[j]=std::min({row[j]+1,row[j-1]+1,previous+(a[i-1]!=b[j-1])});previous=old;}}return row.back();
}
}
void CompareOcr(const std::wstring& prefix) {
    using namespace capture;
    std::ofstream report(std::filesystem::path(prefix+L".txt"));size_t nativeErrors=0,localErrors=0,total=0;
    const std::vector<std::wstring> lines={L"文字识别：已完成窗口大小调整与功能栏修复。",L"中文与 English 混排，支持数字 0123456789。",L"CPU频率 3.39 GHz 内存 53% 上传 0.1 K/s",L"docker compose pull && docker compose up -d",L"https://example.com/api/v1?limit=100&offset=20",L"const std::wstring text = L\"你好，世界！\";"};
    std::wstring truth;for(auto& s:lines)truth+=s+L"\n";
    auto languages=OcrLanguages();if(languages.empty())throw std::runtime_error("Comparison requires Windows OCR");std::wstring language=languages.front().tag;for(auto& l:languages)if(l.tag.rfind(L"zh-Hans",0)==0)language=l.tag;
    Image last;
    for(int size:{14,18,28}) {
        Image image(1200,360);std::fill(image.pixels.begin(),image.pixels.end(),0xffffffff);
        HBITMAP bitmap=ToBitmap(image);HDC dc=CreateCompatibleDC(nullptr);auto previous=SelectObject(dc,bitmap);
        HFONT font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(25,25,25));
        for(size_t i=0;i<lines.size();++i)TextOutW(dc,20,20+int(i)*50,lines[i].c_str(),int(lines[i].size()));
        SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,previous);DeleteDC(dc);image=FromBitmap(bitmap);DeleteObject(bitmap);
        SavePng(image,prefix+L"-"+std::to_wstring(size)+L".png");Cancellation cancel;
        auto start=GetTickCount64();auto native=RecognizeDocument(image,language,cancel);auto nativeMs=GetTickCount64()-start;
        start=GetTickCount64();auto local=RecognizeLocalDocument(image,cancel);auto localMs=GetTickCount64()-start;
        const auto ne=Distance(truth,native.text),le=Distance(truth,local.text);nativeErrors+=ne;localErrors+=le;total+=Compact(truth).size();
        report<<"font="<<size<<" windows_errors="<<ne<<" local_errors="<<le<<" windows_ms="<<nativeMs<<" local_ms="<<localMs<<"\nWindows:\n"<<winrt::to_string(native.text)<<"\n"<<winrt::to_string(LocalOcrModelName())<<":\n"<<winrt::to_string(local.text)<<"\n\n";report.flush();
        std::cout<<"OCR font="<<size<<" windows_errors="<<ne<<" local_errors="<<le<<" local_ms="<<localMs<<std::endl;
        if(local.text.empty())throw std::runtime_error("Offline OCR returned no text");
        for(auto& p:local.paragraphs)for(auto& l:p.lines)if(l.x<0||l.y<0||l.x+l.width>image.width||l.y+l.height>image.height)throw std::runtime_error("Offline OCR returned invalid original coordinates");
        last=std::move(image);
    }
    Cancellation cancel;Image blank(800,400);std::fill(blank.pixels.begin(),blank.pixels.end(),0xffffffff);if(!RecognizeLocalDocument(blank,cancel).text.empty())throw std::runtime_error("Blank offline OCR returned text");
    Image tall(1200,3600);for(int i=0;i<10;++i)std::copy(last.pixels.begin(),last.pixels.end(),tall.pixels.begin()+size_t(i)*last.pixels.size());
    auto longResult=RecognizeLocalDocument(tall,cancel);size_t count=0,pos=0;while((pos=longResult.text.find(L"docker compose pull",pos))!=std::wstring::npos){++count;++pos;}
    if(count!=10)throw std::runtime_error("Offline long-image tile lost or duplicated lines: "+std::to_string(count));
    Cancellation stop;auto work=std::async(std::launch::async,[&]{try{RecognizeLocalDocument(tall,stop);}catch(...){};});Sleep(30);stop.requested=true;
    if(work.wait_for(std::chrono::seconds(2))!=std::future_status::ready)throw std::runtime_error("Offline cancellation exceeded two seconds");work.get();
    report<<"TOTAL chars="<<total<<" windows_errors="<<nativeErrors<<" local_errors="<<localErrors<<"\nBlank, coordinates, long tiles and cancellation PASS\n";
    std::cout<<"OCR COMPARISON chars="<<total<<" Windows="<<nativeErrors<<" Local="<<localErrors<<"; blank/coordinates/tiles/cancel PASS\n";
    if(localErrors>=nativeErrors)throw std::runtime_error("Offline OCR did not improve this evaluation corpus");
}
