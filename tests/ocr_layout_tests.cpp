#include "ocr/ocr_service.h"
#include <stdexcept>
#include <iostream>
using namespace capture;
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){try {
    auto result=GroupOcrLines(800,1200,{{10,10,300,20,L"first"},{10,38,320,20,L"continuation"},{10,100,250,20,L"second"},{10,125,250,20,L"1. new list"},{10,150,250,20,L"2. next"},{500,150,250,20,L"other column"}});
    Require(result.paragraphs.size()==5,"paragraph boundaries");
    Require(result.paragraphs[0].lines.size()==2,"merge neighboring lines");
    Require(result.text.substr(result.paragraphs[0].begin,result.paragraphs[0].end-result.paragraphs[0].begin)==L"first\r\ncontinuation","full text spans");
    for(size_t i=0;i<result.paragraphs.size();++i) Require(result.paragraphs[i].number==i+1,"number ordering");
    std::vector<OcrTextLine> lines;
    AppendOcrTextLines(lines,{{20,100,150,20,L"first tile"},{20,170,150,20,L"overlap"}},0,0,200);
    AppendOcrTextLines(lines,{{20,20,150,20,L"overlap"},{20,70,150,20,L"next tile"}},150,150,300);
    Require(lines.size()==3 && lines.back().y==220 && lines.back().x==20,"tile ownership, duplicate and coordinates");
    auto columns=GroupOcrLines(800,600,{{0,0,100,20,L"left"},{400,25,100,20,L"right"},{0,70,100,20,L"left again"}});
    Require(columns.paragraphs.size()==3,"different columns must not merge");
    Require(GroupOcrLines(100,100,{}).text.empty(),"blank result");
    std::cout<<"OCR LAYOUT PASS: paragraph grouping, columns, lists, ranges and tile geometry\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
