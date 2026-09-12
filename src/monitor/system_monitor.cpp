#include "monitor/system_monitor.h"

#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <pdhmsg.h>
#include <powrprof.h>

#include <algorithm>
#include <cmath>

SystemMonitor::SystemMonitor() {
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    processorPowerBuffer_.resize(systemInfo.dwNumberOfProcessors);

    if (!processorPowerBuffer_.empty()) {
        const ULONG bufferSize =
            static_cast<ULONG>(processorPowerBuffer_.size() * sizeof(ProcessorPowerInformation));
        if (CallNtPowerInformation(
                ProcessorInformation, nullptr, 0,
                processorPowerBuffer_.data(), bufferSize) == ERROR_SUCCESS) {
            ULONG maximumMhz = 0;
            for (const auto& processor : processorPowerBuffer_) {
                maximumMhz = std::max(maximumMhz, processor.maxMhz);
            }
            maximumCpuFrequencyGhz_ = static_cast<double>(maximumMhz) / 1000.0;
        }
    }

    if (maximumCpuFrequencyGhz_ > 0.0 &&
        PdhOpenQueryW(nullptr, 0, &cpuFrequencyQuery_) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(cpuFrequencyQuery_,
                L"\\Processor Information(_Total)\\% Processor Performance",
                0, &cpuFrequencyCounter_) != ERROR_SUCCESS ||
            PdhCollectQueryData(cpuFrequencyQuery_) != ERROR_SUCCESS) {
            PdhCloseQuery(cpuFrequencyQuery_);
            cpuFrequencyQuery_ = nullptr;
            cpuFrequencyCounter_ = nullptr;
        }
    }
}

SystemMonitor::~SystemMonitor() {
    if (cpuFrequencyQuery_) {
        PdhCloseQuery(cpuFrequencyQuery_);
    }
}

void SystemMonitor::Sample(bool includeCpuFrequency) {
    SampleCpuUsage();
    SampleMemory();
    SampleNetwork();
    if (includeCpuFrequency) {
        SampleCpuFrequency();
    }
}

std::uint64_t SystemMonitor::FileTimeToUInt64(const FILETIME& value) noexcept {
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

void SystemMonitor::SampleCpuUsage() {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return;
    }

    const std::uint64_t idleValue = FileTimeToUInt64(idle);
    const std::uint64_t kernelValue = FileTimeToUInt64(kernel);
    const std::uint64_t userValue = FileTimeToUInt64(user);

    if (cpuInitialized_) {
        const std::uint64_t idleDelta = idleValue - previousIdleTime_;
        const std::uint64_t kernelDelta = kernelValue - previousKernelTime_;
        const std::uint64_t userDelta = userValue - previousUserTime_;
        const std::uint64_t total = kernelDelta + userDelta;
        if (total > 0) {
            const std::uint64_t busy = total > idleDelta ? total - idleDelta : 0;
            metrics_.cpuUsagePercent = std::clamp(
                static_cast<double>(busy) * 100.0 / static_cast<double>(total), 0.0, 100.0);
        }
    }

    previousIdleTime_ = idleValue;
    previousKernelTime_ = kernelValue;
    previousUserTime_ = userValue;
    cpuInitialized_ = true;
}

void SystemMonitor::SampleMemory() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        metrics_.memoryUsagePercent = memory.dwMemoryLoad;
    }
}

void SystemMonitor::SampleNetwork() {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) {
        return;
    }

    std::uint64_t received = 0;
    std::uint64_t sent = 0;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IF_ROW2& row = table->Table[index];
        if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        received += row.InOctets;
        sent += row.OutOctets;
    }
    FreeMibTable(table);

    const auto now = std::chrono::steady_clock::now();
    if (networkInitialized_) {
        const double seconds = std::chrono::duration<double>(now - previousNetworkSample_).count();
        const std::uint64_t receivedDelta =
            received >= previousReceivedBytes_ ? received - previousReceivedBytes_ : 0;
        const std::uint64_t sentDelta = sent >= previousSentBytes_ ? sent - previousSentBytes_ : 0;
        if (seconds > 0.0) {
            metrics_.downloadBytesPerSecond = static_cast<double>(receivedDelta) / seconds;
            metrics_.uploadBytesPerSecond = static_cast<double>(sentDelta) / seconds;
        }
    }

    previousReceivedBytes_ = received;
    previousSentBytes_ = sent;
    previousNetworkSample_ = now;
    networkInitialized_ = true;
}

void SystemMonitor::SampleCpuFrequency() {
    if (cpuFrequencyQuery_ && cpuFrequencyCounter_ &&
        PdhCollectQueryData(cpuFrequencyQuery_) == ERROR_SUCCESS) {
        DWORD counterType = 0;
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(
                cpuFrequencyCounter_, PDH_FMT_DOUBLE,
                &counterType, &value) == ERROR_SUCCESS &&
            (value.CStatus == PDH_CSTATUS_VALID_DATA ||
             value.CStatus == PDH_CSTATUS_NEW_DATA) &&
            std::isfinite(value.doubleValue) && value.doubleValue >= 0.0) {
            metrics_.cpuFrequencyGhz =
                maximumCpuFrequencyGhz_ * value.doubleValue / 100.0;
            return;
        }
    }

    // Compatibility fallback for systems that do not expose the English PDH
    // counter. Some firmware reports only the nominal value through this API.
    if (processorPowerBuffer_.empty()) {
        return;
    }
    const std::size_t processorCount = processorPowerBuffer_.size();
    const ULONG bufferSize =
        static_cast<ULONG>(processorPowerBuffer_.size() * sizeof(ProcessorPowerInformation));
    if (CallNtPowerInformation(
            ProcessorInformation, nullptr, 0, processorPowerBuffer_.data(), bufferSize) != ERROR_SUCCESS) {
        return;
    }

    std::uint64_t totalMhz = 0;
    for (std::size_t index = 0; index < processorCount; ++index) {
        totalMhz += processorPowerBuffer_[index].currentMhz;
    }
    metrics_.cpuFrequencyGhz =
        static_cast<double>(totalMhz) / static_cast<double>(processorCount) / 1000.0;
}
