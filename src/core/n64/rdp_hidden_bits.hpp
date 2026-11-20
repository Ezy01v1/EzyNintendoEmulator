#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ezy::n64 {

// Two hidden bits per RDRAM halfword (depth DZ low bits, 16-bit color coverage low bits).
// Direct lookup avoids a tree search and allocation in the pixel loop. Save states retain the ordered sparse v11 format.
// The threaded RDP's band workers write disjoint halfwords of one shared table, so there
// is no exact counter, only a flag saying whether any bit may be set (which keeps clears
// and saves of an unused table free).
class RdpHiddenBits {
public:
    static constexpr std::size_t kHalfwords = 8 * 1024 * 1024 / 2;
    RdpHiddenBits() : values_(kHalfwords) {}

    [[nodiscard]] unsigned read(std::size_t index) const {
        return index < values_.size() ? values_[index] : 0;
    }
    void write(std::size_t index, unsigned bits) {
        if (index >= values_.size()) return;
        values_[index] = static_cast<std::uint8_t>(bits & 3);
        if ((bits & 3) && !used_.load(std::memory_order_relaxed)) used_.store(true, std::memory_order_relaxed);
    }
    // Halfwords with nonzero bits (a scan: only for save states and tests).
    [[nodiscard]] std::uint32_t count() const {
        if (!used_.load(std::memory_order_relaxed)) return 0;
        return static_cast<std::uint32_t>(
            std::count_if(values_.begin(), values_.end(), [](std::uint8_t value) { return value != 0; }));
    }
    void clear() {
        if (!used_.load(std::memory_order_relaxed)) return;
        std::fill(values_.begin(), values_.end(), std::uint8_t{0});
        used_.store(false, std::memory_order_relaxed);
    }
    void clear(std::size_t first, std::size_t end) {
        if (!used_.load(std::memory_order_relaxed)) return;
        end = std::min(end, values_.size());
        first = std::min(first, end);
        std::fill(values_.begin() + static_cast<std::ptrdiff_t>(first), values_.begin() + static_cast<std::ptrdiff_t>(end),
                  std::uint8_t{0});
    }
    template<class Ar> void serialize(Ar& ar) {
        auto count = this->count();
        ar(count);
        if constexpr (Ar::kLoading) {
            clear();
            if (ar.failed() || count > values_.size()) { ar.fail(); return; }
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t halfword = 0;
                std::uint8_t bits = 0;
                ar(halfword); ar(bits);
                if (ar.failed() || halfword >= values_.size() || bits > 3) { ar.fail(); return; }
                if (bits) write(halfword, bits); // Legacy zero entries leave an earlier duplicate unchanged.
            }
        } else if (count) {
            for (std::uint32_t halfword = 0; halfword < values_.size(); ++halfword) {
                auto bits = values_[halfword];
                if (bits) { ar(halfword); ar(bits); }
            }
        }
    }
private:
    std::vector<std::uint8_t> values_;
    std::atomic<bool> used_{false};  // some bit may be set; only clear() resets it
};
}
