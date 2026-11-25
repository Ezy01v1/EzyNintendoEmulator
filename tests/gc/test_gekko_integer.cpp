#include <doctest/doctest.h>

#include <cstdint>

#include "ppc_asm.hpp"

using namespace ppc;
using ezy::gc::Gekko;

namespace {

constexpr std::uint32_t kSo = Gekko::kXerSo, kOv = Gekko::kXerOv, kCa = Gekko::kXerCa;
// CR field values: LT, GT, EQ, SO from the field's high bit down.
constexpr std::uint32_t kLt = 8, kGt = 4, kEq = 2, kSoBit = 1;

std::uint32_t cr_field(const Gekko& cpu, int field) { return (cpu.cr() >> (28 - 4 * field)) & 0xF; }

}  // namespace

TEST_CASE("add, subf, addic and adde set CA, OV, SO and CR0") {
    GekkoFixture f;
    auto& cpu = f.cpu;

    // addo.: 0x7FFFFFFF + 1 overflows to the most negative value.
    cpu.set_gpr(4, 0x7FFFFFFF);
    cpu.set_gpr(5, 1);
    f.exec(add(3, 4, 5, true, true));
    CHECK(cpu.gpr(3) == 0x80000000);
    CHECK((cpu.xer() & kOv) != 0);
    CHECK((cpu.xer() & kSo) != 0);
    CHECK((cpu.xer() & kCa) == 0);  // add never touches CA
    CHECK(cr_field(cpu, 0) == (kLt | kSoBit));

    // addo. without overflow clears OV; SO is sticky and shows in CR0.
    cpu.set_gpr(4, 1);
    f.exec(add(3, 4, 5, true, true));
    CHECK(cpu.gpr(3) == 2);
    CHECK((cpu.xer() & kOv) == 0);
    CHECK((cpu.xer() & kSo) != 0);
    CHECK(cr_field(cpu, 0) == (kGt | kSoBit));

    // A plain add leaves XER and CR alone.
    cpu.set_xer(0);
    cpu.set_cr(0);
    cpu.set_gpr(4, 0x7FFFFFFF);
    f.exec(add(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x80000000);
    CHECK(cpu.xer() == 0);
    CHECK(cpu.cr() == 0);

    // addic carries out of 0xFFFFFFFF + 1 and does not set CR0; addic. does.
    cpu.set_gpr(4, 0xFFFFFFFF);
    f.exec(addic(3, 4, 1));
    CHECK(cpu.gpr(3) == 0);
    CHECK((cpu.xer() & kCa) != 0);
    CHECK(cpu.cr() == 0);
    f.exec(addic_(3, 4, 1));
    CHECK(cr_field(cpu, 0) == kEq);
    f.exec(addic(3, 4, -1));  // 0xFFFFFFFF + 0xFFFFFFFF carries too
    CHECK(cpu.gpr(3) == 0xFFFFFFFE);
    CHECK((cpu.xer() & kCa) != 0);
    cpu.set_gpr(4, 5);
    f.exec(addic(3, 4, 1));
    CHECK((cpu.xer() & kCa) == 0);

    // adde adds CA in; addc sets it.
    cpu.set_xer(kCa);
    cpu.set_gpr(6, 1);
    cpu.set_gpr(7, 2);
    f.exec(adde(3, 6, 7));
    CHECK(cpu.gpr(3) == 4);
    CHECK((cpu.xer() & kCa) == 0);
    cpu.set_gpr(6, 0x80000000);
    cpu.set_gpr(7, 0x80000000);
    f.exec(addc(3, 6, 7, true));
    CHECK(cpu.gpr(3) == 0);
    CHECK((cpu.xer() & (kCa | kOv | kSo)) == (kCa | kOv | kSo));

    // subf computes rB - rA; subfc sets CA when there is no borrow.
    cpu.set_xer(0);
    cpu.set_gpr(4, 1);
    cpu.set_gpr(5, 3);
    f.exec(subf(3, 4, 5));
    CHECK(cpu.gpr(3) == 2);
    f.exec(subfc(3, 4, 5));
    CHECK(cpu.gpr(3) == 2);
    CHECK((cpu.xer() & kCa) != 0);
    f.exec(subfc(3, 5, 4));
    CHECK(cpu.gpr(3) == 0xFFFFFFFE);
    CHECK((cpu.xer() & kCa) == 0);

    // subfe: ~rA + rB + CA, so CA = 0 subtracts one more.
    cpu.set_xer(0);
    f.exec(subfe(3, 4, 5));
    CHECK(cpu.gpr(3) == 1);
    CHECK((cpu.xer() & kCa) != 0);
    f.exec(subfe(3, 4, 5));  // now CA = 1
    CHECK(cpu.gpr(3) == 2);
    CHECK((cpu.xer() & kCa) != 0);

    // subfo.: 0x80000000 - 1 overflows.
    cpu.set_xer(0);
    cpu.set_gpr(4, 1);
    cpu.set_gpr(5, 0x80000000);
    f.exec(subf(3, 4, 5, true, true));
    CHECK(cpu.gpr(3) == 0x7FFFFFFF);
    CHECK((cpu.xer() & (kOv | kSo)) == (kOv | kSo));
    CHECK(cr_field(cpu, 0) == (kGt | kSoBit));

    // subfic: SIMM - rA with CA.
    cpu.set_xer(0);
    cpu.set_gpr(4, 3);
    f.exec(subfic(3, 4, 10));
    CHECK(cpu.gpr(3) == 7);
    CHECK((cpu.xer() & kCa) != 0);
    f.exec(subfic(3, 4, 1));
    CHECK(cpu.gpr(3) == 0xFFFFFFFE);
    CHECK((cpu.xer() & kCa) == 0);

    // addze / addme / subfze / subfme with CA.
    cpu.set_xer(kCa);
    cpu.set_gpr(4, 0xFFFFFFFF);
    f.exec(addze(3, 4));
    CHECK(cpu.gpr(3) == 0);
    CHECK((cpu.xer() & kCa) != 0);
    cpu.set_xer(0);
    cpu.set_gpr(4, 5);
    f.exec(addme(3, 4));  // 5 + 0 - 1
    CHECK(cpu.gpr(3) == 4);
    CHECK((cpu.xer() & kCa) != 0);
    cpu.set_xer(kCa);
    f.exec(subfze(3, 4));  // ~5 + 1 = -5
    CHECK(cpu.gpr(3) == 0xFFFFFFFB);
    CHECK((cpu.xer() & kCa) == 0);
    cpu.set_xer(kCa);
    f.exec(subfme(3, 4));  // ~5 + 1 - 1 = -6
    CHECK(cpu.gpr(3) == 0xFFFFFFFA);
    CHECK((cpu.xer() & kCa) != 0);

    // neg overflows only on the most negative value; it never changes CA.
    cpu.set_xer(0);
    cpu.set_gpr(4, 5);
    f.exec(neg(3, 4, true));
    CHECK(cpu.gpr(3) == 0xFFFFFFFB);
    CHECK(cpu.xer() == 0);
    cpu.set_gpr(4, 0x80000000);
    f.exec(neg(3, 4, true, true));
    CHECK(cpu.gpr(3) == 0x80000000);
    CHECK(cpu.xer() == (kOv | kSo));
    CHECK(cr_field(cpu, 0) == (kLt | kSoBit));

    // addi/addis use 0, not r0, when rA = 0.
    cpu.set_gpr(0, 100);
    f.exec(addi(3, 0, -2));
    CHECK(cpu.gpr(3) == 0xFFFFFFFE);
    f.exec(addis(3, 0, 0x1234));
    CHECK(cpu.gpr(3) == 0x12340000);
    cpu.set_gpr(4, 0x10);
    f.exec(addis(3, 4, -1));
    CHECK(cpu.gpr(3) == 0xFFFF0010);
}

TEST_CASE("rlwinm, rlwimi and rlwnm masks wrap around") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_gpr(4, 0x12345678);

    f.exec(rlwinm(3, 4, 0, 28, 3));  // MB > ME: bits 28..31 and 0..3
    CHECK(cpu.gpr(3) == 0x10000008);
    f.exec(rlwinm(3, 4, 8, 0, 31));
    CHECK(cpu.gpr(3) == 0x34567812);
    f.exec(rlwinm(3, 4, 0, 4, 3));  // MB = ME + 1: every bit
    CHECK(cpu.gpr(3) == 0x12345678);
    f.exec(rlwinm(3, 4, 4, 24, 27));  // one nibble of 0x23456781
    CHECK(cpu.gpr(3) == 0x00000080);
    f.exec(rlwinm(3, 4, 0, 16, 16));  // one bit, MB = ME
    CHECK(cpu.gpr(3) == 0x00000000);
    f.exec(rlwinm(3, 4, 0, 19, 19));
    CHECK(cpu.gpr(3) == 0x00001000);

    cpu.set_gpr(3, 0xAAAAAAAA);
    f.exec(rlwimi(3, 4, 4, 28, 3));  // rot = 0x23456781, mask 0xF000000F
    CHECK(cpu.gpr(3) == 0x2AAAAAA1);

    cpu.set_gpr(5, 36);  // only the low 5 bits count
    f.exec(rlwnm(3, 4, 5, 0, 31));
    CHECK(cpu.gpr(3) == 0x23456781);
    f.exec(rlwnm(3, 4, 5, 28, 3));
    CHECK(cpu.gpr(3) == 0x20000001);

    cpu.set_gpr(4, 0x80000000);
    f.exec(rlwinm(3, 4, 0, 0, 0, true));
    CHECK(cpu.gpr(3) == 0x80000000);
    CHECK(cr_field(cpu, 0) == kLt);
    f.exec(rlwinm(3, 4, 0, 1, 31, true));
    CHECK(cr_field(cpu, 0) == kEq);
}

