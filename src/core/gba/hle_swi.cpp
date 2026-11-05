// BIOS calls emulated in C++ (HLE), used together with our own minimal BIOS (hle_bios.hpp).
// Behaviour follows GBATEK "BIOS Functions". Unlisted calls fall through to the BIOS
// vector, which returns without doing anything.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "gba/gba.hpp"
#include "gba/hle_bios.hpp"

namespace ezy::gba {

namespace {

// --- Math ---

// ArcTan of a 1.14 fixed-point tangent, as the BIOS computes it (a polynomial in tan^2):
// the result is an angle where 0x4000 is 90 degrees.
std::int32_t arctan(std::int32_t tan) {
    const std::int32_t a = -((tan * tan) >> 14);
    std::int32_t b = ((0xA9 * a) >> 14) + 0x390;
    for (const std::int32_t term : {0x91C, 0xFB6, 0x16AA, 0x2081, 0x3651, 0xA2F9}) b = ((b * a) >> 14) + term;
    return (tan * b) >> 16;
}

// ArcTan2(x, y): the full-circle angle 0x0000-0xFFFF of the vector (x, y).
std::uint16_t arctan2(std::int32_t x, std::int32_t y) {
    std::int32_t angle = 0;
    if (y == 0) {
        angle = x >= 0 ? 0 : 0x8000;
    } else if (x == 0) {
        angle = y >= 0 ? 0x4000 : 0xC000;
    } else if (y >= 0) {
        if (x >= 0 && x >= y) {
            angle = arctan((y << 14) / x);
        } else if (x < 0 && -x >= y) {
            angle = arctan((y << 14) / x) + 0x8000;
        } else {
            angle = 0x4000 - arctan((x << 14) / y);
        }
    } else {
        if (x <= 0 && -x > -y) {
            angle = arctan((y << 14) / x) + 0x8000;
        } else if (x > 0 && x >= -y) {
            angle = arctan((y << 14) / x) + 0x10000;
        } else {
            angle = 0xC000 - arctan((x << 14) / y);
        }
    }
    return static_cast<std::uint16_t>(angle);
}

// sin(i * 360/256 degrees) in 1.14 fixed point, like the BIOS's table.
const std::array<std::int32_t, 256>& sine_table() {
    static const std::array<std::int32_t, 256> table = [] {
        std::array<std::int32_t, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            t[i] = static_cast<std::int32_t>(std::lround(std::sin(static_cast<double>(i) * 2.0 * std::numbers::pi / 256.0) * 16384.0));
        }
        return t;
    }();
    return table;
}

// The rotation/scaling matrix BgAffineSet and ObjAffineSet compute: scales are 8.8, the
// angle's upper byte is used (256 steps per turn), the result is PA-PD in 8.8.
struct Matrix {
    std::int32_t pa, pb, pc, pd;
};
Matrix affine_matrix(std::int32_t scale_x, std::int32_t scale_y, std::uint16_t angle) {
    const auto& sine = sine_table();
    const std::int32_t sin = sine[angle >> 8];
    const std::int32_t cos = sine[((angle >> 8) + 64) & 0xFF];
    return {(scale_x * cos) >> 14, -((scale_x * sin) >> 14), (scale_y * sin) >> 14, (scale_y * cos) >> 14};
}

// --- Decompression (GBATEK "BIOS Decompression Functions") ---
//
// Each format starts with a header word: bits 4-7 the type, bits 8-31 the decompressed
// size. The "Wram" variants write bytes; the "Vram" ones write halfwords, because a byte
// write to VRAM lands in both halves of the halfword. We decompress into a buffer (so
// back-references never read half-written VRAM), then store it.

std::uint32_t decompressed_size(Bus& bus, std::uint32_t src) { return bus.read32(src) >> 8; }

void store(Bus& bus, std::uint32_t dst, const std::vector<std::uint8_t>& data, bool halfwords) {
    if (!halfwords) {
        for (std::size_t i = 0; i < data.size(); ++i) bus.write8(dst + static_cast<std::uint32_t>(i), data[i]);
        return;
    }
    for (std::size_t i = 0; i < data.size(); i += 2) {
        const std::uint8_t high = i + 1 < data.size() ? data[i + 1] : 0;
        bus.write16(dst + static_cast<std::uint32_t>(i), static_cast<std::uint16_t>(data[i] | (high << 8)));
    }
}

// LZ77: a flag byte (MSB first) per 8 blocks; 0 = a literal byte, 1 = a back-reference of
// 3-18 bytes from 1-4096 bytes back.
std::vector<std::uint8_t> lz77(Bus& bus, std::uint32_t src) {
    const std::uint32_t size = decompressed_size(bus, src);
    std::vector<std::uint8_t> out;
    out.reserve(size);
    src += 4;
    while (out.size() < size) {
        const std::uint8_t flags = bus.read8(src++);
        for (int block = 7; block >= 0 && out.size() < size; --block) {
            if (((flags >> block) & 1) == 0) {
                out.push_back(bus.read8(src++));
                continue;
            }
            const std::uint8_t b0 = bus.read8(src++);
            const std::uint8_t b1 = bus.read8(src++);
            const std::size_t distance = (static_cast<std::size_t>(b0 & 0xF) << 8 | b1) + 1;
            const int length = (b0 >> 4) + 3;
            for (int k = 0; k < length && out.size() < size; ++k) {
                out.push_back(distance <= out.size() ? out[out.size() - distance] : 0);
            }
        }
    }
    return out;
}

// RLE: a flag byte per run; bit 7 set = the next byte repeated 3-130 times, clear = 1-128
// literal bytes.
std::vector<std::uint8_t> run_length(Bus& bus, std::uint32_t src) {
    const std::uint32_t size = decompressed_size(bus, src);
    std::vector<std::uint8_t> out;
    out.reserve(size);
    src += 4;
    while (out.size() < size) {
        const std::uint8_t flag = bus.read8(src++);
        if ((flag & 0x80) != 0) {
            const std::uint8_t value = bus.read8(src++);
            for (int k = 0; k < (flag & 0x7F) + 3 && out.size() < size; ++k) out.push_back(value);
        } else {
            for (int k = 0; k < (flag & 0x7F) + 1 && out.size() < size; ++k) out.push_back(bus.read8(src++));
        }
    }
    return out;
}

// Huffman: a tree of byte nodes after the header, then the bit stream in 32-bit words, MSB
// first. A node holds the offset to its two children (bits 0-5) and whether the left (bit 7)
// or right (bit 6) child is a leaf, i.e. a 4- or 8-bit symbol. Output is written in words.
void huffman(Bus& bus, std::uint32_t src, std::uint32_t dst) {
    const std::uint32_t header = bus.read32(src);
    const std::uint32_t bits = (header & 0xF) == 4 ? 4 : 8;
    const std::uint32_t size = header >> 8;
    const std::uint32_t tree = src + 4;
    const std::uint32_t root = tree + 1;
    std::uint32_t stream = tree + (bus.read8(tree) + 1U) * 2;

    std::uint32_t node_addr = root;
    std::uint8_t node = bus.read8(root);
    std::uint32_t acc = 0;
    std::uint32_t acc_bits = 0;
    std::uint32_t written = 0;
    while (written < size) {
        const std::uint32_t word = bus.read32(stream);
        stream += 4;
        for (int b = 31; b >= 0 && written < size; --b) {
            const bool right = ((word >> b) & 1) != 0;
            const std::uint32_t child = (node_addr & ~1U) + (node & 0x3FU) * 2 + 2 + (right ? 1 : 0);
            const bool leaf = (node & (right ? 0x40 : 0x80)) != 0;
            if (!leaf) {
                node_addr = child;
                node = bus.read8(child);
                continue;
            }
            acc |= (bus.read8(child) & ((1U << bits) - 1)) << acc_bits;
            acc_bits += bits;
            node_addr = root;
            node = bus.read8(root);
            if (acc_bits == 32) {
                bus.write32(dst + written, acc);
                written += 4;
                acc = 0;
                acc_bits = 0;
            }
        }
    }
}

// BitUnPack: widens 1/2/4/8-bit units into 1/2/4/8/16/32-bit ones, adding an offset to
// non-zero units (or to all, with bit 31 of the offset).
void bit_unpack(Bus& bus, std::uint32_t src, std::uint32_t dst, std::uint32_t info) {
    const std::uint32_t length = bus.read16(info);
    const std::uint32_t src_width = bus.read8(info + 2);
    const std::uint32_t dst_width = bus.read8(info + 3);
    const std::uint32_t offset_word = bus.read32(info + 4);
    const std::uint32_t offset = offset_word & 0x7FFFFFFF;
    const bool offset_zero = (offset_word >> 31) != 0;
    if (src_width == 0 || src_width > 8 || dst_width == 0 || dst_width > 32) return;
    const std::uint32_t src_mask = (1U << src_width) - 1;
    const std::uint32_t dst_mask = dst_width == 32 ? 0xFFFFFFFF : (1U << dst_width) - 1;

    std::uint32_t out = 0;
    std::uint32_t out_bits = 0;
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint8_t byte = bus.read8(src + i);
        for (std::uint32_t s = 0; s < 8; s += src_width) {
            std::uint32_t unit = (byte >> s) & src_mask;
            if (unit != 0 || offset_zero) unit += offset;
            out |= (unit & dst_mask) << out_bits;
            out_bits += dst_width;
            if (out_bits >= 32) {
                bus.write32(dst, out);
                dst += 4;
                out = 0;
                out_bits = 0;
            }
        }
    }
}

