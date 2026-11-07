#pragma once

#include <cstdint>

namespace ezy::n64 {

// MI_INTERRUPT bit numbers.
enum class MiInterrupt : std::uint8_t { Sp = 0, Si = 1, Ai = 2, Vi = 3, Pi = 4, Dp = 5 };

// MIPS Interface: gathers the six RCP interrupts into the one line wired to the CPU's
// Cause.IP2 (pending & mask). Registers at 0x04300000 (n64brew "MIPS Interface").
class Mi {
public:
    void raise(MiInterrupt i) { intr_ |= bit(i); }
    void clear(MiInterrupt i) { intr_ &= ~bit(i); }
    [[nodiscard]] bool line() const { return (intr_ & mask_) != 0; }

    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const {
        switch (offset & 0xF) {
            case 0x0: return mode_;
            case 0x4: return 0x02020102;  // MI_VERSION: RSP, RDP, RAC, IO versions
            case 0x8: return intr_;
            default: return mask_;
        }
    }

    void write(std::uint32_t offset, std::uint32_t value) {
        switch (offset & 0xF) {
            case 0x0:  // MI_MODE: init length, then set/clear pairs for init, ebus, RDRAM reg mode
                mode_ = (mode_ & ~0x7FU) | (value & 0x7F);
                if (value & (1U << 7)) mode_ &= ~(1U << 7);
                if (value & (1U << 8)) mode_ |= 1U << 7;
                if (value & (1U << 9)) mode_ &= ~(1U << 8);
                if (value & (1U << 10)) mode_ |= 1U << 8;
                if (value & (1U << 11)) clear(MiInterrupt::Dp);
                if (value & (1U << 12)) mode_ &= ~(1U << 9);
                if (value & (1U << 13)) mode_ |= 1U << 9;
                break;
            case 0xC:  // MI_MASK: bit 2n clears mask n, bit 2n+1 sets it
                for (unsigned i = 0; i < 6; ++i) {
                    if (value & (1U << (2 * i))) mask_ &= ~(1U << i);
                    if (value & (1U << (2 * i + 1))) mask_ |= 1U << i;
                }
                break;
            default: break;
        }
    }

    template <class Ar>
    void serialize(Ar& ar) {
        ar(mode_), ar(intr_), ar(mask_);
    }

private:
    static constexpr std::uint32_t bit(MiInterrupt i) { return 1U << static_cast<unsigned>(i); }

    std::uint32_t mode_ = 0;
    std::uint32_t intr_ = 0;
    std::uint32_t mask_ = 0;
};

}  // namespace ezy::n64