TEST_CASE("divw and divwu by zero and overflow give the hardware results") {
    GekkoFixture f;
    auto& cpu = f.cpu;

    cpu.set_gpr(4, 7);
    cpu.set_gpr(5, 0);
    f.exec(divw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0);
    cpu.set_gpr(4, static_cast<std::uint32_t>(-7));
    f.exec(divw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xFFFFFFFF);
    CHECK(cpu.xer() == 0);  // no OE: OV untouched

    // 0x80000000 / -1 gives -1 like a negative dividend over zero (Gekko behavior,
    // as Dolphin's Interpreter_Integer.cpp divwx implements it).
    cpu.set_gpr(4, 0x80000000);
    cpu.set_gpr(5, 0xFFFFFFFF);
    f.exec(divw(3, 4, 5, true, true));
    CHECK(cpu.gpr(3) == 0xFFFFFFFF);
    CHECK(cpu.xer() == (kOv | kSo));
    CHECK(cr_field(cpu, 0) == (kLt | kSoBit));

    // A normal divide truncates toward zero and clears OV (SO stays).
    cpu.set_gpr(4, static_cast<std::uint32_t>(-100));
    cpu.set_gpr(5, 7);
    f.exec(divw(3, 4, 5, true));
    CHECK(cpu.gpr(3) == static_cast<std::uint32_t>(-14));
    CHECK(cpu.xer() == kSo);

    cpu.set_xer(0);
    cpu.set_gpr(4, 0xFFFFFFFF);
    cpu.set_gpr(5, 0);
    f.exec(divwu(3, 4, 5, true));
    CHECK(cpu.gpr(3) == 0);
    CHECK(cpu.xer() == (kOv | kSo));
    cpu.set_xer(0);
    cpu.set_gpr(5, 7);
    f.exec(divwu(3, 4, 5, true));
    CHECK(cpu.gpr(3) == 0xFFFFFFFFu / 7);
    CHECK(cpu.xer() == 0);
}

