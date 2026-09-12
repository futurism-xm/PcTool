#pragma once

#include <cstdint>
#include "shared/platform/hotkey_binding.h"

enum class MonitorItem : std::uint32_t {
    UploadSpeed = 1u << 0,
    DownloadSpeed = 1u << 1,
    CpuUsage = 1u << 2,
    MemoryUsage = 1u << 3,
    CpuFrequency = 1u << 5,
};

constexpr std::uint32_t ToMask(MonitorItem item) noexcept {
    return static_cast<std::uint32_t>(item);
}

constexpr std::uint32_t kAllMonitorItems =
    ToMask(MonitorItem::UploadSpeed) |
    ToMask(MonitorItem::DownloadSpeed) |
    ToMask(MonitorItem::CpuUsage) |
    ToMask(MonitorItem::MemoryUsage) |
    ToMask(MonitorItem::CpuFrequency);

class AppSettings final {
public:
    void Load();
    bool Save() const;

    [[nodiscard]] bool IsItemVisible(MonitorItem item) const noexcept;
    bool ToggleItem(MonitorItem item) noexcept;

    [[nodiscard]] bool AutoStartEnabled() const noexcept { return autoStartEnabled_; }
    void SetAutoStartEnabled(bool enabled) noexcept { autoStartEnabled_ = enabled; }
    [[nodiscard]] bool TranslationEnabled() const noexcept { return translationEnabled_; }
    void SetTranslationEnabled(bool enabled) noexcept { translationEnabled_ = enabled; }
    [[nodiscard]] bool ScreenshotEnabled() const noexcept { return screenshotEnabled_; }
    void SetScreenshotEnabled(bool enabled) noexcept { screenshotEnabled_ = enabled; }
    [[nodiscard]] bool MonitorInTaskbar() const noexcept { return monitorInTaskbar_; }
    void SetMonitorInTaskbar(bool enabled) noexcept { monitorInTaskbar_ = enabled; }

    bool ApplyAutoStart() const;
    const hotkeys::Bindings& Hotkeys() const noexcept {return hotkeys_;}
    void SetHotkeys(const hotkeys::Bindings& keys) {hotkeys_=keys;}

private:
    std::uint32_t visibleItems_{kAllMonitorItems};
    bool autoStartEnabled_{true};
    bool translationEnabled_{true};
    bool screenshotEnabled_{true};
    bool monitorInTaskbar_{true};
    hotkeys::Bindings hotkeys_{hotkeys::Defaults};
};
