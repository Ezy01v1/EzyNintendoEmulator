#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ezy::n64 {

// Two hidden DZ bits per RDRAM halfword. Direct lookup avoids a tree search and
// allocation in the pixel loop. Save states retain the ordered sparse v11 format.
class RdpHiddenBits {
public:
    static constexpr std::size_t kHalfwords = 8 * 1024 * 1024 / 2;
    RdpHiddenBits() : values_(kHalfwords) {}

    [[nodiscard]] unsigned read(std::size_t index) const {
        return index < values_.size() ? values_[index] : 0;
    }
    void write(std::size_t index, unsigned bits) {
        if (index >= values_.size()) return;
        const auto value = static_cast<std::uint8_t>(bits & 3);
        count_ -= values_[index] != 0;
        count_ += value != 0;
        values_[index] = value;
    }
    [[nodiscard]] std::uint32_t count() const { return count_; }
    void clear() {
        if (count_) std::fill(values_.begin(), values_.end(), std::uint8_t{0});
        count_ = 0;
    }
    void clear(std::size_t first, std::size_t end) {
        if (!count_) return;
        end = std::min(end, values_.size());
        first = std::min(first, end);
        std::uint32_t removed = 0;
        for (auto i = first; i < end; ++i) {
            removed += values_[i] != 0;
            values_[i] = 0;
        }
        count_ -= removed;
    }
    template<class Ar> void serialize(Ar& ar) {
        auto count = count_;
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
    std::uint32_t count_ = 0;
};
}
