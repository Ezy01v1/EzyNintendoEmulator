#include <doctest/doctest.h>

#include <bit>
#include <cstdint>

#include "ppc_asm.hpp"

using namespace ppc;
using ezy::gc::Gekko;
using ezy::gc::GekkoError;
namespace spr = ezy::gc::spr;

namespace {

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }
double to_single(double value) { return static_cast<double>(static_cast<float>(value)); }

// GQR fields (750CL manual; Dolphin's UGQR): store type bits 0-2, store scale 8-13, load
// type 16-18, load scale 24-29 (bit 0 = LSB). Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16.
// Scales are signed 6-bit.
constexpr std::uint32_t gqr(int ld_type, int ld_scale, int st_type, int st_scale) {
    return (static_cast<std::uint32_t>(ld_scale & 63) << 24) | (static_cast<std::uint32_t>(ld_type & 7) << 16) |
           (static_cast<std::uint32_t>(st_scale & 63) << 8) | static_cast<std::uint32_t>(st_type & 7);
}
constexpr int kFloat = 0, kU8 = 4, kU16 = 5, kS8 = 6, kS16 = 7;

// MSR[FP] on, translation off, HID2 as the IPL leaves it (LSQE, WPE, PSE).
struct PsFixture : GekkoFixture {
    PsFixture() {
        cpu.set_msr(Gekko::kMsrFp);
        cpu.set_spr(spr::kHid2, Gekko::kHid2Lsqe | Gekko::kHid2Wpe | Gekko::kHid2Pse);
    }
    void check(int reg, double ps0, double ps1) {
        CAPTURE(reg);
        CHECK(cpu.ps0(reg) == ps0);
        CHECK(cpu.ps1(reg) == ps1);
    }
};

std::uint32_t cr_field(const Gekko& cpu, int field) { return (cpu.cr() >> (28 - 4 * field)) & 0xF; }

}  // namespace

TEST_CASE("ps_add, ps_madd, ps_sum0/1, ps_muls0/1, ps_merge00..11 work lane by lane") {
    PsFixture f;
    auto& cpu = f.cpu;
    cpu.set_ps(1, 1.5, -2.0);
    cpu.set_ps(2, 0.25, 4.0);
    cpu.set_ps(3, 2.0, 3.0);

    f.exec(ps_add(4, 1, 2));
    f.check(4, 1.75, 2.0);
    f.exec(ps_sub(4, 1, 2));
    f.check(4, 1.25, -6.0);
    f.exec(ps_mul(4, 1, 3));
    f.check(4, 3.0, -6.0);
    f.exec(ps_div(4, 1, 3));
    f.check(4, 0.75, to_single(-2.0 / 3.0));
    f.exec(ps_madd(5, 1, 3, 2));
    f.check(5, 3.25, -2.0);
    f.exec(ps_msub(5, 1, 3, 2));
    f.check(5, 2.75, -10.0);
    f.exec(ps_nmadd(5, 1, 3, 2));
    f.check(5, -3.25, 2.0);
    f.exec(ps_nmsub(5, 1, 3, 2));
    f.check(5, -2.75, 10.0);
    // ps_sum0: ps0 = frA.ps0 + frB.ps1, ps1 = frC.ps1; ps_sum1: ps0 = frC.ps0, ps1 = the sum.
    f.exec(ps_sum0(6, 1, 3, 2));
    f.check(6, 5.5, 3.0);
    f.exec(ps_sum1(7, 1, 3, 2));
    f.check(7, 2.0, 5.5);
    // ps_muls0/1: both lanes times frC.ps0 / frC.ps1; ps_madds0/1 add frB lane by lane.
    f.exec(ps_muls0(8, 1, 3));
    f.check(8, 3.0, -4.0);
    f.exec(ps_muls1(9, 1, 3));
    f.check(9, 4.5, -6.0);
    f.exec(ps_madds0(10, 1, 3, 2));
    f.check(10, 3.25, 0.0);
    f.exec(ps_madds1(10, 1, 3, 2));
    f.check(10, 4.75, -2.0);
    // Merges pick a lane of frA and a lane of frB; the destination may be a source.
    f.exec(ps_merge00(11, 1, 2));
    f.check(11, 1.5, 0.25);
    f.exec(ps_merge01(11, 1, 2));
    f.check(11, 1.5, 4.0);
    f.exec(ps_merge10(11, 1, 2));
    f.check(11, -2.0, 0.25);
    f.exec(ps_merge11(11, 1, 2));
    f.check(11, -2.0, 4.0);
    cpu.set_ps(12, 7.0, 8.0);
    f.exec(ps_merge10(12, 12, 2));
    f.check(12, 8.0, 0.25);

    // Both lanes round to single.
    cpu.set_ps(13, 1.0 / 3.0, 2.0 / 3.0);
    cpu.set_ps(14, 0.0, 0.0);
    f.exec(ps_add(15, 13, 14));
    f.check(15, to_single(1.0 / 3.0), to_single(2.0 / 3.0));
    // ps_sum0's copied lane is rounded too.
    f.exec(ps_sum0(15, 14, 13, 14));
    f.check(15, 0.0, to_single(2.0 / 3.0));
    // FPRF follows ps0 (ps1 for ps_sum1).
    cpu.set_ps(16, -1.0, 1.0);
    f.exec(ps_add(17, 16, 14));
    CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == (0x08u << 12));
    f.exec(ps_sum1(17, 14, 16, 16));  // ps1 = 0 + 1
    CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == (0x04u << 12));
}

