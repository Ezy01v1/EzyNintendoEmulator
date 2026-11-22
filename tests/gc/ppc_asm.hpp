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

// A-form (floating-point arithmetic): opcode, frD, frA, frB, frC, a 5-bit extended opcode, Rc.
constexpr u32 a_form(int op, int d, int a, int b, int c, int xo, bool rc = false) {
    return (static_cast<u32>(op) << 26) | field(d, 21) | field(a, 16) | field(b, 11) | field(c, 6) |
           (static_cast<u32>(xo & 31) << 1) | (rc ? 1u : 0u);
}

// Floating-point loads and stores: (frD/frS, offset, rA) and (frD/frS, rA, rB).
constexpr u32 lfs(int ft, std::int32_t off, int ra) { return d_form(48, ft, ra, off); }
constexpr u32 lfsu(int ft, std::int32_t off, int ra) { return d_form(49, ft, ra, off); }
constexpr u32 lfd(int ft, std::int32_t off, int ra) { return d_form(50, ft, ra, off); }
constexpr u32 lfdu(int ft, std::int32_t off, int ra) { return d_form(51, ft, ra, off); }
constexpr u32 stfs(int fs, std::int32_t off, int ra) { return d_form(52, fs, ra, off); }
constexpr u32 stfsu(int fs, std::int32_t off, int ra) { return d_form(53, fs, ra, off); }
constexpr u32 stfd(int fs, std::int32_t off, int ra) { return d_form(54, fs, ra, off); }
constexpr u32 stfdu(int fs, std::int32_t off, int ra) { return d_form(55, fs, ra, off); }
constexpr u32 lfsx(int ft, int ra, int rb) { return x_form(31, ft, ra, rb, 535); }
constexpr u32 lfsux(int ft, int ra, int rb) { return x_form(31, ft, ra, rb, 567); }
constexpr u32 lfdx(int ft, int ra, int rb) { return x_form(31, ft, ra, rb, 599); }
constexpr u32 lfdux(int ft, int ra, int rb) { return x_form(31, ft, ra, rb, 631); }
constexpr u32 stfsx(int fs, int ra, int rb) { return x_form(31, fs, ra, rb, 663); }
constexpr u32 stfsux(int fs, int ra, int rb) { return x_form(31, fs, ra, rb, 695); }
constexpr u32 stfdx(int fs, int ra, int rb) { return x_form(31, fs, ra, rb, 727); }
constexpr u32 stfdux(int fs, int ra, int rb) { return x_form(31, fs, ra, rb, 759); }
constexpr u32 stfiwx(int fs, int ra, int rb) { return x_form(31, fs, ra, rb, 983); }

// Floating-point arithmetic, assembler operand order: fmul/fmadd take frC before frB
// (fmadd frD,frA,frC,frB computes frA * frC + frB).
constexpr u32 fadd(int d, int a, int b, bool rc = false) { return a_form(63, d, a, b, 0, 21, rc); }
constexpr u32 fadds(int d, int a, int b, bool rc = false) { return a_form(59, d, a, b, 0, 21, rc); }
constexpr u32 fsub(int d, int a, int b, bool rc = false) { return a_form(63, d, a, b, 0, 20, rc); }
constexpr u32 fsubs(int d, int a, int b, bool rc = false) { return a_form(59, d, a, b, 0, 20, rc); }
constexpr u32 fmul(int d, int a, int c, bool rc = false) { return a_form(63, d, a, 0, c, 25, rc); }
constexpr u32 fmuls(int d, int a, int c, bool rc = false) { return a_form(59, d, a, 0, c, 25, rc); }
constexpr u32 fdiv(int d, int a, int b, bool rc = false) { return a_form(63, d, a, b, 0, 18, rc); }
constexpr u32 fdivs(int d, int a, int b, bool rc = false) { return a_form(59, d, a, b, 0, 18, rc); }
constexpr u32 fmadd(int d, int a, int c, int b) { return a_form(63, d, a, b, c, 29); }
constexpr u32 fmadds(int d, int a, int c, int b) { return a_form(59, d, a, b, c, 29); }
constexpr u32 fmsub(int d, int a, int c, int b) { return a_form(63, d, a, b, c, 28); }
constexpr u32 fmsubs(int d, int a, int c, int b) { return a_form(59, d, a, b, c, 28); }
constexpr u32 fnmadd(int d, int a, int c, int b) { return a_form(63, d, a, b, c, 31); }
constexpr u32 fnmadds(int d, int a, int c, int b) { return a_form(59, d, a, b, c, 31); }
constexpr u32 fnmsub(int d, int a, int c, int b) { return a_form(63, d, a, b, c, 30); }
constexpr u32 fnmsubs(int d, int a, int c, int b) { return a_form(59, d, a, b, c, 30); }
constexpr u32 fsel(int d, int a, int c, int b) { return a_form(63, d, a, b, c, 23); }
constexpr u32 fres(int d, int b) { return a_form(59, d, 0, b, 0, 24); }
constexpr u32 frsqrte(int d, int b) { return a_form(63, d, 0, b, 0, 26); }

