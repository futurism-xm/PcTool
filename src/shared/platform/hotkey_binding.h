#pragma once
#include <windows.h>
#include <array>
#include <string>
namespace hotkeys {
struct Binding {
    UINT modifiers{},key{};
    bool operator==(const Binding& other) const {return modifiers==other.modifiers&&key==other.key;}
    bool operator!=(const Binding& other) const {return !(*this==other);}
};
enum class Action : size_t { Screenshot,Clipboard,InputTranslation,CaptureTranslation,SelectionTranslation,TaskManager,Paint,Archive,Settings,Count };
using Bindings=std::array<Binding,size_t(Action::Count)>;
inline constexpr Bindings Defaults{{{MOD_CONTROL|MOD_ALT,'A'},{MOD_ALT,'C'},{MOD_ALT,'Q'},{MOD_ALT,'W'},{MOD_ALT,'E'},{},{},{},{}}};
inline constexpr const wchar_t* Names[]={L"屏幕截图",L"剪切板历史",L"输入翻译",L"截图翻译",L"划词翻译",L"任务管理器",L"调用画图",L"7-zip",L"快捷键管理"};
inline constexpr size_t DisplayOrder[]={2,3,4,0,5,1,6,7,8};
inline bool Empty(Binding b){return b.key==0&&b.modifiers==0;}
inline bool Valid(Binding b){
    if(Empty(b))return true;
    const bool function=b.key>=VK_F1&&b.key<=VK_F24;
    return !(b.modifiers&~(MOD_ALT|MOD_CONTROL|MOD_SHIFT))&&
        (function||(b.key>='A'&&b.key<='Z')||(b.key>='0'&&b.key<='9'))&&
        (function||(b.modifiers&(MOD_CONTROL|MOD_ALT)));
}
inline bool Valid(const Bindings& values){for(size_t i=0;i<values.size();++i){if(!Valid(values[i]))return false;if(Empty(values[i]))continue;for(size_t j=0;j<i;++j)if(values[i]==values[j])return false;}return true;}
inline std::wstring Text(Binding b){
    if(Empty(b))return {};
    std::wstring text;if(b.modifiers&MOD_CONTROL)text+=L"Ctrl+";if(b.modifiers&MOD_ALT)text+=L"Alt+";if(b.modifiers&MOD_SHIFT)text+=L"Shift+";
    if(b.key>=VK_F1&&b.key<=VK_F24)text+=L"F"+std::to_wstring(b.key-VK_F1+1);else text+=wchar_t(b.key);return text;
}
}
