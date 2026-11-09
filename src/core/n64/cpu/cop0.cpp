#include "n64/cpu/cop0.hpp"

namespace ezy::n64 {

namespace {

// Writable bits per register for MTC0/DMTC0. Read-only registers have mask 0.
constexpr std::array<std::uint64_t, 32> kWriteMask = [] {
    std::array<std::uint64_t, 32> m{};
    m[Cop0::kIndex] = 0x8000003F;
    m[Cop0::kEntryLo0] = 0x3FFFFFFF;
    m[Cop0::kEntryLo1] = 0x3FFFFFFF;
    m[Cop0::kContext] = 0xFFFFFFFFFF800000ULL;  // PTEBase; BadVPN2 is set by exceptions
    m[Cop0::kPageMask] = 0x01FFE000;
    m[Cop0::kWired] = 0x3F;
    m[Cop0::kBadVAddr] = 0;
    m[Cop0::kCount] = 0xFFFFFFFF;
    m[Cop0::kEntryHi] = 0xC00000FFFFFFE0FFULL;  // R, VPN2, ASID
    m[Cop0::kCompare] = 0xFFFFFFFF;
    m[Cop0::kStatus] = 0xFF57FFFF;
    m[Cop0::kCause] = 0x300;  // only the software interrupts IP0/IP1
    m[Cop0::kEpc] = ~std::uint64_t{0};
    m[Cop0::kPrId] = 0;
    m[Cop0::kConfig] = 0x0F00800F;
    m[Cop0::kLLAddr] = 0xFFFFFFFF;
    m[Cop0::kWatchLo] = 0xFFFFFFFB;
    m[Cop0::kWatchHi] = 0xF;
    m[Cop0::kXContext] = 0xFFFFFFFE00000000ULL;
    m[Cop0::kParityError] = 0xFF;
    m[Cop0::kCacheError] = 0;
    m[Cop0::kTagLo] = 0x0FFFFFC0;
    m[Cop0::kTagHi] = 0;
    m[Cop0::kErrorEpc] = ~std::uint64_t{0};
    return m;
}();

constexpr bool is_unused(int reg) { return reg == 7 || (reg >= 21 && reg <= 25) || reg == 31; }

}  // namespace

// Cold reset values (VR4300 manual, "Reset Exception"): BEV and ERL set.
Cop0::Cop0() {
    regs_[kRandom] = 31;
    regs_[kStatus] = kBEV | kERL;
    regs_[kPrId] = 0x00000B22;
    regs_[kConfig] = 0x7006E463;
}

std::uint64_t Cop0::read(int reg) const {
    if (is_unused(reg)) return latch_;
    if (reg == kCount) return (half_cycles_ >> 1) & 0xFFFFFFFF;
    return regs_[static_cast<std::size_t>(reg)];
}

void Cop0::write(int reg, std::uint64_t value) {
    latch_ = value;
    if (is_unused(reg)) return;
    const auto i = static_cast<std::size_t>(reg);
    switch (reg) {
        case kCount:
            half_cycles_ = (value & 0xFFFFFFFF) << 1;
            return;
        case kCompare:
            regs_[i] = value & 0xFFFFFFFF;
            regs_[kCause] &= ~std::uint64_t{kIP7};
            return;
        case kWired:
            regs_[i] = value & 0x3F;
            regs_[kRandom] = 31;
            return;
        default:
            regs_[i] = (regs_[i] & ~kWriteMask[i]) | (value & kWriteMask[i]);
            return;
    }
}

void Cop0::advance(int cycles) {
    const std::uint32_t before = static_cast<std::uint32_t>(half_cycles_ >> 1);
    half_cycles_ += static_cast<std::uint64_t>(cycles);
    const std::uint32_t after = static_cast<std::uint32_t>(half_cycles_ >> 1);
    // Did Count step onto Compare? (Wrap-safe: Compare lies in (before, after].)
    const std::uint32_t compare = static_cast<std::uint32_t>(regs_[kCompare]);
    if (after != before && static_cast<std::uint32_t>(compare - before - 1) < static_cast<std::uint32_t>(after - before)) {
        regs_[kCause] |= kIP7;
    }
}

// Random counts down and goes back to 31 when it reaches Wired. It is a 6-bit counter, so
// with Wired above 31 it runs through 0..63 (n64-systemtest "Wired OOB/Random").
void Cop0::tick_random() {
    const std::uint64_t wired = regs_[kWired] & 63;
    std::uint64_t& random = regs_[kRandom];
    random = random == wired ? 31 : (random - 1) & 63;
}

void Cop0::retire_many(std::uint64_t cycles, std::uint64_t instructions) {
    half_cycles_ += cycles;
    // Random walks a fixed cycle: 31 down to Wired (Wired <= 31), or 31 down to 0, then 63
    // down to Wired (Wired > 31). Step one at a time until it is on the cycle, then jump.
    const std::uint64_t wired = regs_[kWired] & 63;
    const std::uint64_t length = wired <= 31 ? 32 - wired : 96 - wired;
    const auto on_cycle = [&](std::uint64_t r) { return wired <= 31 ? r >= wired && r <= 31 : r <= 31 || r >= wired; };
    while (instructions > 0 && !on_cycle(regs_[kRandom])) {
        tick_random();
        --instructions;
    }
    const std::uint64_t r = regs_[kRandom];
    const std::uint64_t index = ((r <= 31 ? 31 - r : 95 - r) + instructions) % length;
    regs_[kRandom] = index <= 31 ? 31 - index : 95 - index;
}

}  // namespace ezy::n64
