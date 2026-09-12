#pragma once
#include <windows.h>
#include <algorithm>
#include <functional>
#include <cstdint>

namespace shared_ui {
struct ScrollGeometry {
    int top{},length{},travel{},maximum{};
    ScrollGeometry(int height,int total,int page,int position,int minimum){
        maximum=std::max(0,total-page);
        length=std::min(height,std::max(minimum,int(int64_t(height)*page/std::max(1,total))));
        travel=std::max(0,height-length);
        top=maximum?int(int64_t(travel)*std::clamp(position,0,maximum)/maximum):0;
    }
    int Position(int offset)const{return travel?int(int64_t(std::clamp(offset,0,travel))*maximum/travel):0;}
};
// Both text and viewport scrolling use the same arrowless, DPI-scaled control.
class SlimScrollbar {
public:
    HWND Create(HWND parent,int id,std::function<void(int)> changed,COLORREF background);
    void Update(int total,int page,int position,UINT dpi);
    HWND Window()const{return window_;}
    int Position()const{return position_;}
    int Page()const{return page_;}
    int Maximum()const{return std::max(0,total_-page_);}
private:
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    ScrollGeometry Geometry()const;
    void Move(int position);
    HWND window_{};std::function<void(int)> changed_;
    int total_{},page_{},position_{},grab_{};UINT dpi_{96};COLORREF background_{};bool dragging_{};
};
// A separate layered owned window leaves the popup client and native EDIT opaque.
class PopupShadow {
public:
    explicit PopupShadow(const wchar_t* className=L"PcTool.PopupShadow"):className_(className){}
    ~PopupShadow();
    void Sync(HWND owner,UINT dpi);
    void Hide();
    void Close();
private:
    const wchar_t* className_;
    HWND window_{};
    int width_{},height_{},margin_{};
};
}
