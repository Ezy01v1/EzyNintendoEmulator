#include <doctest/doctest.h>

#include <bit>
#include <cstdint>

#include "mips_asm.hpp"
#include "n64/cpu/cop0.hpp"

using namespace mips;
using ezy::n64::Cop0;
using ezy::n64::ExceptionCode;

namespace {

constexpr int t0 = 8, t1 = 9, t2 = 10;
constexpr int kS = 16, kD = 17, kW = 20;

constexpr std::uint32_t mtc1(int rt, int fs) { return r(0x11, 4, rt, fs, 0, 0); }
constexpr std::uint32_t mfc1(int rt, int fs) { return r(0x11, 0, rt, fs, 0, 0); }
constexpr std::uint32_t dmtc1(int rt, int fs) { return r(0x11, 5, rt, fs, 0, 0); }
constexpr std::uint32_t dmfc1(int rt, int fs) { return r(0x11, 1, rt, fs, 0, 0); }
constexpr std::uint32_t ctc1(int rt, int fs) { return r(0x11, 6, rt, fs, 0, 0); }
constexpr std::uint32_t cfc1(int rt, int fs) { return r(0x11, 2, rt, fs, 0, 0); }
constexpr std::uint32_t fop(int fmt, int ft, int fs, int fd, int funct) { return r(0x11, fmt, ft, fs, fd, funct); }
constexpr std::uint32_t bc1t(std::int32_t words) { return i(0x11, 8, 1, words); }

// FCR31 bits.
constexpr std::uint32_t kCauseI = 1U << 12, kCauseZ = 1U << 15, kCauseE = 1U << 17, kFlagZ = 1U << 5,
                        kFlagI = 1U << 2, kEnableI = 1U << 7;

// Kernel mode with COP1 usable; fr selects the 64-bit register mode.
void fpu_ready(CpuFixture& f, bool fr) { f.cpu.cop0().write(Cop0::kStatus, Cop0::kCU1 | (fr ? Cop0::kFR : 0)); }

std::uint32_t exc_code(CpuFixture& f) { return static_cast<std::uint32_t>(f.cpu.cop0().read(Cop0::kCause) >> 2) & 31; }

}  // namespace

TEST_CASE("ADD.S rounds by FCR31.RM") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, std::bit_cast<std::uint32_t>(1.0f));
    f.cpu.set_gpr(t1, std::bit_cast<std::uint32_t>(0x1p-24f));  // half an ulp of 1.0
    f.cpu.set_gpr(t2, 2);                                          // RM = toward +infinity
    f.load({mtc1(t0, 1), mtc1(t1, 2), fop(kS, 2, 1, 3, 0x00), mfc1(t0, 3), ctc1(t2, 31), fop(kS, 2, 1, 4, 0x00),
            mfc1(t1, 4)});
    f.run(7);
    CHECK(static_cast<std::uint32_t>(f.cpu.gpr(t0)) == std::bit_cast<std::uint32_t>(1.0f));  // ties to even
    CHECK(static_cast<std::uint32_t>(f.cpu.gpr(t1)) == std::bit_cast<std::uint32_t>(1.0f) + 1);
}

TEST_CASE("FR=0 pairs registers for doubles") {
    CpuFixture f;
    fpu_ready(f, false);
    f.cpu.set_gpr(t0, 0x11223344'55667788ULL);
    f.load({dmtc1(t0, 2), mfc1(t1, 2), mfc1(t2, 3)});
    f.run(3);
    CHECK(f.cpu.gpr(t1) == 0x55667788);
    CHECK(f.cpu.gpr(t2) == 0x11223344);
}

TEST_CASE("MTC1 to an odd register with FR=0 writes the high half") {
    CpuFixture f;
    fpu_ready(f, false);
    f.cpu.set_gpr(t0, 0xAABBCCDD);
    f.cpu.set_gpr(t1, 0x01020304);
    f.load({mtc1(t0, 5), mtc1(t1, 4), dmfc1(t2, 4)});
    f.run(3);
    CHECK(f.cpu.gpr(t2) == 0xAABBCCDD'01020304ULL);
}

TEST_CASE("DIV.S by zero sets the Z cause and flag") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, std::bit_cast<std::uint32_t>(1.0f));
    f.load({mtc1(t0, 1), mtc1(0, 2), fop(kS, 2, 1, 3, 0x03), mfc1(t1, 3), cfc1(t2, 31)});
    f.run(5);
    CHECK(static_cast<std::uint32_t>(f.cpu.gpr(t1)) == 0x7F800000);  // +infinity
    CHECK((f.cpu.gpr(t2) & kCauseZ) != 0);
    CHECK((f.cpu.gpr(t2) & kFlagZ) != 0);
}

