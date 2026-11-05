// BIOS calls emulated in C++ (HLE), used together with our own minimal BIOS (hle_bios.hpp).
// Behaviour follows GBATEK "BIOS Functions". Unlisted calls fall through to the BIOS
// vector, which returns without doing anything.

#include <cstdint>
#include <limits>

#include "gba/gba.hpp"
#include "gba/hle_bios.hpp"

namespace ezy::gba {

namespace {

std::uint32_t isqrt(std::uint32_t n) {
    std::uint32_t root = 0;
    std::uint32_t bit = 1U << 30;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

}  // namespace

bool Gba::on_swi(std::uint8_t number) {
    switch (number) {
        case 0x02:  // Halt
        case 0x03:  // Stop (treated as Halt)
            bus_.set_halted(true);
            return true;
        case 0x04: return intr_wait(cpu_.reg(0) != 0, static_cast<std::uint16_t>(cpu_.reg(1)));
        case 0x05:  // VBlankIntrWait = IntrWait(discard old, VBlank)
            cpu_.set_reg(0, 1);
            cpu_.set_reg(1, 1);
            return intr_wait(true, 1);
        case 0x06: swi_div(cpu_.reg(0), cpu_.reg(1)); return true;  // Div: r0 / r1
        case 0x07: swi_div(cpu_.reg(1), cpu_.reg(0)); return true;  // DivArm: r1 / r0
        case 0x08: cpu_.set_reg(0, isqrt(cpu_.reg(0))); return true;  // Sqrt
        case 0x0B: swi_cpu_set(false); return true;
        case 0x0C: swi_cpu_set(true); return true;
        default: return false;
    }
}

// IntrWait: halt until one of `flags` is set in BIOS_IF (0x03007FF8). The game's own IRQ
// handler sets those bits; the BIOS only checks and clears them.
//
// While waiting, the SWI is re-executed after every interrupt: we halt and point the PC
// back at the SWI, so the IRQ returns to it. `intr_waiting_` makes the re-executions skip
// the "discard old flags" step, which only happens on the first call.
bool Gba::intr_wait(bool discard, std::uint16_t flags) {
    bus_.write16(0x04000208, 1);  // IntrWait enables IME
    auto bios_if = bus_.read16(kBiosIfAddr);
    if (discard && !intr_waiting_) bios_if = static_cast<std::uint16_t>(bios_if & ~flags);
    if ((bios_if & flags) != 0) {
        bus_.write16(kBiosIfAddr, static_cast<std::uint16_t>(bios_if & ~flags));
        intr_waiting_ = false;
        return true;
    }
    bus_.write16(kBiosIfAddr, bios_if);
    intr_waiting_ = true;
    bus_.set_halted(true);
    // During execution r15 is the SWI's address + 8 (ARM) or + 4 (Thumb).
    cpu_.jump(cpu_.reg(15) - (cpu_.thumb() ? 4U : 8U));
    return true;
}

// Div / DivArm: r0 = quotient, r1 = remainder, r3 = |quotient|.
void Gba::swi_div(std::uint32_t numerator, std::uint32_t denominator) {
    const auto num = static_cast<std::int32_t>(numerator);
    const auto den = static_cast<std::int32_t>(denominator);
    std::int32_t quotient = 0;
    std::int32_t remainder = num;
    if (den == 0) {
        quotient = num < 0 ? -1 : 1;  // the real BIOS loops forever; avoid UB instead
    } else if (num == std::numeric_limits<std::int32_t>::min() && den == -1) {
        quotient = num;
        remainder = 0;
    } else {
        quotient = num / den;
        remainder = num % den;
    }
    cpu_.set_reg(0, static_cast<std::uint32_t>(quotient));
    cpu_.set_reg(1, static_cast<std::uint32_t>(remainder));
    cpu_.set_reg(3, quotient < 0 ? 0U - static_cast<std::uint32_t>(quotient) : static_cast<std::uint32_t>(quotient));
}

// CpuSet (r0 = source, r1 = destination, r2 = count | flags) and CpuFastSet.
// Bit 24: fill (the source stays fixed). Bit 26 (CpuSet only): 32-bit units.
// CpuFastSet always copies words, in blocks of 8.
void Gba::swi_cpu_set(bool fast) {
    std::uint32_t src = cpu_.reg(0);
    std::uint32_t dst = cpu_.reg(1);
    const std::uint32_t control = cpu_.reg(2);
    const bool fill = (control & (1U << 24)) != 0;
    const bool words = fast || (control & (1U << 26)) != 0;
    std::uint32_t count = control & 0x1FFFFF;
    if (fast) count = (count + 7) & ~7U;

    const std::uint32_t step = words ? 4 : 2;
    src &= ~(step - 1);
    dst &= ~(step - 1);
    Access access = kNonseq;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (words) {
            bus_.write32(dst, bus_.read32(src, access), access);
        } else {
            bus_.write16(dst, bus_.read16(src, access), access);
        }
        if (!fill) src += step;
        dst += step;
        access = kSeq;
    }
}

}  // namespace ezy::gba
