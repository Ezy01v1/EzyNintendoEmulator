#pragma once

#include <cstdint>
#include <vector>

// Public header of ezy_jit. One byte per 4 KiB guest RAM page: nonzero = the page holds
// compiled code. Compiled stores read table() directly, so the layout is part of the contract.
namespace ezy::jit {

class CodePages {
public:
    static constexpr std::uint32_t kPageShift = 12;

    explicit CodePages(std::uint32_t ram_size) : table_((ram_size + 0xFFFU) >> kPageShift, 0) {}

    [[nodiscard]] const std::uint8_t* table() const { return table_.data(); }
    void mark(std::uint32_t page) { if (page < table_.size()) table_[page] = 1; }
    void clear(std::uint32_t page) { if (page < table_.size()) table_[page] = 0; }
    void clear_all() { table_.assign(table_.size(), 0); }

    // True if any page overlapping [begin, end) is marked; addresses past RAM are ignored.
    [[nodiscard]] bool marked(std::uint32_t begin, std::uint32_t end) const {
        if (end <= begin) return false;
        const std::uint32_t last = (end - 1) >> kPageShift;
        for (std::uint32_t p = begin >> kPageShift; p <= last && p < table_.size(); ++p)
            if (table_[p] != 0) return true;
        return false;
    }

private:
    std::vector<std::uint8_t> table_;
};

}  // namespace ezy::jit