TEST_CASE("enabled inexact raises FPE and leaves fd unchanged") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, std::bit_cast<std::uint32_t>(1.0f));
    f.cpu.set_gpr(t1, std::bit_cast<std::uint32_t>(3.0f));
    f.cpu.set_gpr(t2, kEnableI);
    f.load({mtc1(t0, 1), mtc1(t1, 2), mtc1(0, 3), ctc1(t2, 31), fop(kS, 2, 1, 3, 0x03)});
    f.run(5);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::FPE));
    CHECK(f.cpu.cop1().fpr_bits(3) == 0);
    CHECK((f.cpu.cop1().fcr31() & kCauseI) != 0);
    CHECK((f.cpu.cop1().fcr31() & kFlagI) == 0);
}

TEST_CASE("denormal operand raises unimplemented operation") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, 1);  // the smallest single denormal
    f.load({mtc1(t0, 1), mtc1(t0, 2), fop(kS, 2, 1, 3, 0x00)});
    f.run(3);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::FPE));
    CHECK((f.cpu.cop1().fcr31() & kCauseE) != 0);
}

TEST_CASE("CVT.W.S out of range raises unimplemented operation") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, std::bit_cast<std::uint32_t>(3.0e9f));
    f.load({mtc1(t0, 1), fop(kS, 0, 1, 2, 0x24)});
    f.run(2);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::FPE));
    CHECK((f.cpu.cop1().fcr31() & kCauseE) != 0);
}

TEST_CASE("CVT.W.S and CVT.S.W convert in range") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, std::bit_cast<std::uint32_t>(-7.0f));
    f.load({mtc1(t0, 1), fop(kS, 0, 1, 2, 0x24), mfc1(t1, 2), fop(kW, 0, 2, 3, 0x20), mfc1(t2, 3)});
    f.run(5);
    CHECK(f.cpu.gpr(t1) == 0xFFFFFFFFFFFFFFF9ULL);
    CHECK(static_cast<std::uint32_t>(f.cpu.gpr(t2)) == std::bit_cast<std::uint32_t>(-7.0f));
}

TEST_CASE("C.LT.D then BC1T branches") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, std::bit_cast<std::uint64_t>(1.5));
    f.cpu.set_gpr(t1, std::bit_cast<std::uint64_t>(2.5));
    // 0: dmtc1  4: dmtc1  8: c.lt.d f1,f2  12: bc1t +2 (to 24)  16: slot  20: skipped  24: target
    f.load({dmtc1(t0, 1), dmtc1(t1, 2), fop(kD, 2, 1, 0, 0x3C), bc1t(2), addiu(t2, 0, 1), addiu(t2, 0, 2),
            addiu(t0, 0, 9)});
    f.run(6);
    CHECK(f.cpu.gpr(t2) == 1);
    CHECK(f.cpu.gpr(t0) == 9);
    CHECK((f.cpu.cop1().fcr31() & (1U << 23)) != 0);
}

TEST_CASE("CTC1 with an enabled cause bit raises FPE") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, kCauseZ | (1U << 10));  // Z cause + Z enable
    f.load({ctc1(t0, 31)});
    f.run(1);
    CHECK(exc_code(f) == static_cast<std::uint32_t>(ExceptionCode::FPE));
}

// PeterLemon FPUCompare. MIPS (pre-2008) NaNs: the top fraction bit *set* means
// signalling, so 0x7FBFFFFF (the VR4300's default NaN) is quiet.
TEST_CASE("compare with a quiet NaN: C.EQ.S is false and quiet, C.LT.S signals invalid") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, 0x7FBFFFFF);
    f.cpu.set_gpr(t1, std::bit_cast<std::uint32_t>(1.0f));
    f.load({mtc1(t0, 1), mtc1(t1, 2), fop(kS, 2, 1, 0, 0x32), cfc1(t2, 31), fop(kS, 2, 1, 0, 0x3C), cfc1(t0, 31)});
    f.run(6);
    CHECK(f.cpu.gpr(t2) == 0);                    // C.EQ.S: false, no flags
    CHECK(f.cpu.gpr(t0) == ((1U << 16) | (1U << 6)));  // C.LT.S: V cause and flag, no trap
}

TEST_CASE("compare with a signalling NaN signals invalid even for C.EQ.S") {
    CpuFixture f;
    fpu_ready(f, true);
    f.cpu.set_gpr(t0, 0x7FC00000);  // top fraction bit set: signalling on MIPS
    f.load({mtc1(t0, 1), mtc1(t0, 2), fop(kS, 2, 1, 0, 0x32), cfc1(t2, 31)});
    f.run(4);
    CHECK((f.cpu.gpr(t2) & (1U << 16)) != 0);
}

TEST_CASE("FCR0 reads the VR4300 implementation number") {
    CpuFixture f;
    fpu_ready(f, true);
    f.load({cfc1(t0, 0)});
    f.run(1);
    CHECK(f.cpu.gpr(t0) == 0xA00);
}