TEST_CASE("multiplies: mullw overflow, mulhw, mulhwu and mulli") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_gpr(4, 0x10000);
    cpu.set_gpr(5, 0x10000);
    f.exec(mullw(3, 4, 5, true));
    CHECK(cpu.gpr(3) == 0);
    CHECK(cpu.xer() == (kOv | kSo));
    cpu.set_xer(0);
    cpu.set_gpr(4, static_cast<std::uint32_t>(-3));
    cpu.set_gpr(5, 7);
    f.exec(mullw(3, 4, 5, true, true));
    CHECK(cpu.gpr(3) == static_cast<std::uint32_t>(-21));
    CHECK(cpu.xer() == 0);
    CHECK(cr_field(cpu, 0) == kLt);
    f.exec(mulhw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xFFFFFFFF);
    f.exec(mulhwu(3, 4, 5));
    CHECK(cpu.gpr(3) == 6);  // (2^32 - 3) * 7 >> 32
    f.exec(mulli(3, 5, -2));
    CHECK(cpu.gpr(3) == static_cast<std::uint32_t>(-14));
}

TEST_CASE("logic, shifts, sign extension and count leading zeros") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_gpr(4, 0xF0F0FF00);
    cpu.set_gpr(5, 0xFF00F0F0);
    f.exec(and_(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xF000F000);
    f.exec(andc(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x00F00F00);
    f.exec(or_(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xFFF0FFF0);
    f.exec(orc(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xF0FFFF0F);
    f.exec(xor_(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x0FF00FF0);
    f.exec(nand(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x0FFF0FFF);
    f.exec(nor(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x000F000F);
    f.exec(eqv(3, 4, 5, true));
    CHECK(cpu.gpr(3) == 0xF00FF00F);
    CHECK(cr_field(cpu, 0) == kLt);

    f.exec(ori(3, 4, 0x00FF));
    CHECK(cpu.gpr(3) == 0xF0F0FFFF);
    f.exec(oris(3, 4, 0x0F0F));
    CHECK(cpu.gpr(3) == 0xFFFFFF00);
    f.exec(xori(3, 4, 0xFFFF));
    CHECK(cpu.gpr(3) == 0xF0F000FF);
    f.exec(xoris(3, 4, 0xFFFF));
    CHECK(cpu.gpr(3) == 0x0F0FFF00);
    cpu.set_cr(0);
    f.exec(andi_(3, 4, 0x00FF));  // always records
    CHECK(cpu.gpr(3) == 0);
    CHECK(cr_field(cpu, 0) == kEq);
    f.exec(andis_(3, 4, 0x8000));
    CHECK(cpu.gpr(3) == 0x80000000);
    CHECK(cr_field(cpu, 0) == kLt);

    cpu.set_gpr(4, 0x80000001);
    cpu.set_gpr(5, 4);
    f.exec(slw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x00000010);
    f.exec(srw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x08000000);
    cpu.set_gpr(5, 32);  // 32..63 shifts everything out
    f.exec(slw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0);
    f.exec(srw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0);
    cpu.set_gpr(5, 64);  // only 6 bits count: 64 is a shift of 0
    f.exec(slw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x80000001);

    // sraw/srawi: CA = negative and a 1 bit shifted out.
    cpu.set_xer(0);
    cpu.set_gpr(5, 4);
    f.exec(sraw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xF8000000);
    CHECK((cpu.xer() & kCa) != 0);
    f.exec(srawi(3, 4, 0));
    CHECK(cpu.gpr(3) == 0x80000001);
    CHECK((cpu.xer() & kCa) == 0);
    cpu.set_gpr(4, 0x80000010);
    f.exec(srawi(3, 4, 4));
    CHECK(cpu.gpr(3) == 0xF8000001);
    CHECK((cpu.xer() & kCa) == 0);  // only zeros shifted out
    cpu.set_gpr(5, 40);
    f.exec(sraw(3, 4, 5, true));
    CHECK(cpu.gpr(3) == 0xFFFFFFFF);
    CHECK((cpu.xer() & kCa) != 0);
    CHECK(cr_field(cpu, 0) == kLt);
    cpu.set_gpr(4, 0x7FFFFFFF);
    f.exec(sraw(3, 4, 5));
    CHECK(cpu.gpr(3) == 0);
    CHECK((cpu.xer() & kCa) == 0);
    f.exec(srawi(3, 4, 31));
    CHECK(cpu.gpr(3) == 0);
    CHECK((cpu.xer() & kCa) == 0);

    cpu.set_gpr(4, 0x00001080);
    f.exec(extsb(3, 4));
    CHECK(cpu.gpr(3) == 0xFFFFFF80);
    f.exec(extsh(3, 4));
    CHECK(cpu.gpr(3) == 0x00001080);
    cpu.set_gpr(4, 0x00008000);
    f.exec(extsh(3, 4));
    CHECK(cpu.gpr(3) == 0xFFFF8000);
    f.exec(cntlzw(3, 4));
    CHECK(cpu.gpr(3) == 16);
    cpu.set_gpr(4, 0);
    f.exec(cntlzw(3, 4, true));
    CHECK(cpu.gpr(3) == 32);
    CHECK(cr_field(cpu, 0) == kGt);
}

TEST_CASE("compares and condition register operations") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_gpr(4, 0xFFFFFFFF);  // -1 signed, the largest unsigned
    cpu.set_gpr(5, 1);
    f.exec(cmpw(0, 4, 5));
    CHECK(cr_field(cpu, 0) == kLt);
    f.exec(cmplw(1, 4, 5));
    CHECK(cr_field(cpu, 1) == kGt);
    f.exec(cmpwi(7, 4, -1));
    CHECK(cr_field(cpu, 7) == kEq);
    f.exec(cmplwi(6, 5, 0xFFFF));  // unsigned immediate, not sign-extended
    CHECK(cr_field(cpu, 6) == kLt);
    cpu.set_xer(kSo);
    f.exec(cmpwi(2, 5, 1));
    CHECK(cr_field(cpu, 2) == (kEq | kSoBit));
    CHECK(cr_field(cpu, 0) == kLt);  // other fields untouched

    cpu.set_cr(0x80000000);  // CR bit 0 (cr0 LT)
    f.exec(cror(5, 0, 1));
    CHECK((cpu.cr() >> (31 - 5) & 1) == 1);
    f.exec(crand(6, 0, 1));
    CHECK((cpu.cr() >> (31 - 6) & 1) == 0);
    f.exec(crandc(7, 0, 1));
    CHECK((cpu.cr() >> (31 - 7) & 1) == 1);
    f.exec(crxor(0, 0, 0));  // crclr
    CHECK((cpu.cr() >> 31) == 0);
    f.exec(creqv(1, 1, 1));  // crset
    CHECK((cpu.cr() >> 30 & 1) == 1);
    f.exec(crnor(2, 0, 0));  // crnot of a 0
    CHECK((cpu.cr() >> 29 & 1) == 1);
    f.exec(crnand(3, 1, 2));
    CHECK((cpu.cr() >> 28 & 1) == 0);
    f.exec(crorc(4, 0, 1));  // 0 | ~1
    CHECK((cpu.cr() >> 27 & 1) == 0);

    cpu.set_cr(0x12345678);
    f.exec(mcrf(7, 1));
    CHECK(cpu.cr() == 0x12345672);
    f.exec(mfcr(3));
    CHECK(cpu.gpr(3) == 0x12345672);
    cpu.set_gpr(4, 0xABCDEF01);
    f.exec(mtcrf(0x81, 4));  // fields 0 and 7
    CHECK(cpu.cr() == 0xA2345671);

    cpu.set_xer(0xE0000012);
    f.exec(mcrxr(3));
    CHECK(cr_field(cpu, 3) == 0xE);
    CHECK(cpu.xer() == 0x00000012);
}

TEST_CASE("branches use BO and BI, decrement CTR and set LR") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    constexpr std::uint32_t p = GekkoFixture::kProgram;

    // bl: LR = the next instruction.
    f.exec(bl(0x10));
    CHECK(cpu.pc() == p + 0x10);
    CHECK(cpu.lr() == p + 4);

    // b backwards and ba absolute.
    f.exec(b(-8));
    CHECK(cpu.pc() == p - 8);
    f.exec(ba(0x100));
    CHECK(cpu.pc() == 0x100);

    // A bdnz loop runs CTR times.
    cpu.set_ctr(3);
    f.load({li(3, 0), addi(3, 3, 1), bc(16, 0, -4), li(4, 9)});
    f.run(8);
    CHECK(cpu.gpr(3) == 3);
    CHECK(cpu.ctr() == 0);
    CHECK(cpu.gpr(4) == 9);
    CHECK(cpu.pc() == p + 16);

    // bdz: branch when CTR reaches 0.
    cpu.set_ctr(1);
    f.exec(bc(18, 0, 0x20));
    CHECK(cpu.pc() == p + 0x20);
    cpu.set_ctr(2);
    f.exec(bc(18, 0, 0x20));
    CHECK(cpu.pc() == p + 4);
    CHECK(cpu.ctr() == 1);

    // beq/bne on cr0, and a condition in cr1.
    cpu.set_gpr(3, 3);
    f.load({cmpwi(0, 3, 3), bc(12, 2, 8)});  // beq +8
    f.run(2);
    CHECK(cpu.pc() == p + 12);
    f.load({cmpwi(0, 3, 3), bc(4, 2, 8)});  // bne +8: not taken
    f.run(2);
    CHECK(cpu.pc() == p + 8);
    f.load({cmpwi(1, 3, 4), bc(12, 4, 0x40, true)});  // blt cr1,+0x40 with link
    f.run(2);
    CHECK(cpu.pc() == p + 4 + 0x40);
    CHECK(cpu.lr() == p + 8);

    // bdnzt: decrement and test both.
    cpu.set_ctr(2);
    cpu.set_cr(0x20000000);  // cr0 EQ
    f.exec(bc(8, 2, 0x10));
    CHECK(cpu.pc() == p + 0x10);
    CHECK(cpu.ctr() == 1);
    f.exec(bc(8, 2, 0x10));  // CTR becomes 0: not taken
    CHECK(cpu.pc() == p + 4);

    // A not-taken bc with LK still sets LR.
    cpu.set_cr(0);
    cpu.set_lr(0);
    f.exec(bc(12, 2, 0x10, true));
    CHECK(cpu.pc() == p + 4);
    CHECK(cpu.lr() == p + 4);

    // blr ignores the low 2 bits; blrl links after reading the old LR.
    cpu.set_lr(0x5103);
    f.exec(blr());
    CHECK(cpu.pc() == 0x5100);
    cpu.set_lr(0x5200);
    f.exec(bclr(20, 0, true));
    CHECK(cpu.pc() == 0x5200);
    CHECK(cpu.lr() == p + 4);
    // bdnzlr
    cpu.set_lr(0x5300);
    cpu.set_ctr(5);
    f.exec(bclr(16, 0));
    CHECK(cpu.pc() == 0x5300);
    CHECK(cpu.ctr() == 4);

    // bctr / bctrl, and a conditional bcctr not taken.
    cpu.set_ctr(0x6000);
    f.exec(bctr());
    CHECK(cpu.pc() == 0x6000);
    f.exec(bcctr(20, 0, true));
    CHECK(cpu.pc() == 0x6000);
    CHECK(cpu.lr() == p + 4);
    CHECK(cpu.ctr() == 0x6000);
    cpu.set_cr(0);
    f.exec(bcctr(12, 2));
    CHECK(cpu.pc() == p + 4);
}

TEST_CASE("loads and stores: update forms, indexed, byte-reversed, lmw/stmw") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    bus.write32(0x1000, 0x8899AABB);
    bus.write32(0x1004, 0x11223344);
    cpu.set_gpr(4, 0x1000);

    f.exec(lwz(3, 0, 4));
    CHECK(cpu.gpr(3) == 0x8899AABB);
    f.exec(lbz(3, 1, 4));
    CHECK(cpu.gpr(3) == 0x99);
    f.exec(lhz(3, 2, 4));
    CHECK(cpu.gpr(3) == 0xAABB);
    f.exec(lha(3, 2, 4));
    CHECK(cpu.gpr(3) == 0xFFFFAABB);
    f.exec(lwz(3, 1, 4));  // unaligned word
    CHECK(cpu.gpr(3) == 0x99AABB11);
    cpu.set_gpr(0, 0x500);  // rA = 0 means 0, not r0
    f.exec(lwz(3, 0x1004, 0));
    CHECK(cpu.gpr(3) == 0x11223344);

    // Update forms write the address back to rA.
    f.exec(lwzu(3, 4, 4));
    CHECK(cpu.gpr(3) == 0x11223344);
    CHECK(cpu.gpr(4) == 0x1004);
    f.exec(lbzu(3, -1, 4));
    CHECK(cpu.gpr(3) == 0xBB);
    CHECK(cpu.gpr(4) == 0x1003);
    f.exec(lhzu(3, -1, 4));
    CHECK(cpu.gpr(3) == 0xAABB);
    CHECK(cpu.gpr(4) == 0x1002);
    f.exec(lhau(3, -2, 4));
    CHECK(cpu.gpr(3) == 0xFFFF8899);
    CHECK(cpu.gpr(4) == 0x1000);

    // Indexed forms.
    cpu.set_gpr(5, 4);
    f.exec(lwzx(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x11223344);
    f.exec(lbzx(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x11);
    f.exec(lhzx(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x1122);
    f.exec(lhax(3, 4, 0));  // rA = r4 (0x1000), rB = r0 (0x500)
    CHECK(cpu.gpr(3) == bus.read16(0x1500));
    f.exec(lwzux(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x11223344);
    CHECK(cpu.gpr(4) == 0x1004);
    cpu.set_gpr(5, static_cast<std::uint32_t>(-4));
    f.exec(lhaux(3, 4, 5));
    CHECK(cpu.gpr(3) == 0xFFFF8899);
    CHECK(cpu.gpr(4) == 0x1000);
    cpu.set_gpr(5, 7);
    f.exec(lbzux(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x44);
    CHECK(cpu.gpr(4) == 0x1007);
    cpu.set_gpr(5, static_cast<std::uint32_t>(-3));
    f.exec(lhzux(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x1122);
    CHECK(cpu.gpr(4) == 0x1004);

    // Stores.
    cpu.set_gpr(4, 0x2000);
    cpu.set_gpr(6, 0xCAFEBABE);
    f.exec(stw(6, 0, 4));
    CHECK(bus.read32(0x2000) == 0xCAFEBABE);
    f.exec(sth(6, 4, 4));
    CHECK(bus.read16(0x2004) == 0xBABE);
    f.exec(stb(6, 6, 4));
    CHECK(bus.read8(0x2006) == 0xBE);
    f.exec(stw(6, 9, 4));  // unaligned
    CHECK(bus.read32(0x2009) == 0xCAFEBABE);
    f.exec(stwu(6, 0x10, 4));
    CHECK(bus.read32(0x2010) == 0xCAFEBABE);
    CHECK(cpu.gpr(4) == 0x2010);
    f.exec(sthu(6, 4, 4));
    CHECK(bus.read16(0x2014) == 0xBABE);
    CHECK(cpu.gpr(4) == 0x2014);
    f.exec(stbu(6, 4, 4));
    CHECK(bus.read8(0x2018) == 0xBE);
    CHECK(cpu.gpr(4) == 0x2018);
    cpu.set_gpr(5, 8);
    f.exec(stwx(6, 4, 5));
    CHECK(bus.read32(0x2020) == 0xCAFEBABE);
    f.exec(sthx(6, 4, 5));
    CHECK(bus.read16(0x2020) == 0xBABE);
    f.exec(stbx(6, 4, 5));
    CHECK(bus.read8(0x2020) == 0xBE);
    f.exec(stwux(6, 4, 5));
    CHECK(bus.read32(0x2020) == 0xCAFEBABE);
    CHECK(cpu.gpr(4) == 0x2020);
    f.exec(sthux(6, 4, 5));
    CHECK(bus.read16(0x2028) == 0xBABE);
    CHECK(cpu.gpr(4) == 0x2028);
    f.exec(stbux(6, 4, 5));
    CHECK(bus.read8(0x2030) == 0xBE);
    CHECK(cpu.gpr(4) == 0x2030);

    // Byte-reversed.
    cpu.set_gpr(4, 0x1000);
    cpu.set_gpr(5, 4);
    f.exec(lwbrx(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x44332211);
    f.exec(lhbrx(3, 4, 5));
    CHECK(cpu.gpr(3) == 0x2211);
    cpu.set_gpr(4, 0x2100);
    f.exec(stwbrx(6, 4, 5));
    CHECK(bus.read32(0x2104) == 0xBEBAFECA);
    f.exec(sthbrx(6, 4, 0));  // rB = r0 = 0x500
    CHECK(bus.read16(0x2600) == 0xBEBA);

    // lmw/stmw move rD..r31.
    for (std::uint32_t i = 0; i < 3; ++i) bus.write32(0x3800 + 4 * i, 0xA0 + i);
    cpu.set_gpr(4, 0x3800);
    f.exec(lmw(29, 0, 4));
    CHECK(cpu.gpr(29) == 0xA0);
    CHECK(cpu.gpr(30) == 0xA1);
    CHECK(cpu.gpr(31) == 0xA2);
    cpu.set_gpr(30, 0x55);
    f.exec(stmw(29, 0x20, 4));
    CHECK(bus.read32(0x3820) == 0xA0);
    CHECK(bus.read32(0x3824) == 0x55);
    CHECK(bus.read32(0x3828) == 0xA2);
}

TEST_CASE("lswi, lswx, stswi and stswx move strings of bytes") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    for (std::uint32_t i = 0; i < 40; ++i) bus.write8(0x1000 + i, static_cast<std::uint8_t>(i + 1));
    cpu.set_gpr(4, 0x1000);

    // 6 bytes: r5 gets 4, r6 gets 2 in its high bytes, the rest zeroed.
    cpu.set_gpr(6, 0xFFFFFFFF);
    f.exec(lswi(5, 4, 6));
    CHECK(cpu.gpr(5) == 0x01020304);
    CHECK(cpu.gpr(6) == 0x05060000);

    // Register numbers wrap from r31 to r0. NB = 0 means 32 bytes (r30, r31, r0..r5).
    cpu.set_gpr(9, 0x1000);
    f.exec(lswi(30, 9, 0));
    CHECK(cpu.gpr(30) == 0x01020304);
    CHECK(cpu.gpr(31) == 0x05060708);
    CHECK(cpu.gpr(0) == 0x090A0B0C);
    CHECK(cpu.gpr(5) == 0x1D1E1F20);
    cpu.set_gpr(4, 0x1000);

    // lswx takes the count from XER[25-31] and the address from (rA|0) + rB.
    cpu.set_xer(3);
    cpu.set_gpr(7, 2);
    f.exec(lswx(8, 4, 7));
    CHECK(cpu.gpr(8) == 0x03040500);

    cpu.set_gpr(10, 0xA1A2A3A4);
    cpu.set_gpr(11, 0xB1B2B3B4);
    cpu.set_gpr(4, 0x2000);
    f.exec(stswi(10, 4, 5));
    CHECK(bus.read32(0x2000) == 0xA1A2A3A4);
    CHECK(bus.read8(0x2004) == 0xB1);
    CHECK(bus.read8(0x2005) == 0x00);
    cpu.set_xer(6);
    f.exec(stswx(10, 4, 7));
    CHECK(bus.read32(0x2002) == 0xA1A2A3A4);
    CHECK(bus.read16(0x2006) == 0xB1B2);
    CHECK(bus.read8(0x2008) == 0x00);
}

TEST_CASE("lwarx and stwcx. use the reservation") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    bus.write32(0x1000, 41);
    cpu.set_gpr(4, 0x1000);
    cpu.set_gpr(6, 42);

    // Without a reservation the store fails: CR0 = SO copy only.
    cpu.set_xer(kSo);
    f.exec(stwcx_(6, 0, 4));
    CHECK(bus.read32(0x1000) == 41);
    CHECK(cr_field(cpu, 0) == kSoBit);

    f.exec(lwarx(3, 0, 4));
    CHECK(cpu.gpr(3) == 41);
    f.exec(stwcx_(6, 0, 4));
    CHECK(bus.read32(0x1000) == 42);
    CHECK(cr_field(cpu, 0) == (kEq | kSoBit));
    // The reservation is used up.
    cpu.set_gpr(6, 43);
    f.exec(stwcx_(6, 0, 4));
    CHECK(bus.read32(0x1000) == 42);
    CHECK(cr_field(cpu, 0) == kSoBit);

    // stwcx. to another address than lwarx's stores nothing and keeps the reservation
    // (Dolphin's stwcxd compares the address).
    bus.write32(0x1004, 7);
    f.exec(lwarx(3, 0, 4));
    cpu.set_gpr(5, 0x1004);
    f.exec(stwcx_(6, 0, 5));
    CHECK(bus.read32(0x1004) == 7);
    CHECK(cr_field(cpu, 0) == kSoBit);
    f.exec(stwcx_(6, 0, 4));
    CHECK(bus.read32(0x1000) == 43);
    CHECK(cr_field(cpu, 0) == (kEq | kSoBit));
}

TEST_CASE("invalid forms with a known result run like the hardware") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    bus.write32(0x1008, 0x11223344);
    // lwzu with rA = rD: rA gets the EA (written last; Dolphin's lwzu).
    cpu.set_gpr(4, 0x1000);
    f.exec(lwzu(4, 8, 4));
    CHECK(cpu.gpr(4) == 0x1008);
    // stwcx with Rc = 0 behaves as stwcx. (the table slot ignores Rc): CR0 is set.
    f.exec(lwarx(3, 0, 4));
    cpu.set_gpr(6, 99);
    cpu.set_cr(0);
    f.exec(x_form(31, 6, 0, 4, 150, false));
    CHECK(bus.read32(0x1008) == 99);
    CHECK(cr_field(cpu, 0) == kEq);
    // lmw loading rA: rA ends with the loaded word (the EA was computed first).
    bus.write32(0x1100, 0xAAAA0000);
    bus.write32(0x1104, 0xBBBB0000);
    cpu.set_gpr(30, 0x1100);
    f.exec(lmw(30, 0, 30));
    CHECK(cpu.gpr(30) == 0xAAAA0000);
    CHECK(cpu.gpr(31) == 0xBBBB0000);
}

TEST_CASE("cache and synchronization instructions") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    for (std::uint32_t i = 0; i < 0x60; i += 4) bus.write32(0x1000 + i, 0xFFFFFFFF);
    cpu.set_gpr(4, 0x1000);
    cpu.set_gpr(5, 0x2C);

    // dcbz clears the 32-byte line holding the address (with the data cache on, as the IPL
    // leaves it).
    cpu.set_spr(ezy::gc::spr::kHid0, Gekko::kHid0Dce);
    f.exec(dcbz(4, 5));
    for (std::uint32_t i = 0x20; i < 0x40; i += 4) CHECK(bus.read32(0x1000 + i) == 0);
    CHECK(bus.read32(0x101C) == 0xFFFFFFFF);
    CHECK(bus.read32(0x1040) == 0xFFFFFFFF);

    // The rest have no effect on memory or registers here (no caches are modeled).
    f.load({dcbf(4, 5), dcbst(4, 5), dcbt(4, 5), dcbtst(4, 5), dcbi(4, 5), icbi(4, 5), sync(), isync(), eieio()});
    f.run(9);
    CHECK(cpu.pc() == GekkoFixture::kProgram + 36);
    CHECK(bus.read32(0x1000) == 0xFFFFFFFF);
}