TEST_CASE("paired moves, selects, compares and estimates") {
    PsFixture f;
    auto& cpu = f.cpu;
    cpu.set_ps(1, 1.5, -2.0);
    cpu.set_ps(2, 0.25, 4.0);
    cpu.set_ps(3, -0.0, -1.0);
    f.exec(ps_neg(4, 1));
    f.check(4, -1.5, 2.0);
    f.exec(ps_abs(4, 1));
    f.check(4, 1.5, 2.0);
    f.exec(ps_nabs(4, 1));
    f.check(4, -1.5, -2.0);
    f.exec(ps_mr(4, 2));
    f.check(4, 0.25, 4.0);
    // ps_sel by lane: frA >= 0 (-0 included) picks frC, else frB.
    f.exec(ps_sel(5, 3, 1, 2));
    f.check(5, 1.5, 4.0);

    f.exec(ps_cmpu0(1, 1, 2));
    CHECK(cr_field(cpu, 1) == 4);  // 1.5 > 0.25
    f.exec(ps_cmpo1(2, 1, 2));
    CHECK(cr_field(cpu, 2) == 8);  // -2 < 4
    f.exec(ps_cmpu1(3, 2, 2));
    CHECK(cr_field(cpu, 3) == 2);
    f.exec(ps_cmpo0(4, 2, 1));
    CHECK(cr_field(cpu, 4) == 8);
    CHECK(((cpu.fpscr() >> 12) & 0xF) == 8);

    // Estimates per lane (Dolphin's Common/FloatUtils.cpp tables).
    cpu.set_ps(6, 2.0, -2.0);
    f.exec(ps_res(7, 6));
    f.check(7, 0.49993896484375, -0.49993896484375);
    cpu.set_ps(6, 4.0, 1.0);
    f.exec(ps_rsqrte(7, 6));
    f.check(7, 0.499908447265625, 0.99981689453125);
}