// Diff filters: each unit is the previous one plus a delta.
void diff_unfilter(Bus& bus, std::uint32_t src, std::uint32_t dst, bool sixteen, bool halfword_writes) {
    const std::uint32_t size = decompressed_size(bus, src);
    src += 4;
    if (sixteen) {
        std::uint16_t value = 0;
        for (std::uint32_t i = 0; i < size; i += 2) {
            value = static_cast<std::uint16_t>(value + bus.read16(src + i));
            bus.write16(dst + i, value);
        }
        return;
    }
    std::vector<std::uint8_t> out;
    out.reserve(size);
    std::uint8_t value = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        value = static_cast<std::uint8_t>(value + bus.read8(src + i));
        out.push_back(value);
    }
    store(bus, dst, out, halfword_writes);
}

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
    const std::uint32_t r0 = cpu_.reg(0);
    const std::uint32_t r1 = cpu_.reg(1);
    const std::uint32_t r2 = cpu_.reg(2);
    const std::uint32_t r3 = cpu_.reg(3);
    switch (number) {
        case 0x00: swi_soft_reset(); return true;
        case 0x01: swi_register_ram_reset(r0); return true;
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
        case 0x09:  // ArcTan: r0 = tan (1.14)
            cpu_.set_reg(0, static_cast<std::uint32_t>(arctan(static_cast<std::int16_t>(r0))));
            return true;
        case 0x0A:  // ArcTan2: r0 = x, r1 = y (signed 16-bit)
            cpu_.set_reg(0, arctan2(static_cast<std::int16_t>(r0), static_cast<std::int16_t>(r1)));
            return true;
        case 0x0B: swi_cpu_set(false); return true;
        case 0x0C: swi_cpu_set(true); return true;
        case 0x0D: cpu_.set_reg(0, 0xBAAE187F); return true;  // GetBiosChecksum (GBA BIOS)
        case 0x0E:  // BgAffineSet: r0 = 20-byte sources, r1 = 16-byte results, r2 = count
            for (std::uint32_t i = 0; i < r2; ++i) {
                const std::uint32_t s = r0 + i * 20;
                const std::uint32_t d = r1 + i * 16;
                const auto origin_x = static_cast<std::int32_t>(bus_.read32(s));  // texture, 8.8
                const auto origin_y = static_cast<std::int32_t>(bus_.read32(s + 4));
                const std::int32_t screen_x = static_cast<std::int16_t>(bus_.read16(s + 8));
                const std::int32_t screen_y = static_cast<std::int16_t>(bus_.read16(s + 10));
                const Matrix m = affine_matrix(static_cast<std::int16_t>(bus_.read16(s + 12)),
                                               static_cast<std::int16_t>(bus_.read16(s + 14)), bus_.read16(s + 16));
                bus_.write16(d, static_cast<std::uint16_t>(m.pa));
                bus_.write16(d + 2, static_cast<std::uint16_t>(m.pb));
                bus_.write16(d + 4, static_cast<std::uint16_t>(m.pc));
                bus_.write16(d + 6, static_cast<std::uint16_t>(m.pd));
                // Reference point: the texture origin minus where the screen centre maps to.
                bus_.write32(d + 8, static_cast<std::uint32_t>(origin_x - (m.pa * screen_x + m.pb * screen_y)));
                bus_.write32(d + 12, static_cast<std::uint32_t>(origin_y - (m.pc * screen_x + m.pd * screen_y)));
            }
            return true;
        case 0x0F:  // ObjAffineSet: r0 = 8-byte sources, r1 = dest, r2 = count, r3 = stride
            for (std::uint32_t i = 0; i < r2; ++i) {
                const std::uint32_t s = r0 + i * 8;
                const Matrix m = affine_matrix(static_cast<std::int16_t>(bus_.read16(s)),
                                               static_cast<std::int16_t>(bus_.read16(s + 2)), bus_.read16(s + 4));
                const std::uint32_t d = r1 + i * 4 * r3;
                bus_.write16(d, static_cast<std::uint16_t>(m.pa));
                bus_.write16(d + r3, static_cast<std::uint16_t>(m.pb));
                bus_.write16(d + 2 * r3, static_cast<std::uint16_t>(m.pc));
                bus_.write16(d + 3 * r3, static_cast<std::uint16_t>(m.pd));
            }
            return true;
        case 0x10: bit_unpack(bus_, r0, r1, r2); return true;
        case 0x11: store(bus_, r1, lz77(bus_, r0), false); return true;        // LZ77UnCompWram
        case 0x12: store(bus_, r1, lz77(bus_, r0), true); return true;         // LZ77UnCompVram
        case 0x13: huffman(bus_, r0, r1); return true;                         // HuffUnComp
        case 0x14: store(bus_, r1, run_length(bus_, r0), false); return true;  // RLUnCompWram
        case 0x15: store(bus_, r1, run_length(bus_, r0), true); return true;   // RLUnCompVram
        case 0x16: diff_unfilter(bus_, r0, r1, false, false); return true;     // Diff8bitUnFilterWram
        case 0x17: diff_unfilter(bus_, r0, r1, false, true); return true;      // Diff8bitUnFilterVram
        case 0x18: diff_unfilter(bus_, r0, r1, true, true); return true;       // Diff16bitUnFilter
        case 0x19: {  // SoundBias: r0 = 0 lowers the bias to 0, otherwise raises it to 0x200
            const std::uint16_t bias = bus_.read16(0x04000088);
            bus_.write16(0x04000088, static_cast<std::uint16_t>((bias & ~0x3FEU) | (r0 != 0 ? 0x200U : 0U)));
            return true;
        }
        case 0x1F: {  // MidiKey2Freq: r0 = WaveData*, r1 = MIDI key, r2 = fine adjust (1/256 key)
            const double frequency = bus_.read32(r0 + 4);
            const double keys = 180.0 - static_cast<double>(r1 & 0xFF) - static_cast<double>(r2 & 0xFF) / 256.0;
            cpu_.set_reg(0, static_cast<std::uint32_t>(frequency / std::pow(2.0, keys / 12.0)));
            return true;
        }
        case 0x25: cpu_.set_reg(0, 1); return true;   // MultiBoot: no link cable, report failure
        case 0x26: swi_soft_reset(); return true;     // HardReset: we skip the boot intro
        case 0x27:  // CustomHalt: r2 = the value for HALTCNT
            bus_.write8(0x04000301, static_cast<std::uint8_t>(r2));
            return true;
        default: return false;  // e.g. the BIOS sound driver (0x1A-0x1E, 0x20-0x24, 0x28-0x29)
    }
}

