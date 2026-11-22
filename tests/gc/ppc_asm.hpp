#pragma once

// A tiny PowerPC encoder for unit tests: enough to write short Gekko programs by hand.
// Operand order follows the assembler (rD/rA first, as in `addi r3,r4,1`); load/store
// helpers take (rD, offset, rA) like `lwz r3,8(r4)`. Names that are C++ keywords or
// alternative tokens get a trailing underscore (and_, or_, xor_...).

#include <cstdint>
#include <initializer_list>

#include "flat_bus.hpp"
#include "gc/gekko/gekko.hpp"

namespace ppc {

using u32 = std::uint32_t;

constexpr u32 field(int value, int shift) { return static_cast<u32>(value & 31) << shift; }

// D-form: opcode, rD/rS/TO/crfD<<2, rA, 16-bit immediate.
constexpr u32 d_form(int op, int rt, int ra, std::int32_t imm) {
    return (static_cast<u32>(op) << 26) | field(rt, 21) | field(ra, 16) | (static_cast<u32>(imm) & 0xFFFF);
}
// X-form / XL-form: opcode, three 5-bit fields, a 10-bit extended opcode, Rc/LK.
constexpr u32 x_form(int op, int rt, int ra, int rb, int xo, bool rc = false) {
    return (static_cast<u32>(op) << 26) | field(rt, 21) | field(ra, 16) | field(rb, 11) |
           (static_cast<u32>(xo & 0x3FF) << 1) | (rc ? 1u : 0u);
}
// XO-form (opcode 31 arithmetic): OE in bit 21, a 9-bit extended opcode.
constexpr u32 xo_form(int rt, int ra, int rb, int xo, bool oe, bool rc) {
    return x_form(31, rt, ra, rb, (oe ? 0x200 : 0) | (xo & 0x1FF), rc);
}
// M-form rotates: rS, rA, SH/rB, MB, ME.
constexpr u32 m_form(int op, int rs, int ra, int sh, int mb, int me, bool rc) {
    return (static_cast<u32>(op) << 26) | field(rs, 21) | field(ra, 16) | field(sh, 11) | field(mb, 6) |
           field(me, 1) | (rc ? 1u : 0u);
}
// The SPR/TBR number as encoded: its two 5-bit halves swapped.
constexpr u32 spr_field(int spr) {
    return (static_cast<u32>(spr & 0x1F) << 16) | (static_cast<u32>((spr >> 5) & 0x1F) << 11);
}

// Immediate arithmetic and logic
constexpr u32 twi(int to, int ra, std::int32_t imm) { return d_form(3, to, ra, imm); }
constexpr u32 mulli(int rt, int ra, std::int32_t imm) { return d_form(7, rt, ra, imm); }
constexpr u32 subfic(int rt, int ra, std::int32_t imm) { return d_form(8, rt, ra, imm); }
constexpr u32 cmplwi(int crf, int ra, std::int32_t imm) { return d_form(10, crf << 2, ra, imm); }
constexpr u32 cmpwi(int crf, int ra, std::int32_t imm) { return d_form(11, crf << 2, ra, imm); }
constexpr u32 addic(int rt, int ra, std::int32_t imm) { return d_form(12, rt, ra, imm); }
constexpr u32 addic_(int rt, int ra, std::int32_t imm) { return d_form(13, rt, ra, imm); }
constexpr u32 addi(int rt, int ra, std::int32_t imm) { return d_form(14, rt, ra, imm); }
constexpr u32 li(int rt, std::int32_t imm) { return addi(rt, 0, imm); }
constexpr u32 addis(int rt, int ra, std::int32_t imm) { return d_form(15, rt, ra, imm); }
constexpr u32 lis(int rt, std::int32_t imm) { return addis(rt, 0, imm); }
constexpr u32 ori(int ra, int rs, std::int32_t imm) { return d_form(24, rs, ra, imm); }
constexpr u32 oris(int ra, int rs, std::int32_t imm) { return d_form(25, rs, ra, imm); }
constexpr u32 xori(int ra, int rs, std::int32_t imm) { return d_form(26, rs, ra, imm); }
constexpr u32 xoris(int ra, int rs, std::int32_t imm) { return d_form(27, rs, ra, imm); }
constexpr u32 andi_(int ra, int rs, std::int32_t imm) { return d_form(28, rs, ra, imm); }
constexpr u32 andis_(int ra, int rs, std::int32_t imm) { return d_form(29, rs, ra, imm); }

// XO-form arithmetic: (rD, rA, rB, OE, Rc). subf* compute rB - rA.
constexpr u32 add(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 266, oe, rc); }
constexpr u32 addc(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 10, oe, rc); }
constexpr u32 adde(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 138, oe, rc); }
constexpr u32 addze(int rt, int ra, bool oe = false, bool rc = false) { return xo_form(rt, ra, 0, 202, oe, rc); }
constexpr u32 addme(int rt, int ra, bool oe = false, bool rc = false) { return xo_form(rt, ra, 0, 234, oe, rc); }
constexpr u32 subf(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 40, oe, rc); }
constexpr u32 subfc(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 8, oe, rc); }
constexpr u32 subfe(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 136, oe, rc); }
constexpr u32 subfze(int rt, int ra, bool oe = false, bool rc = false) { return xo_form(rt, ra, 0, 200, oe, rc); }
constexpr u32 subfme(int rt, int ra, bool oe = false, bool rc = false) { return xo_form(rt, ra, 0, 232, oe, rc); }
constexpr u32 neg(int rt, int ra, bool oe = false, bool rc = false) { return xo_form(rt, ra, 0, 104, oe, rc); }
constexpr u32 mullw(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 235, oe, rc); }
constexpr u32 mulhw(int rt, int ra, int rb, bool rc = false) { return xo_form(rt, ra, rb, 75, false, rc); }
constexpr u32 mulhwu(int rt, int ra, int rb, bool rc = false) { return xo_form(rt, ra, rb, 11, false, rc); }
constexpr u32 divw(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 491, oe, rc); }
constexpr u32 divwu(int rt, int ra, int rb, bool oe = false, bool rc = false) { return xo_form(rt, ra, rb, 459, oe, rc); }

