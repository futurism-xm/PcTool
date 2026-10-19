#pragma once
#include <filesystem>
#include <string>

namespace startup_registration {
std::wstring ValueName(const std::filesystem::path& executable);
// Current user's ordinary Run entry. Tests always use an isolated namespace.
bool Apply(bool enabled,const std::filesystem::path& executable,std::wstring& error,
           const std::wstring& nameSpace=L"PcTool");
}
