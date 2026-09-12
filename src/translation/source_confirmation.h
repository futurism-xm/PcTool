#pragma once
#include <windows.h>
#include <string>

namespace translation {
bool HandleSourceConfirmationMessage(MSG& message);
bool ConfirmSourceAction(HWND owner, const std::wstring& title,
    const std::wstring& message, const std::wstring& accept,
    const std::wstring& cancel = L"继续编辑");
}
