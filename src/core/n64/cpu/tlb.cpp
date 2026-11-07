#include "n64/cpu/tlb.hpp"

namespace ezy::n64 {

namespace {

// EntryHi fields compared on a lookup: R (63:62) and VPN2 (39:13).
constexpr std::uint64_t kRegionVpn2 = 0xC00000FFFFFFE000ULL;

}  // namespace

bool Tlb::matches(const Entry& e, std::uint64_t vaddr, std::uint8_t asid) {
    const std::uint64_t compare = kRegionVpn2 & ~e.mask;
    if ((vaddr & compare) != (e.entry_hi & compare)) return false;
    return e.global || static_cast<std::uint8_t>(e.entry_hi) == asid;
}

Tlb::Result Tlb::translate(std::uint64_t vaddr, std::uint8_t asid) const {
    for (const Entry& e : entries_) {
        if (!matches(e, vaddr, asid)) continue;
        const std::uint64_t page_size = (e.mask + 1) >> 1;  // the pair covers mask + 1 bytes
        const std::uint64_t lo = e.lo[(vaddr & page_size) ? 1 : 0];
        Result r;
        r.hit = true;
        r.valid = (lo & 2) != 0;
        r.dirty = (lo & 4) != 0;
        r.cached = ((lo >> 3) & 7) != 2;
        const std::uint64_t pfn = (lo >> 6) & 0xFFFFF;
        r.paddr = static_cast<std::uint32_t>(((pfn << 12) & ~(page_size - 1)) | (vaddr & (page_size - 1)));
        return r;
    }
    return {};
}

void Tlb::write(int index, const Cop0& cop0) {
    Entry& e = entries_[static_cast<std::size_t>(index & 31)];
    // PageMask bits come in pairs (4 KB, 16 KB, ... 16 MB); the TLB keeps a pair only when
    // its upper bit is set (n64-systemtest: 0x2000 reads back as 0).
    std::uint64_t mask = 0;
    const std::uint64_t written = cop0.read(Cop0::kPageMask);
    for (unsigned pair = 0; pair < 6; ++pair) {
        if (written & (std::uint64_t{1} << (14 + 2 * pair))) mask |= std::uint64_t{3} << (13 + 2 * pair);
    }
    e.mask = mask | 0x1FFF;
    e.entry_hi = cop0.read(Cop0::kEntryHi) & (0xC00000FFFFFFE0FFULL & ~(e.mask & ~std::uint64_t{0x1FFF}));
    const std::uint64_t lo0 = cop0.read(Cop0::kEntryLo0);
    const std::uint64_t lo1 = cop0.read(Cop0::kEntryLo1);
    e.global = static_cast<std::uint8_t>(lo0 & lo1 & 1);
    // An entry keeps 26 bits of EntryLo: PFN (20), C, D, V (n64-systemtest).
    e.lo[0] = lo0 & 0x03FFFFFE;
    e.lo[1] = lo1 & 0x03FFFFFE;
}

void Tlb::read(int index, Cop0& cop0) const {
    const Entry& e = entries_[static_cast<std::size_t>(index & 31)];
    cop0.raw(Cop0::kPageMask) = e.mask & 0x01FFE000;
    cop0.raw(Cop0::kEntryHi) = e.entry_hi;
    cop0.raw(Cop0::kEntryLo0) = e.lo[0] | e.global;
    cop0.raw(Cop0::kEntryLo1) = e.lo[1] | e.global;
}

int Tlb::probe(const Cop0& cop0) const {
    const std::uint64_t entry_hi = cop0.read(Cop0::kEntryHi);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (matches(entries_[i], entry_hi, static_cast<std::uint8_t>(entry_hi))) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace ezy::n64