TEST_CASE("psq_l and psq_st quantize u8, s8, u16, s16 with scale, saturating on store") {
    PsFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    cpu.set_gpr(4, 0x1000);

    struct StoreCase {
        int type, scale;
        double ps0, ps1;
        std::uint32_t expected;  // the bytes written, big-endian
        int size;                // bytes per value
    };
    const StoreCase stores[] = {
        {kU8, 0, 300.0, -5.0, 0xFF00, 1},         // saturates at 255 and 0
        {kU8, 0, 2.9, 254.99, 0x02FE, 1},         // truncates toward zero
        {kS8, 0, 300.0, -300.0, 0x7F80, 1},       // 127 and -128
        {kS8, 3, -1.3, 2.0, 0xF610, 1},           // x 8: -10.4 -> -10, 16
        {kU16, 2, 1.5, 70000.0, 0x0006FFFF, 2},   // x 4: 6, saturates at 65535
        {kS16, -1, -7.0, 100.0, 0xFFFD0032, 2},   // x 1/2: -3.5 -> -3, 50
        {kS16, 0, -40000.0, 40000.0, 0x80007FFF, 2},
    };
    for (const StoreCase& c : stores) {
        CAPTURE(c.type);
        CAPTURE(c.scale);
        cpu.set_gqr(1, gqr(0, 0, c.type, c.scale));
        cpu.set_ps(1, c.ps0, c.ps1);
        bus.write32(0x1000, 0xAAAAAAAA);
        bus.write32(0x1004, 0xAAAAAAAA);
        f.exec(psq_st(1, 0, 4, 0, 1));
        if (c.size == 1) {
            CHECK(bus.read16(0x1000) == c.expected);
            CHECK(bus.read8(0x1002) == 0xAA);
        } else {
            CHECK(bus.read32(0x1000) == c.expected);
            CHECK(bus.read8(0x1004) == 0xAA);
        }
    }

    struct LoadCase {
        int type, scale;
        std::uint32_t data;  // big-endian, at 0x1000
        double ps0, ps1;
    };
    const LoadCase loads[] = {
        {kS8, 1, 0x807F0000, -64.0, 63.5},
        {kU8, 0, 0xFF010000, 255.0, 1.0},
        {kU16, 8, 0x0100FFFF, 1.0, 255.99609375},
        {kS16, -2, 0xFFFF0003, -4.0, 12.0},
        {kS16, 0, 0x80007FFF, -32768.0, 32767.0},
    };
    for (const LoadCase& c : loads) {
        CAPTURE(c.type);
        CAPTURE(c.scale);
        cpu.set_gqr(2, gqr(c.type, c.scale, 0, 0));
        bus.write32(0x1000, c.data);
        cpu.set_ps(2, 0.0, 0.0);
        f.exec(psq_l(2, 0, 4, 0, 2));
        f.check(2, c.ps0, c.ps1);
    }

    // Type 0 is a pair of singles, unscaled whatever the scale field says.
    cpu.set_gqr(3, gqr(kFloat, 5, kFloat, 5));
    bus.write32(0x1000, 0x3F800000);
    bus.write32(0x1004, 0xC0000000);
    f.exec(psq_l(3, 0, 4, 0, 3));
    f.check(3, 1.0, -2.0);
    cpu.set_ps(3, 0.5, -0.25);
    f.exec(psq_st(3, 8, 4, 0, 3));
    CHECK(bus.read32(0x1008) == 0x3F000000);
    CHECK(bus.read32(0x100C) == 0xBE800000);
    // A double too small for a normal single stores as zero (Dolphin's ConvertToSingleFTZ).
    cpu.set_ps(3, -1.0e-40, 1.0e-300);
    f.exec(psq_st(3, 8, 4, 0, 3));
    CHECK(bus.read32(0x1008) == 0x80000000);
    CHECK(bus.read32(0x100C) == 0);

    // A store of NaN quantizes to 0.
    cpu.set_gqr(1, gqr(0, 0, kS16, 0));
    cpu.set_ps(1, std::bit_cast<double>(0x7FF8000000000000ull), 1.0);
    f.exec(psq_st(1, 0, 4, 0, 1));
    CHECK(bus.read32(0x1000) == 0x00000001);
}

TEST_CASE("psq_l with W=1 sets ps1 to 1.0") {
    PsFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    cpu.set_gpr(4, 0x1000);
    bus.write32(0x1000, 0x80FF0000);
    cpu.set_gqr(2, gqr(kU8, 0, kU8, 0));
    cpu.set_ps(1, 5.0, 6.0);
    f.exec(psq_l(1, 0, 4, 1, 2));
    f.check(1, 128.0, 1.0);
    cpu.set_gqr(2, gqr(kFloat, 0, kFloat, 0));
    bus.write32(0x1000, 0x40400000);  // 3.0f
    f.exec(psq_lx(1, 0, 4, 1, 2));
    f.check(1, 3.0, 1.0);

    // A W=1 store writes ps0 only.
    bus.write32(0x1010, 0xAAAAAAAA);
    bus.write32(0x1014, 0xAAAAAAAA);
    cpu.set_gqr(5, gqr(0, 0, kU8, 0));
    cpu.set_ps(7, 9.0, 10.0);
    f.exec(psq_st(7, 0x10, 4, 1, 5));
    CHECK(bus.read32(0x1010) == 0x09AAAAAA);
    cpu.set_gqr(5, gqr(0, 0, kFloat, 0));
    f.exec(psq_st(7, 0x10, 4, 1, 5));
    CHECK(bus.read32(0x1010) == 0x41100000);
    CHECK(bus.read32(0x1014) == 0xAAAAAAAA);
}

