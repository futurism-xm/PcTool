#include "translation/source_menu_probe.h"
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace translation {
std::future<SourceMenuState> ProbeSourceMenu(DWORD process){
    return std::async(std::launch::async,[process]{
        SourceMenuState result;
        const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(initialized))return result;
        {
            Microsoft::WRL::ComPtr<IUIAutomation2> automation;
            if(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))){
                automation->put_ConnectionTimeout(100);
                automation->put_TransactionTimeout(100);
                Microsoft::WRL::ComPtr<IUIAutomationElement> element;
                Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker;
                if(SUCCEEDED(automation->GetFocusedElement(&element))&&element&&SUCCEEDED(automation->get_ControlViewWalker(&walker))){
                    for(int depth=0;depth<8&&element;++depth){
                        int pid{},type{};
                        if(FAILED(element->get_CurrentProcessId(&pid))||DWORD(pid)!=process||FAILED(element->get_CurrentControlType(&type)))break;
                        result.available=true;
                        if(type==UIA_MenuControlTypeId||type==UIA_MenuBarControlTypeId||type==UIA_MenuItemControlTypeId){result.menu=true;break;}
                        Microsoft::WRL::ComPtr<IUIAutomationElement> parent;
                        if(FAILED(walker->GetParentElement(element.Get(),&parent)))break;
                        element=std::move(parent);
                    }
                }
            }
        }
        CoUninitialize();return result;
    });
}
}