// Floating-point X-form (opcode 63)
constexpr u32 fcmpu(int crf, int a, int b) { return x_form(63, crf << 2, a, b, 0); }
constexpr u32 fcmpo(int crf, int a, int b) { return x_form(63, crf << 2, a, b, 32); }
constexpr u32 frsp(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 12, rc); }
constexpr u32 fctiw(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 14, rc); }
constexpr u32 fctiwz(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 15, rc); }
constexpr u32 fneg(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 40, rc); }
constexpr u32 fmr(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 72, rc); }
constexpr u32 fnabs(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 136, rc); }
constexpr u32 fabs_(int d, int b, bool rc = false) { return x_form(63, d, 0, b, 264, rc); }
constexpr u32 mffs(int d, bool rc = false) { return x_form(63, d, 0, 0, 583, rc); }
constexpr u32 mtfsb0(int bit, bool rc = false) { return x_form(63, bit, 0, 0, 70, rc); }
constexpr u32 mtfsb1(int bit, bool rc = false) { return x_form(63, bit, 0, 0, 38, rc); }
constexpr u32 mcrfs(int crfd, int crfs) { return x_form(63, crfd << 2, crfs << 2, 0, 64); }
constexpr u32 mtfsfi(int crf, int imm, bool rc = false) {
    return (63u << 26) | (static_cast<u32>(crf & 7) << 23) | (static_cast<u32>(imm & 15) << 12) | (134u << 1) |
           (rc ? 1u : 0u);
}
constexpr u32 mtfsf(int fm, int b, bool rc = false) {
    return (63u << 26) | (static_cast<u32>(fm & 0xFF) << 17) | field(b, 11) | (711u << 1) | (rc ? 1u : 0u);
}

// Paired singles (opcode 4). A-form ones take (frD, frA, [frC,] [frB]) like the assembler.
constexpr u32 ps_add(int d, int a, int b) { return a_form(4, d, a, b, 0, 21); }
constexpr u32 ps_sub(int d, int a, int b) { return a_form(4, d, a, b, 0, 20); }
constexpr u32 ps_mul(int d, int a, int c) { return a_form(4, d, a, 0, c, 25); }
constexpr u32 ps_div(int d, int a, int b) { return a_form(4, d, a, b, 0, 18); }
constexpr u32 ps_madd(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 29); }
constexpr u32 ps_msub(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 28); }
constexpr u32 ps_nmadd(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 31); }
constexpr u32 ps_nmsub(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 30); }
constexpr u32 ps_sel(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 23); }
constexpr u32 ps_sum0(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 10); }
constexpr u32 ps_sum1(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 11); }
constexpr u32 ps_muls0(int d, int a, int c) { return a_form(4, d, a, 0, c, 12); }
constexpr u32 ps_muls1(int d, int a, int c) { return a_form(4, d, a, 0, c, 13); }
constexpr u32 ps_madds0(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 14); }
constexpr u32 ps_madds1(int d, int a, int c, int b) { return a_form(4, d, a, b, c, 15); }
constexpr u32 ps_res(int d, int b) { return a_form(4, d, 0, b, 0, 24); }
constexpr u32 ps_rsqrte(int d, int b) { return a_form(4, d, 0, b, 0, 26); }
constexpr u32 ps_cmpu0(int crf, int a, int b) { return x_form(4, crf << 2, a, b, 0); }
constexpr u32 ps_cmpo0(int crf, int a, int b) { return x_form(4, crf << 2, a, b, 32); }
constexpr u32 ps_cmpu1(int crf, int a, int b) { return x_form(4, crf << 2, a, b, 64); }
constexpr u32 ps_cmpo1(int crf, int a, int b) { return x_form(4, crf << 2, a, b, 96); }
constexpr u32 ps_neg(int d, int b) { return x_form(4, d, 0, b, 40); }
constexpr u32 ps_mr(int d, int b) { return x_form(4, d, 0, b, 72); }
constexpr u32 ps_nabs(int d, int b) { return x_form(4, d, 0, b, 136); }
constexpr u32 ps_abs(int d, int b) { return x_form(4, d, 0, b, 264); }
constexpr u32 ps_merge00(int d, int a, int b) { return x_form(4, d, a, b, 528); }
constexpr u32 ps_merge01(int d, int a, int b) { return x_form(4, d, a, b, 560); }
constexpr u32 ps_merge10(int d, int a, int b) { return x_form(4, d, a, b, 592); }
constexpr u32 ps_merge11(int d, int a, int b) { return x_form(4, d, a, b, 624); }

// Quantized loads and stores: W (one value), I (the GQR). D-form: (frD, offset (12-bit), rA,
// W, I); indexed (opcode 4): (frD, rA, rB, W, I).
constexpr u32 psq_d(int op, int f, std::int32_t off, int ra, int w, int i) {
    return (static_cast<u32>(op) << 26) | field(f, 21) | field(ra, 16) | (static_cast<u32>(w & 1) << 15) |
           (static_cast<u32>(i & 7) << 12) | (static_cast<u32>(off) & 0xFFF);
}
constexpr u32 psq_x(int xo, int f, int ra, int rb, int w, int i) {
    return (4u << 26) | field(f, 21) | field(ra, 16) | field(rb, 11) | (static_cast<u32>(w & 1) << 10) |
           (static_cast<u32>(i & 7) << 7) | (static_cast<u32>(xo) << 1);
}
constexpr u32 psq_l(int f, std::int32_t off, int ra, int w, int i) { return psq_d(56, f, off, ra, w, i); }
constexpr u32 psq_lu(int f, std::int32_t off, int ra, int w, int i) { return psq_d(57, f, off, ra, w, i); }
constexpr u32 psq_st(int f, std::int32_t off, int ra, int w, int i) { return psq_d(60, f, off, ra, w, i); }
constexpr u32 psq_stu(int f, std::int32_t off, int ra, int w, int i) { return psq_d(61, f, off, ra, w, i); }
constexpr u32 psq_lx(int f, int ra, int rb, int w, int i) { return psq_x(6, f, ra, rb, w, i); }
constexpr u32 psq_stx(int f, int ra, int rb, int w, int i) { return psq_x(7, f, ra, rb, w, i); }
constexpr u32 psq_lux(int f, int ra, int rb, int w, int i) { return psq_x(38, f, ra, rb, w, i); }
constexpr u32 psq_stux(int f, int ra, int rb, int w, int i) { return psq_x(39, f, ra, rb, w, i); }

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