// X-form logic and shifts: (rA, rS, rB, Rc).
constexpr u32 and_(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 28, rc); }
constexpr u32 andc(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 60, rc); }
constexpr u32 or_(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 444, rc); }
constexpr u32 orc(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 412, rc); }
constexpr u32 xor_(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 316, rc); }
constexpr u32 nand(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 476, rc); }
constexpr u32 nor(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 124, rc); }
constexpr u32 eqv(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 284, rc); }
constexpr u32 mr(int ra, int rs) { return or_(ra, rs, rs); }
constexpr u32 slw(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 24, rc); }
constexpr u32 srw(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 536, rc); }
constexpr u32 sraw(int ra, int rs, int rb, bool rc = false) { return x_form(31, rs, ra, rb, 792, rc); }
constexpr u32 srawi(int ra, int rs, int sh, bool rc = false) { return x_form(31, rs, ra, sh, 824, rc); }
constexpr u32 cntlzw(int ra, int rs, bool rc = false) { return x_form(31, rs, ra, 0, 26, rc); }
constexpr u32 extsb(int ra, int rs, bool rc = false) { return x_form(31, rs, ra, 0, 954, rc); }
constexpr u32 extsh(int ra, int rs, bool rc = false) { return x_form(31, rs, ra, 0, 922, rc); }
constexpr u32 cmpw(int crf, int ra, int rb) { return x_form(31, crf << 2, ra, rb, 0); }
constexpr u32 cmplw(int crf, int ra, int rb) { return x_form(31, crf << 2, ra, rb, 32); }
constexpr u32 tw(int to, int ra, int rb) { return x_form(31, to, ra, rb, 4); }

// Rotates: (rA, rS, SH or rB, MB, ME, Rc).
constexpr u32 rlwimi(int ra, int rs, int sh, int mb, int me, bool rc = false) { return m_form(20, rs, ra, sh, mb, me, rc); }
constexpr u32 rlwinm(int ra, int rs, int sh, int mb, int me, bool rc = false) { return m_form(21, rs, ra, sh, mb, me, rc); }
constexpr u32 rlwnm(int ra, int rs, int rb, int mb, int me, bool rc = false) { return m_form(23, rs, ra, rb, mb, me, rc); }

