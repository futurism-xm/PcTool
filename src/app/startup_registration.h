#pragma once
#include <filesystem>
#include <string>

namespace startup_registration {
// Namespace is explicit so tests cannot touch the production task or Run key.
bool Apply(bool enabled,const std::filesystem::path& executable,std::wstring& error,
           const std::wstring& nameSpace=L"PcTool");
}
