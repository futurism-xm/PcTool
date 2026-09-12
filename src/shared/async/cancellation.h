#pragma once
#include <atomic>
#include <cstdint>
namespace capture {
struct Cancellation {
    std::atomic<bool> requested{false};
    uint64_t generation{};
    bool Accepts(uint64_t id) const { return !requested.load() && id==generation; }
};
}