// SoftReset: clears the top 512 bytes of IWRAM (the BIOS's area), resets the registers and
// restarts the cartridge (or EWRAM, if the byte at 0x03007FFA is non-zero) in System mode.
void Gba::swi_soft_reset() {
    const bool to_ewram = bus_.read8(0x03007FFA) != 0;
    for (std::uint32_t a = 0x03007E00; a < 0x03008000; a += 4) bus_.write32(a, 0);
    cpu_.set_cpsr(static_cast<std::uint32_t>(Mode::System));  // switch banks properly first
    for (std::size_t r = 0; r <= 12; ++r) cpu_.set_reg(r, 0);
    cpu_.set_reg(14, 0);
    for (const Bank bank : {kBankSvc, kBankIrq}) {
        cpu_.set_banked(bank, 6, 0);
        cpu_.set_spsr(bank, 0);
    }
    intr_waiting_ = false;
    boot_without_bios(to_ewram ? 0x02000000 : 0x08000000);
}

// RegisterRamReset: r0 bits pick what to clear. 0 EWRAM, 1 IWRAM (but the top 512 bytes),
// 2 palette, 3 VRAM, 4 OAM, 5 serial registers, 6 sound registers, 7 the other registers.
// The display is always put in forced blank.
void Gba::swi_register_ram_reset(std::uint32_t flags) {
    const auto clear = [this](std::uint32_t from, std::uint32_t to) {
        for (std::uint32_t a = from; a < to; a += 4) bus_.write32(a, 0);
    };
    bus_.write16(0x04000000, 0x0080);
    if ((flags & 0x01) != 0) clear(0x02000000, 0x02040000);
    if ((flags & 0x02) != 0) clear(0x03000000, 0x03007E00);
    if ((flags & 0x04) != 0) clear(0x05000000, 0x05000400);
    if ((flags & 0x08) != 0) clear(0x06000000, 0x06018000);
    if ((flags & 0x10) != 0) clear(0x07000000, 0x07000400);
    if ((flags & 0x20) != 0) {
        clear(0x04000120, 0x04000130);
        bus_.write16(0x04000134, 0x8000);  // RCNT: general purpose mode
    }
    if ((flags & 0x40) != 0) {
        bus_.write16(0x04000084, 0);  // master off: clears and locks the PSG registers
        clear(0x04000080, 0x04000084);
        clear(0x04000090, 0x040000A0);  // wave RAM (the bank the CPU sees)
        bus_.write16(0x04000088, 0x0200);
    }
    if ((flags & 0x80) != 0) {
        for (std::uint32_t a = 0x04000004; a < 0x04000060; a += 2) bus_.write16(a, 0);
        for (const std::uint32_t a : {0x04000020U, 0x04000026U, 0x04000030U, 0x04000036U}) bus_.write16(a, 0x0100);
        for (std::uint32_t a = 0x040000B0; a < 0x040000E0; a += 2) bus_.write16(a, 0);  // DMA
        for (std::uint32_t a = 0x04000100; a < 0x04000110; a += 2) bus_.write16(a, 0);  // timers
        bus_.write16(0x04000200, 0);       // IE
        bus_.write16(0x04000202, 0xFFFF);  // IF: acknowledge everything
        bus_.write16(0x04000204, 0);       // WAITCNT
        bus_.write16(0x04000208, 0);       // IME
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
