#pragma once

// A tiny MIPS encoder for unit tests: enough to write short programs by hand.

#include <cstdint>
#include <initializer_list>

#include "flat_bus.hpp"
#include "n64/cpu/vr4300.hpp"

namespace mips {

constexpr std::uint32_t r(int op, int rs, int rt, int rd, int sa, int funct) {
    return (std::uint32_t(op) << 26) | (std::uint32_t(rs) << 21) | (std::uint32_t(rt) << 16) | (std::uint32_t(rd) << 11) |
           (std::uint32_t(sa) << 6) | std::uint32_t(funct);
}
constexpr std::uint32_t i(int op, int rs, int rt, std::int32_t imm) {
    return (std::uint32_t(op) << 26) | (std::uint32_t(rs) << 21) | (std::uint32_t(rt) << 16) | (std::uint32_t(imm) & 0xFFFF);
}
constexpr std::uint32_t j(int op, std::uint32_t target) { return (std::uint32_t(op) << 26) | ((target >> 2) & 0x3FFFFFF); }

// SPECIAL (op 0) by funct
constexpr std::uint32_t special(int rs, int rt, int rd, int sa, int funct) { return r(0, rs, rt, rd, sa, funct); }
constexpr std::uint32_t sll(int rd, int rt, int sa) { return special(0, rt, rd, sa, 0x00); }
constexpr std::uint32_t addu(int rd, int rs, int rt) { return special(rs, rt, rd, 0, 0x21); }
constexpr std::uint32_t add(int rd, int rs, int rt) { return special(rs, rt, rd, 0, 0x20); }
constexpr std::uint32_t dadd(int rd, int rs, int rt) { return special(rs, rt, rd, 0, 0x2C); }
constexpr std::uint32_t dsll32(int rd, int rt, int sa) { return special(0, rt, rd, sa, 0x3C); }
constexpr std::uint32_t jr(int rs) { return special(rs, 0, 0, 0, 0x08); }
constexpr std::uint32_t mult(int rs, int rt) { return special(rs, rt, 0, 0, 0x18); }
constexpr std::uint32_t div(int rs, int rt) { return special(rs, rt, 0, 0, 0x1A); }
constexpr std::uint32_t divu(int rs, int rt) { return special(rs, rt, 0, 0, 0x1B); }
constexpr std::uint32_t mfhi(int rd) { return special(0, 0, rd, 0, 0x10); }
constexpr std::uint32_t mflo(int rd) { return special(0, 0, rd, 0, 0x12); }
constexpr std::uint32_t teq(int rs, int rt) { return special(rs, rt, 0, 0, 0x34); }
constexpr std::uint32_t syscall() { return special(0, 0, 0, 0, 0x0C); }
constexpr std::uint32_t nop() { return 0; }

constexpr std::uint32_t addiu(int rt, int rs, std::int32_t imm) { return i(0x09, rs, rt, imm); }
constexpr std::uint32_t addi(int rt, int rs, std::int32_t imm) { return i(0x08, rs, rt, imm); }
constexpr std::uint32_t ori(int rt, int rs, std::int32_t imm) { return i(0x0D, rs, rt, imm); }
constexpr std::uint32_t lui(int rt, std::int32_t imm) { return i(0x0F, 0, rt, imm); }
constexpr std::uint32_t beq(int rs, int rt, std::int32_t words) { return i(0x04, rs, rt, words); }
constexpr std::uint32_t bne(int rs, int rt, std::int32_t words) { return i(0x05, rs, rt, words); }
constexpr std::uint32_t beql(int rs, int rt, std::int32_t words) { return i(0x14, rs, rt, words); }
constexpr std::uint32_t lw(int rt, int base, std::int32_t off) { return i(0x23, base, rt, off); }
constexpr std::uint32_t lwl(int rt, int base, std::int32_t off) { return i(0x22, base, rt, off); }
constexpr std::uint32_t lwr(int rt, int base, std::int32_t off) { return i(0x26, base, rt, off); }
constexpr std::uint32_t sw(int rt, int base, std::int32_t off) { return i(0x2B, base, rt, off); }
constexpr std::uint32_t sdl(int rt, int base, std::int32_t off) { return i(0x2C, base, rt, off); }
constexpr std::uint32_t sdr(int rt, int base, std::int32_t off) { return i(0x2D, base, rt, off); }
constexpr std::uint32_t jal(std::uint32_t target) { return j(0x03, target); }

// COP0
constexpr std::uint32_t mtc0(int rt, int rd) { return r(0x10, 0x04, rt, rd, 0, 0); }
constexpr std::uint32_t mfc0(int rt, int rd) { return r(0x10, 0x00, rt, rd, 0, 0); }
constexpr std::uint32_t eret() { return r(0x10, 0x10, 0, 0, 0, 0x18); }

}  // namespace mips

// A CPU on a flat bus with a program at physical 0x1000, entered through kseg0.
struct CpuFixture {
    static constexpr std::uint32_t kProgram = 0x1000;
    static constexpr std::uint64_t kEntry = 0xFFFFFFFF80000000ULL + kProgram;
    FlatBus bus;
    ezy::n64::Vr4300 cpu{bus};

    void load(std::initializer_list<std::uint32_t> words, std::uint32_t at = kProgram) {
        for (const std::uint32_t w : words) {
            bus.write32(at, w);
            at += 4;
        }
        cpu.set_pc(kEntry);
    }
    int run(int instructions) {
        int cycles = 0;
        for (int n = 0; n < instructions; ++n) cycles += cpu.step();
        return cycles;
    }
};
