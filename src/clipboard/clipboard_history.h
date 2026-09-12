#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ClipboardHistoryEntry {
    uint64_t id{};
    std::wstring text;
};
class ClipboardHistoryModel {
public:
    bool Add(std::wstring text);
    const std::vector<std::shared_ptr<const ClipboardHistoryEntry>>& Entries() const { return entries_; }
    static std::wstring Summary(const std::wstring& text);
private:
    uint64_t nextId_=1;
    std::vector<std::shared_ptr<const ClipboardHistoryEntry>> entries_;
};
class ClipboardHistory {
public:
    ClipboardHistory();
    ~ClipboardHistory();
    bool Start();
    void Show();
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

