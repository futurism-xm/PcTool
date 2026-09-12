#pragma once
#include "translation/source_config.h"
#include <windows.h>
#include <functional>
#include <memory>
namespace translation {
bool HandleSourceSettingsMessage(MSG&);
class SourceSettings {
public:
    SourceSettings(SourceStore&,std::vector<SourceConfig>&,std::function<void()> changed);
    ~SourceSettings();
    void Show(HWND owner);
    void Close();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
