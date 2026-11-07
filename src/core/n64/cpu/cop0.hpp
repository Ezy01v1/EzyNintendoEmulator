#pragma once

#include <array>
#include <cstdint>

namespace ezy::n64 {

// Cause.ExcCode values (VR4300 manual, "Exception Processing").
enum class ExceptionCode : std::uint8_t {
    Int = 0,
    Mod = 1,
    TlbL = 2,
    TlbS = 3,
    AdEL = 4,
    AdES = 5,
    Sys = 8,
    Bp = 9,
    RI = 10,
    CpU = 11,
    Ov = 12,
    Tr = 13,
    FPE = 15,
    Watch = 23,
};

// System control coprocessor registers. read()/write() are what MFC0/MTC0 (and their
// 64-bit forms) see: each register has its own write mask, some are read-only, and the
// unused numbers read back the last value written to any COP0 register (the internal
// bus latches it).
class Cop0 {
public:
    static constexpr int kIndex = 0, kRandom = 1, kEntryLo0 = 2, kEntryLo1 = 3, kContext = 4, kPageMask = 5,
                         kWired = 6, kBadVAddr = 8, kCount = 9, kEntryHi = 10, kCompare = 11, kStatus = 12,
                         kCause = 13, kEpc = 14, kPrId = 15, kConfig = 16, kLLAddr = 17, kWatchLo = 18,
                         kWatchHi = 19, kXContext = 20, kParityError = 26, kCacheError = 27, kTagLo = 28,
                         kTagHi = 29, kErrorEpc = 30;

    // Status bits.
    static constexpr std::uint32_t kIE = 1U << 0, kEXL = 1U << 1, kERL = 1U << 2, kUX = 1U << 5, kSX = 1U << 6,
                                   kKX = 1U << 7, kBEV = 1U << 22, kFR = 1U << 26, kCU0 = 1U << 28,
                                   kCU1 = 1U << 29;
    // Cause bits.
    static constexpr std::uint32_t kIP2 = 1U << 10, kIP7 = 1U << 15, kBD = 1U << 31;

    Cop0();

    [[nodiscard]] std::uint64_t read(int reg) const;
    void write(int reg, std::uint64_t value);

    // Direct access for the CPU (exceptions, TLB), without the MTC0 masks.
    [[nodiscard]] std::uint64_t& raw(int reg) { return regs_[static_cast<std::size_t>(reg)]; }
    [[nodiscard]] std::uint32_t status() const { return static_cast<std::uint32_t>(regs_[kStatus]); }
    [[nodiscard]] std::uint32_t cause() const { return static_cast<std::uint32_t>(regs_[kCause]); }

    // Count advances once every 2 CPU cycles; reaching Compare sets Cause.IP7.
    void advance(int cycles);
    // One instruction retired: Random counts down from 31 to Wired, then wraps.
    void tick_random();
    void set_ip2(bool level);

    template <class Ar>
    void serialize(Ar& ar) {
        ar(regs_), ar(half_cycles_), ar(latch_);
    }

private:
    std::array<std::uint64_t, 32> regs_{};
    std::uint64_t half_cycles_ = 0;  // Count * 2 + the odd cycle
    std::uint64_t latch_ = 0;
};

}  // namespace ezy::n64
