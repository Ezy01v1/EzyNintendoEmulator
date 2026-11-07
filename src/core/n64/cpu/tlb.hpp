#pragma once

#include <array>
#include <cstdint>

#include "n64/cpu/cop0.hpp"

namespace ezy::n64 {

// The VR4300's joint TLB: 32 entries, each mapping a pair of pages (even and odd) of 4 KB
// to 16 MB (PageMask). An entry matches a virtual address when the region bits (63:62)
// and VPN2 agree outside the page mask, and either it is global or its ASID is the
// current one (EntryHi).
class Tlb {
public:
    struct Result {
        bool hit = false;
        bool valid = false;
        bool dirty = false;
        bool cached = true;
        std::uint32_t paddr = 0;
    };

    [[nodiscard]] Result translate(std::uint64_t vaddr, std::uint8_t asid) const;
    void write(int index, const Cop0& cop0);  // TLBWI / TLBWR
    void read(int index, Cop0& cop0) const;   // TLBR
    [[nodiscard]] int probe(const Cop0& cop0) const;  // TLBP: index, or -1

    template <class Ar>
    void serialize(Ar& ar) {
        ar(entries_);
    }

private:
    struct Entry {
        std::uint64_t mask = 0;      // PageMask | 0x1FFF: the bits below VPN2
        std::uint64_t entry_hi = 0;  // R, VPN2 (ASID in the low byte)
        std::uint64_t lo[2] = {0, 0};
        std::uint8_t global = 0;
        std::uint8_t pad[7] = {};
    };
    [[nodiscard]] static bool matches(const Entry& e, std::uint64_t vaddr, std::uint8_t asid);

    std::array<Entry, 32> entries_{};
};

}  // namespace ezy::n64
