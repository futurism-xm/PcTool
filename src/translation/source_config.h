#pragma once
#include <string>
#include <vector>
#include <filesystem>
namespace translation {
enum class SourceKind { OpenAI, DeepL, Azure, Baidu };
struct SourceConfig {
    std::wstring id,name,endpoint,key,model,region,appId;
    SourceKind kind{SourceKind::OpenAI};
    bool pro{},enabled{},verified{};
};
const wchar_t* SourceName(SourceKind kind);
SourceConfig NewSource(SourceKind kind);
std::wstring ValidateConfig(const SourceConfig& source);
bool ContainsHan(const std::wstring& text);
class SourceStore {
public:
    explicit SourceStore(std::filesystem::path directory={});
    std::vector<SourceConfig> Load();
    void Save(const std::vector<SourceConfig>& sources);
    const std::filesystem::path& Directory() const {return directory_;}
private:
    std::filesystem::path directory_;
};
}
