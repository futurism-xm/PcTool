#pragma once
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gpu_monitor {
// GPU Engine counters are per process. Sum processes sharing the same physical
// engine, then use the busiest engine (also across adapters), not their sum.
inline double Usage(const PDH_FMT_COUNTERVALUE_ITEM_W* items, DWORD count) {
    std::unordered_map<std::wstring, double> engines;
    for (DWORD i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (!item.szName || (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) ||
            !std::isfinite(item.FmtValue.doubleValue) || item.FmtValue.doubleValue < 0) continue;
        const std::wstring name(item.szName);
        const auto start = name.find(L"luid_");
        const auto end = name.find(L"_engtype_", start);
        if (start == std::wstring::npos || end == std::wstring::npos ||
            name.find(L"_phys_", start) == std::wstring::npos ||
            name.find(L"_eng_", start) == std::wstring::npos) continue;
        engines[name.substr(start, end - start)] += item.FmtValue.doubleValue;
    }
    if (engines.empty()) return -1;
    double result = 0;
    for (const auto& engine : engines) result = std::max(result, engine.second);
    return std::clamp(result, 0.0, 100.0);
}

class Sampler final {
public:
    ~Sampler() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void Enable(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        ++revision_;
        value_.store(-1);
        if (enabled && !worker_.joinable()) {
            try { worker_ = std::thread([this] { Run(); }); }
            catch (...) { enabled_ = false; }
        }
        wake_.notify_all();
    }
    double Value() const noexcept { return value_.load(); }
private:
    void Run() noexcept {
        PDH_HQUERY query{};
        PDH_HCOUNTER counter{};
        std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer;
        auto nextOpen = std::chrono::steady_clock::time_point{};
        auto close = [&] { if (query) PdhCloseQuery(query); query = nullptr; counter = nullptr; };
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_) {
                if (!enabled_) {
                    lock.unlock(); close(); buffer.clear(); buffer.shrink_to_fit(); lock.lock();
                    wake_.wait(lock, [&] { return stopping_ || enabled_; });
                    nextOpen = {};
                    continue;
                }
                const auto revision = revision_;
                lock.unlock();
                double result = -1;
                const auto now = std::chrono::steady_clock::now();
                if (!query && now >= nextOpen) {
                    nextOpen = now + std::chrono::seconds(30);
                    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
                        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) close();
                }
                if (query) {
                    const auto collected = PdhCollectQueryData(query);
                    if (collected == ERROR_SUCCESS) {
                        DWORD bytes = 0, count = 0;
                        auto status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
                        // Process instances can change between sizing and reading.
                        for (int retry = 0; retry < 3 && status == PDH_MORE_DATA && bytes <= 16 * 1024 * 1024; ++retry) {
                            buffer.resize((bytes + sizeof(buffer[0]) - 1) / sizeof(buffer[0]));
                            status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, buffer.data());
                        }
                        if (status == ERROR_SUCCESS && count <= buffer.size()) result = Usage(buffer.data(), count);
                    } else if (collected != PDH_NO_DATA) close();
                }
                lock.lock();
                if (enabled_ && revision_ == revision) value_.store(result);
                if (revision_ != revision) { lock.unlock(); close(); lock.lock(); nextOpen = {}; continue; }
                wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_ || revision_ != revision; });
            }
        } catch (...) { value_.store(-1); }
        close();
    }
    std::atomic<double> value_{-1};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool enabled_{};
    bool stopping_{};
    unsigned revision_{};
};
}