// Branches. Offsets are in bytes from the branch.
constexpr u32 b(std::int32_t offset) { return (18u << 26) | (static_cast<u32>(offset) & 0x03FFFFFC); }
constexpr u32 bl(std::int32_t offset) { return b(offset) | 1; }
constexpr u32 ba(u32 target) { return (18u << 26) | (target & 0x03FFFFFC) | 2; }
constexpr u32 bc(int bo, int bi, std::int32_t offset, bool lk = false) {
    return (16u << 26) | field(bo, 21) | field(bi, 16) | (static_cast<u32>(offset) & 0xFFFC) | (lk ? 1u : 0u);
}
constexpr u32 bclr(int bo, int bi, bool lk = false) { return x_form(19, bo, bi, 0, 16, lk); }
constexpr u32 bcctr(int bo, int bi, bool lk = false) { return x_form(19, bo, bi, 0, 528, lk); }
constexpr u32 blr() { return bclr(20, 0); }
constexpr u32 bctr() { return bcctr(20, 0); }

// Condition register
constexpr u32 mcrf(int crfd, int crfs) { return x_form(19, crfd << 2, crfs << 2, 0, 0); }
constexpr u32 crand(int d, int a, int bb) { return x_form(19, d, a, bb, 257); }
constexpr u32 crandc(int d, int a, int bb) { return x_form(19, d, a, bb, 129); }
constexpr u32 cror(int d, int a, int bb) { return x_form(19, d, a, bb, 449); }
constexpr u32 crorc(int d, int a, int bb) { return x_form(19, d, a, bb, 417); }
constexpr u32 crxor(int d, int a, int bb) { return x_form(19, d, a, bb, 193); }
constexpr u32 crnand(int d, int a, int bb) { return x_form(19, d, a, bb, 225); }
constexpr u32 crnor(int d, int a, int bb) { return x_form(19, d, a, bb, 33); }
constexpr u32 creqv(int d, int a, int bb) { return x_form(19, d, a, bb, 289); }
constexpr u32 mfcr(int rt) { return x_form(31, rt, 0, 0, 19); }
constexpr u32 mtcrf(int crm, int rs) {
    return (31u << 26) | field(rs, 21) | (static_cast<u32>(crm & 0xFF) << 12) | (144u << 1);
}
constexpr u32 mcrxr(int crf) { return x_form(31, crf << 2, 0, 0, 512); }

// Loads and stores, D-form: (rD/rS, offset, rA).
constexpr u32 lwz(int rt, std::int32_t off, int ra) { return d_form(32, rt, ra, off); }
constexpr u32 lwzu(int rt, std::int32_t off, int ra) { return d_form(33, rt, ra, off); }
constexpr u32 lbz(int rt, std::int32_t off, int ra) { return d_form(34, rt, ra, off); }
constexpr u32 lbzu(int rt, std::int32_t off, int ra) { return d_form(35, rt, ra, off); }
constexpr u32 stw(int rs, std::int32_t off, int ra) { return d_form(36, rs, ra, off); }
constexpr u32 stwu(int rs, std::int32_t off, int ra) { return d_form(37, rs, ra, off); }
constexpr u32 stb(int rs, std::int32_t off, int ra) { return d_form(38, rs, ra, off); }
constexpr u32 stbu(int rs, std::int32_t off, int ra) { return d_form(39, rs, ra, off); }
constexpr u32 lhz(int rt, std::int32_t off, int ra) { return d_form(40, rt, ra, off); }
constexpr u32 lhzu(int rt, std::int32_t off, int ra) { return d_form(41, rt, ra, off); }
constexpr u32 lha(int rt, std::int32_t off, int ra) { return d_form(42, rt, ra, off); }
constexpr u32 lhau(int rt, std::int32_t off, int ra) { return d_form(43, rt, ra, off); }
constexpr u32 sth(int rs, std::int32_t off, int ra) { return d_form(44, rs, ra, off); }
constexpr u32 sthu(int rs, std::int32_t off, int ra) { return d_form(45, rs, ra, off); }
constexpr u32 lmw(int rt, std::int32_t off, int ra) { return d_form(46, rt, ra, off); }
constexpr u32 stmw(int rs, std::int32_t off, int ra) { return d_form(47, rs, ra, off); }

