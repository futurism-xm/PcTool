#include "translation/source_confirmation.h"
#include "shared/ui/confirmation.h"
namespace translation {
bool HandleSourceConfirmationMessage(MSG& message){return shared_ui::HandleSourceConfirmationMessage(message);}
bool ConfirmSourceAction(HWND owner,const std::wstring& title,const std::wstring& message,const std::wstring& accept,const std::wstring& cancel){return shared_ui::ConfirmSourceAction(owner,title,message,accept,cancel);}
}
