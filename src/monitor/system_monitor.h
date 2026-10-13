#pragma once

#include <windows.h>
#include <pdh.h>

#include <chrono>
#include <cstdint>
#include <vector>
#include "monitor/gpu_monitor.h"

struct SystemMetrics {
    double uploadBytesPerSecond{};
    double downloadBytesPerSecond{};
    double cpuUsagePercent{};
    double gpuUsagePercent{-1};
    DWORD memoryUsagePercent{};
    double cpuFrequencyGhz{};
};

class SystemMonitor final {
public:
    SystemMonitor();
    ~SystemMonitor();

    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    void Sample(bool includeCpuFrequency, bool includeGpuUsage = false);
    void SetGpuEnabled(bool enabled) { gpuSampler_.Enable(enabled); metrics_.gpuUsagePercent = gpuSampler_.Value(); }
    [[nodiscard]] const SystemMetrics& Metrics() const noexcept { return metrics_; }

private:
    struct ProcessorPowerInformation {
        ULONG number;
        ULONG maxMhz;
        ULONG currentMhz;
        ULONG mhzLimit;
        ULONG maxIdleState;
        ULONG currentIdleState;
    };

    static std::uint64_t FileTimeToUInt64(const FILETIME& value) noexcept;
    void SampleCpuUsage();
    void SampleMemory();
    void SampleNetwork();
    void SampleCpuFrequency();

    SystemMetrics metrics_{};
    gpu_monitor::Sampler gpuSampler_;
    std::uint64_t previousIdleTime_{};
    std::uint64_t previousKernelTime_{};
    std::uint64_t previousUserTime_{};
    std::uint64_t previousReceivedBytes_{};
    std::uint64_t previousSentBytes_{};
    bool cpuInitialized_{};
    bool networkInitialized_{};
    std::chrono::steady_clock::time_point previousNetworkSample_;
    std::vector<ProcessorPowerInformation> processorPowerBuffer_;
    PDH_HQUERY cpuFrequencyQuery_{};
    PDH_HCOUNTER cpuFrequencyCounter_{};
    double maximumCpuFrequencyGhz_{};
};