// Loads and stores, X-form: (rD/rS, rA, rB).
constexpr u32 lwarx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 20); }
constexpr u32 lwzx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 23); }
constexpr u32 lwzux(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 55); }
constexpr u32 lbzx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 87); }
constexpr u32 lbzux(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 119); }
constexpr u32 stwcx_(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 150, true); }
constexpr u32 stwx(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 151); }
constexpr u32 stwux(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 183); }
constexpr u32 stbx(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 215); }
constexpr u32 stbux(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 247); }
constexpr u32 lhzx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 279); }
constexpr u32 lhzux(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 311); }
constexpr u32 lhax(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 343); }
constexpr u32 lhaux(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 375); }
constexpr u32 sthx(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 407); }
constexpr u32 sthux(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 439); }
constexpr u32 lswx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 533); }
constexpr u32 lwbrx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 534); }
constexpr u32 lswi(int rt, int ra, int nb) { return x_form(31, rt, ra, nb, 597); }
constexpr u32 stswx(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 661); }
constexpr u32 stwbrx(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 662); }
constexpr u32 stswi(int rs, int ra, int nb) { return x_form(31, rs, ra, nb, 725); }
constexpr u32 lhbrx(int rt, int ra, int rb) { return x_form(31, rt, ra, rb, 790); }
constexpr u32 sthbrx(int rs, int ra, int rb) { return x_form(31, rs, ra, rb, 918); }

// Caches and synchronization
constexpr u32 dcbst(int ra, int rb) { return x_form(31, 0, ra, rb, 54); }
constexpr u32 dcbf(int ra, int rb) { return x_form(31, 0, ra, rb, 86); }
constexpr u32 dcbtst(int ra, int rb) { return x_form(31, 0, ra, rb, 246); }
constexpr u32 dcbt(int ra, int rb) { return x_form(31, 0, ra, rb, 278); }
constexpr u32 dcbi(int ra, int rb) { return x_form(31, 0, ra, rb, 470); }
constexpr u32 icbi(int ra, int rb) { return x_form(31, 0, ra, rb, 982); }
constexpr u32 dcbz(int ra, int rb) { return x_form(31, 0, ra, rb, 1014); }
constexpr u32 dcbz_l(int ra, int rb) { return x_form(4, 0, ra, rb, 1014); }
constexpr u32 sync() { return x_form(31, 0, 0, 0, 598); }
constexpr u32 eieio() { return x_form(31, 0, 0, 0, 854); }
constexpr u32 isync() { return x_form(19, 0, 0, 0, 150); }

// System
constexpr u32 sc() { return (17u << 26) | 2; }
constexpr u32 rfi() { return x_form(19, 0, 0, 0, 50); }
constexpr u32 mfmsr(int rt) { return x_form(31, rt, 0, 0, 83); }
constexpr u32 mtmsr(int rs) { return x_form(31, rs, 0, 0, 146); }
constexpr u32 mfspr(int rt, int spr) { return (31u << 26) | field(rt, 21) | spr_field(spr) | (339u << 1); }
constexpr u32 mtspr(int spr, int rs) { return (31u << 26) | field(rs, 21) | spr_field(spr) | (467u << 1); }
constexpr u32 mftb(int rt, int tbr) { return (31u << 26) | field(rt, 21) | spr_field(tbr) | (371u << 1); }
constexpr u32 mfsr(int rt, int sr) { return x_form(31, rt, sr & 15, 0, 595); }
constexpr u32 mtsr(int sr, int rs) { return x_form(31, rs, sr & 15, 0, 210); }
constexpr u32 mfsrin(int rt, int rb) { return x_form(31, rt, 0, rb, 659); }
constexpr u32 mtsrin(int rs, int rb) { return x_form(31, rs, 0, rb, 242); }

}  // namespace ppc

// A Gekko on a flat bus. Programs go at physical 0x3000 by default (above the exception
// vectors); translation is off after construction (MSR = 0), so addresses are physical.
struct GekkoFixture {
    static constexpr std::uint32_t kProgram = 0x3000;
    FlatBus bus;
    ezy::gc::Gekko cpu{bus};

    GekkoFixture() { cpu.set_msr(0); }

    void load(std::initializer_list<std::uint32_t> words, std::uint32_t at = kProgram) {
        cpu.set_pc(at);
        for (const std::uint32_t w : words) {
            bus.write32(at, w);
            at += 4;
        }
    }
    int run(int instructions) {
        int cycles = 0;
        for (int n = 0; n < instructions; ++n) cycles += cpu.step();
        return cycles;
    }
    // Runs one instruction at kProgram.
    void exec(std::uint32_t word) {
        load({word});
        run(1);
    }
};