TEST_CASE("quantized load and store forms: update, indexed, offsets") {
    PsFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    cpu.set_gqr(0, gqr(kS16, 0, kS16, 0));
    bus.write32(0x1000, 0x00010002);
    bus.write32(0x1004, 0xFFFF0005);
    cpu.set_gpr(4, 0x1004);
    cpu.set_gpr(5, 4);

    // The D-form offset is 12 bits, signed.
    f.exec(psq_l(1, -4, 4, 0, 0));
    f.check(1, 1.0, 2.0);
    f.exec(psq_lu(2, -4, 4, 0, 0));
    f.check(2, 1.0, 2.0);
    CHECK(cpu.gpr(4) == 0x1000);
    f.exec(psq_lx(3, 4, 5, 0, 0));
    f.check(3, -1.0, 5.0);
    f.exec(psq_lux(3, 4, 5, 1, 0));
    f.check(3, -1.0, 1.0);
    CHECK(cpu.gpr(4) == 0x1004);
    // rA = 0 reads as zero in the non-update forms.
    cpu.set_gpr(0, 0x500);
    cpu.set_gpr(6, 0x1000);
    f.exec(psq_lx(3, 0, 6, 0, 0));
    f.check(3, 1.0, 2.0);

    cpu.set_ps(4, 7.0, -8.0);
    f.exec(psq_stu(4, 0x7FC, 4, 0, 0));
    CHECK(cpu.gpr(4) == 0x1800);
    CHECK(bus.read32(0x1800) == 0x0007FFF8);
    f.exec(psq_stx(4, 4, 5, 1, 0));
    CHECK(bus.read16(0x1804) == 0x0007);
    f.exec(psq_stux(4, 4, 5, 0, 0));
    CHECK(cpu.gpr(4) == 0x1804);
    CHECK(bus.read32(0x1804) == 0x0007FFF8);
    // An unaligned pair is fine (the access is split).
    cpu.set_gqr(0, gqr(kFloat, 0, kFloat, 0));
    f.exec(psq_st(4, 1, 4, 0, 0));
    CHECK(bus.read32(0x1805) == 0x40E00000);
    CHECK(bus.read32(0x1809) == 0xC1000000);
    f.exec(psq_l(5, 1, 4, 0, 0));
    f.check(5, 7.0, -8.0);
}

TEST_CASE("psq_l and psq_st need HID2[LSQE]; the indexed forms do not") {
    PsFixture f;
    auto& cpu = f.cpu;
    cpu.set_spr(spr::kHid2, Gekko::kHid2Pse);
    cpu.set_gpr(4, 0x1000);
    cpu.set_ps(1, 2.0, 3.0);
    for (const std::uint32_t word : {psq_l(1, 0, 4, 0, 0), psq_lu(1, 0, 4, 0, 0), psq_st(1, 0, 4, 0, 0),
                                     psq_stu(1, 0, 4, 0, 0)}) {
        CAPTURE(word);
        cpu.set_msr(Gekko::kMsrFp);
        f.exec(word);
        CHECK(cpu.pc() == 0x700);
        CHECK((cpu.spr(spr::kSrr1) & Gekko::kProgramIllegal) != 0);
        f.check(1, 2.0, 3.0);
    }
    cpu.set_msr(Gekko::kMsrFp);
    f.exec(psq_lx(1, 0, 4, 0, 0));  // GQR0 = 0: two singles of 0
    CHECK(cpu.pc() == 0x3004);
    f.check(1, 0.0, 0.0);
}

TEST_CASE("a quantized access that faults leaves registers and memory alone") {
    PsFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    // Data translation on, one DBAT for 0x80000000 (128 KiB) -> physical 0.
    cpu.set_spr(spr::kDbat0u, 0x80000002);
    cpu.set_spr(spr::kDbat0l, 0x00000002);
    cpu.set_msr(Gekko::kMsrFp | Gekko::kMsrDr);
    cpu.set_gqr(1, gqr(kU16, 0, kU16, 0));
    cpu.set_ps(1, 2.0, 3.0);
    // A pair straddling the end of the BAT: the second half has no translation.
    cpu.set_gpr(4, 0x8001FFFE);
    f.exec(psq_lu(1, 0, 4, 0, 1));
    CHECK(cpu.pc() == 0x300);
    CHECK(cpu.spr(spr::kDar) == 0x80020000);
    f.check(1, 2.0, 3.0);
    CHECK(cpu.gpr(4) == 0x8001FFFE);

    cpu.set_msr(Gekko::kMsrFp | Gekko::kMsrDr);
    bus.write32(0x1FFFE, 0xAAAAAAAA);
    f.exec(psq_stux(1, 4, 0, 0, 1));
    CHECK(cpu.pc() == 0x300);
    CHECK((cpu.spr(spr::kDsisr) & Gekko::kDsisrStore) != 0);
    CHECK(bus.read32(0x1FFFE) == 0xAAAAAAAA);
    CHECK(cpu.gpr(4) == 0x8001FFFE);
}

TEST_CASE("a reserved GQR type stops emulation") {
    PsFixture f;
    auto& cpu = f.cpu;
    cpu.set_gpr(4, 0x1000);
    cpu.set_gqr(1, gqr(2, 0, 0, 0));
    f.load({psq_l(1, 0, 4, 0, 1)});
    CHECK_THROWS_AS(f.run(1), GekkoError);
    cpu.set_gqr(1, gqr(0, 0, 3, 0));
    f.load({psq_st(1, 0, 4, 0, 1)});
    CHECK_THROWS_AS(f.run(1), GekkoError);
}
