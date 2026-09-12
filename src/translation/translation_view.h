#pragma once
#include "translation/translation_submission.h"
#include "shared/ui/popup_controls.h"
#include <windows.h>
#include <algorithm>
#include <functional>
#include <cstdint>

namespace translation {
struct PopupStyle {
    static constexpr int Width=430,FontSize=14,LineHeight=21,ScrollbarWidth=6;
    static constexpr COLORREF InputBackground=RGB(229,229,229),ResultBackground=RGB(237,237,237),Text=RGB(96,98,102);
};
using shared_ui::ScrollGeometry;
using shared_ui::SlimScrollbar;
using shared_ui::PopupShadow;
struct PopupLayout {
    int inputBottom{},contentHeight{};
    RECT input{},inputScrollbar{},configure{};
    std::vector<RECT> cards,headers,bodies,toggles;
    PopupLayout()=default;
    PopupLayout(int width,UINT dpi,int inputHeight,const std::vector<int>& resultHeights,const std::vector<SourceCard>& sources){
        auto d=[dpi](int n){return MulDiv(n,dpi,96);};
        // The scrollbar lives in the card's existing right padding, not in an
        // extra gutter. Its visibility never changes text wrapping or margins.
        inputScrollbar={width-d(12+3+PopupStyle::ScrollbarWidth),d(13),width-d(12+3),d(13)+inputHeight};
        input={d(28),d(13),inputScrollbar.left-d(2),d(13)+inputHeight};
        inputBottom=input.bottom+d(13);
        int y=inputBottom+d(12);
        cards.resize(sources.size());headers.resize(sources.size());bodies.resize(sources.size());toggles.resize(sources.size());
        for(size_t i=0;i<sources.size();++i){
            const int height=i<resultHeights.size()?resultHeights[i]:0;
            const int body=sources[i].expanded?height+d(10):0;
            headers[i]={d(12),y,width-d(12),y+d(35)};
            toggles[i]={headers[i].right-d(36),y+d(4),headers[i].right-d(6),y+d(31)};
            cards[i]={d(12),y,width-d(12),y+d(35)+body};
            bodies[i]={d(25),y+d(35),width-d(25),y+d(35)+height};
            y=cards[i].bottom+d(8);
        }
        if(sources.empty()){configure={d(12),y,width-d(12),y+d(35)};y=configure.bottom+d(8);}
        contentHeight=y;
    }
};
}
