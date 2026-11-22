#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <limits>

#include "ppc_asm.hpp"

using namespace ppc;
using ezy::gc::Gekko;
namespace spr = ezy::gc::spr;

namespace {

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }
double from_bits(std::uint64_t value) { return std::bit_cast<double>(value); }
double single_bits(std::uint32_t value) { return static_cast<double>(std::bit_cast<float>(value)); }
// The value as the Gekko's single-precision rounding (to nearest) leaves it.
double to_single(double value) { return static_cast<double>(static_cast<float>(value)); }

constexpr std::uint64_t kQuietNan = 0x7FF8000000000000;
constexpr std::uint64_t kSignalingNan = 0x7FF4000000000000;
// FPRF classes (C, FL, FG, FE, FU), as FPSCR bits 15-19.
constexpr std::uint32_t kPositiveNormal = 0x04u << 12, kNegativeNormal = 0x08u << 12, kPositiveZero = 0x02u << 12,
                        kNegativeZero = 0x12u << 12, kQuietNanClass = 0x11u << 12, kPositiveInfinity = 0x05u << 12;

// MSR[FP] on, translation off.
struct FpFixture : GekkoFixture {
    FpFixture() { cpu.set_msr(Gekko::kMsrFp); }
    void rounding(std::uint32_t rn) { cpu.set_fpscr((cpu.fpscr() & ~Gekko::kFpscrRn) | rn); }
};

std::uint32_t cr_field(const Gekko& cpu, int field) { return (cpu.cr() >> (28 - 4 * field)) & 0xF; }

}  // namespace

TEST_CASE("fadds rounds to single and duplicates into ps1") {
    FpFixture f;
    auto& cpu = f.cpu;
    const double third = 1.0 / 3.0;
    cpu.set_ps(1, third, 0.0);
    cpu.set_ps(2, 0.0, 0.0);
    cpu.set_ps(3, 99.0, 99.0);
    f.exec(fadds(3, 1, 2));
    CHECK(cpu.ps0(3) == to_single(third));
    CHECK(cpu.ps1(3) == to_single(third));
    CHECK(cpu.ps0(3) != third);
    CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == kPositiveNormal);

    // The double form keeps every bit and leaves ps1 alone.
    cpu.set_ps(4, 7.0, 7.0);
    f.exec(fadd(4, 1, 2));
    CHECK(cpu.ps0(4) == third);
    CHECK(cpu.ps1(4) == 7.0);

    // Single rounding follows FPSCR[RN]: 1 + 2^-30 is 1 to nearest and toward zero, the next
    // single up toward +infinity; its negative goes down toward -infinity.
    const double tiny = 1.0 / 1073741824.0;  // 2^-30
    const double next_up = 1.0 + 1.0 / 8388608.0;  // 1 + 2^-23
    cpu.set_ps(5, 1.0, 1.0);
    cpu.set_ps(6, tiny, tiny);
    cpu.set_ps(7, -1.0, -1.0);
    cpu.set_ps(8, -tiny, -tiny);
    f.exec(fadds(9, 5, 6));
    CHECK(cpu.ps0(9) == 1.0);
    f.rounding(1);
    f.exec(fadds(9, 5, 6));
    CHECK(cpu.ps0(9) == 1.0);
    f.rounding(2);
    f.exec(fadds(9, 5, 6));
    CHECK(cpu.ps0(9) == next_up);
    CHECK(cpu.ps1(9) == next_up);
    f.exec(fadds(9, 7, 8));
    CHECK(cpu.ps0(9) == -1.0);
    f.rounding(3);
    f.exec(fadds(9, 7, 8));
    CHECK(cpu.ps0(9) == -next_up);
    f.exec(fadds(9, 5, 6));
    CHECK(cpu.ps0(9) == 1.0);
    // The host is back to round-to-nearest after each instruction.
    f.rounding(0);
    volatile double one = 1.0, small = tiny;
    CHECK(one + small * small == 1.0);
}

TEST_CASE("fmadd and fmadds fuse before rounding") {
    FpFixture f;
    auto& cpu = f.cpu;
    // Double: (1 + 2^-27)(1 - 2^-27) - 1 = -2^-54. A separate multiply rounds the product to
    // 1.0 (a tie, to even) and gives 0.
    const double e27 = 1.0 / 134217728.0;
    cpu.set_ps(1, 1.0 + e27, 0.0);
    cpu.set_ps(2, 1.0 - e27, 0.0);
    cpu.set_ps(3, -1.0, 0.0);
    f.exec(fmadd(4, 1, 2, 3));
    CHECK(cpu.ps0(4) == -(e27 * e27));
    CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == kNegativeNormal);
    f.exec(fmul(5, 1, 2));
    CHECK(cpu.ps0(5) == 1.0);
    f.exec(fadd(5, 5, 3));
    CHECK(cpu.ps0(5) == 0.0);
    // fmsub / fnmadd / fnmsub with the same operands.
    f.exec(fmsub(6, 1, 2, 3));  // a*c - b = 2 - 2^-54 -> 2 to nearest
    CHECK(cpu.ps0(6) == 2.0);
    f.exec(fnmadd(6, 1, 2, 3));
    CHECK(cpu.ps0(6) == e27 * e27);
    f.exec(fnmsub(6, 1, 2, 3));
    CHECK(cpu.ps0(6) == -2.0);

    // Single: (1 + 2^-20)(1 - 2^-20) - 1 = -2^-40, a single. fmuls then fadds gives 0.
    const double e20 = 1.0 / 1048576.0;
    cpu.set_ps(1, 1.0 + e20, 0.0);
    cpu.set_ps(2, 1.0 - e20, 0.0);
    f.exec(fmadds(7, 1, 2, 3));
    CHECK(cpu.ps0(7) == -(e20 * e20));
    CHECK(cpu.ps1(7) == -(e20 * e20));
    f.exec(fmuls(8, 1, 2));
    f.exec(fadds(8, 8, 3));
    CHECK(cpu.ps0(8) == 0.0);
    f.exec(fnmsubs(9, 1, 2, 3));  // -(a*c - b) = -(2 - 2^-40) -> -2 as a single
    CHECK(cpu.ps0(9) == -2.0);
    f.exec(fmsubs(9, 1, 2, 3));
    CHECK(cpu.ps1(9) == 2.0);
    f.exec(fnmadds(9, 1, 2, 3));
    CHECK(cpu.ps0(9) == e20 * e20);

    // fmadds rounds once: a double FMA rounded to single would give 0xBF55BF18 here (the
    // Mario Strikers Charged case in Dolphin's Interpreter_FPUtils.h).
    cpu.set_ps(1, single_bits(0x42480000), 0.0);
    cpu.set_ps(2, single_bits(0xBC88CC38), 0.0);
    cpu.set_ps(3, single_bits(0x1B1C72A0), 0.0);
    f.exec(fmadds(10, 1, 2, 3));
    CHECK(std::bit_cast<std::uint32_t>(static_cast<float>(cpu.ps0(10))) == 0xBF55BF17u);
}

TEST_CASE("single-precision multiplies round frC to 25 bits first") {
    FpFixture f;
    auto& cpu = f.cpu;
    // frC = 1 + 2^-25 (not a single) rounds up to 1 + 2^-24 before the multiply, so
    // 3 * frC = 3 + 3 * 2^-24 -> rounds to 3 + 2^-22 as a single. A plain double multiply
    // rounded to single would give 3.0.
    cpu.set_ps(1, 3.0, 0.0);
    cpu.set_ps(2, 1.0 + 1.0 / 33554432.0, 0.0);
    f.exec(fmuls(3, 1, 2));
    CHECK(cpu.ps0(3) == 3.0 + 1.0 / 4194304.0);
    f.exec(fmul(4, 1, 2));
    CHECK(cpu.ps0(4) == 3.0 * (1.0 + 1.0 / 33554432.0));
}

TEST_CASE("frsp, fctiw and fctiwz follow the FPSCR rounding mode") {
    FpFixture f;
    auto& cpu = f.cpu;
    // frsp of 1 + 2^-24 + 2^-30 (just above half a single ulp): up to nearest and +inf, down
    // toward zero and -inf.
    const double up = 1.0 + 1.0 / 8388608.0;
    cpu.set_ps(1, 1.0 + 1.0 / 16777216.0 + 1.0 / 1073741824.0, 0.0);
    cpu.set_ps(2, 5.0, 6.0);
    const double expected[4] = {up, 1.0, up, 1.0};
    for (std::uint32_t rn = 0; rn < 4; ++rn) {
        CAPTURE(rn);
        cpu.set_fpscr(rn);
        f.exec(frsp(2, 1));
        CHECK(cpu.ps0(2) == expected[rn]);
        CHECK(cpu.ps1(2) == expected[rn]);  // the single result is duplicated
        CHECK((cpu.fpscr() & Gekko::kFpscrFi) != 0);
        CHECK(((cpu.fpscr() & Gekko::kFpscrFr) != 0) == (expected[rn] == up));
        CHECK((cpu.fpscr() & Gekko::kFpscrXx) != 0);
        CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == kPositiveNormal);
    }
    // An exact frsp clears FI and FR.
    cpu.set_ps(1, 0.5, 0.0);
    f.exec(frsp(2, 1));
    CHECK((cpu.fpscr() & (Gekko::kFpscrFi | Gekko::kFpscrFr)) == 0);

    // fctiw by mode: nearest-even, toward zero, toward +inf, toward -inf.
    struct Case {
        double in;
        std::int32_t out[4];
    };
    const Case cases[] = {
        {2.5, {2, 2, 3, 2}},     {3.5, {4, 3, 4, 3}},     {-2.5, {-2, -2, -2, -3}},
        {3.7, {4, 3, 4, 3}},     {-3.7, {-4, -3, -3, -4}}, {-3.2, {-3, -3, -3, -4}},
        {7.0, {7, 7, 7, 7}},
    };
    for (const Case& c : cases) {
        for (std::uint32_t rn = 0; rn < 4; ++rn) {
            CAPTURE(c.in);
            CAPTURE(rn);
            cpu.set_fpscr(rn);
            cpu.set_ps(1, c.in, 0.0);
            cpu.set_ps(3, 0.0, 42.0);
            f.exec(fctiw(3, 1));
            CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == static_cast<std::uint32_t>(c.out[rn]));
            CHECK((bits(cpu.ps0(3)) >> 32) == 0xFFF80000u);  // the high word hardware leaves
            CHECK(cpu.ps1(3) == 42.0);
            CHECK(((cpu.fpscr() & Gekko::kFpscrFi) != 0) == (c.in != 7.0));
            // fctiwz truncates whatever RN says.
            f.exec(fctiwz(3, 1));
            CHECK(static_cast<std::int32_t>(bits(cpu.ps0(3))) == static_cast<std::int32_t>(c.in));
        }
    }

    // Saturation: too large, too small, NaN (VXCVI, and VXSNAN for a signaling NaN).
    cpu.set_fpscr(0);
    cpu.set_ps(1, 3.0e9, 0.0);
    f.exec(fctiw(3, 1));
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x7FFFFFFFu);
    CHECK((cpu.fpscr() & (Gekko::kFpscrVxcvi | Gekko::kFpscrVx | Gekko::kFpscrFx)) ==
          (Gekko::kFpscrVxcvi | Gekko::kFpscrVx | Gekko::kFpscrFx));
    cpu.set_ps(1, 2147483647.6, 0.0);  // rounds to 2^31: saturates
    f.exec(fctiw(3, 1));
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x7FFFFFFFu);
    f.exec(fctiwz(3, 1));  // truncates to 2^31 - 1: fits
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x7FFFFFFFu);
    cpu.set_ps(1, -3.0e9, 0.0);
    f.exec(fctiwz(3, 1));
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x80000000u);
    cpu.set_ps(1, -2147483648.0, 0.0);
    cpu.set_fpscr(0);
    f.exec(fctiw(3, 1));
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x80000000u);
    CHECK((cpu.fpscr() & Gekko::kFpscrVxcvi) == 0);
    cpu.set_ps(1, std::numeric_limits<double>::infinity(), 0.0);
    f.exec(fctiw(3, 1));
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x7FFFFFFFu);
    cpu.set_fpscr(0);
    cpu.set_ps(1, from_bits(kSignalingNan), 0.0);
    f.exec(fctiw(3, 1));
    CHECK(static_cast<std::uint32_t>(bits(cpu.ps0(3))) == 0x80000000u);
    CHECK((cpu.fpscr() & (Gekko::kFpscrVxcvi | Gekko::kFpscrVxsnan)) == (Gekko::kFpscrVxcvi | Gekko::kFpscrVxsnan));
    // -0 converts to 0 with bit 32 set (hardware result, Dolphin's ConvertToInteger).
    cpu.set_ps(1, -0.0, 0.0);
    f.exec(fctiwz(3, 1));
    CHECK(bits(cpu.ps0(3)) == 0xFFF8000100000000ull);
}

TEST_CASE("fcmpu and fcmpo set CR fields and FPCC, NaN unordered") {
    FpFixture f;
    auto& cpu = f.cpu;
    cpu.set_ps(1, 1.0, 0.0);
    cpu.set_ps(2, 2.0, 0.0);
    cpu.set_ps(3, from_bits(kQuietNan), 0.0);
    cpu.set_ps(4, from_bits(kSignalingNan), 0.0);
    cpu.set_ps(5, -0.0, 0.0);
    cpu.set_ps(6, 0.0, 0.0);

    f.exec(fcmpu(1, 1, 2));
    CHECK(cr_field(cpu, 1) == 8);  // FL
    CHECK(((cpu.fpscr() >> 12) & 0xF) == 8);
    f.exec(fcmpu(7, 2, 1));
    CHECK(cr_field(cpu, 7) == 4);  // FG
    CHECK(((cpu.fpscr() >> 12) & 0xF) == 4);
    f.exec(fcmpo(0, 5, 6));
    CHECK(cr_field(cpu, 0) == 2);  // -0 == +0
    CHECK(((cpu.fpscr() >> 12) & 0xF) == 2);
    CHECK(cpu.fpscr() == (2u << 12));  // no exception bits

    // A quiet NaN: unordered; fcmpu raises nothing, fcmpo raises VXVC.
    f.exec(fcmpu(2, 1, 3));
    CHECK(cr_field(cpu, 2) == 1);
    CHECK(((cpu.fpscr() >> 12) & 0xF) == 1);
    CHECK((cpu.fpscr() & Gekko::kFpscrAnyException) == 0);
    f.exec(fcmpo(2, 3, 1));
    CHECK(cr_field(cpu, 2) == 1);
    CHECK(cpu.fpscr() == (Gekko::kFpscrFx | Gekko::kFpscrVx | Gekko::kFpscrVxvc | (1u << 12)));

    // A signaling NaN: fcmpu raises VXSNAN; fcmpo VXSNAN and VXVC (VE = 0).
    cpu.set_fpscr(0);
    f.exec(fcmpu(3, 4, 1));
    CHECK(cr_field(cpu, 3) == 1);
    CHECK(cpu.fpscr() == (Gekko::kFpscrFx | Gekko::kFpscrVx | Gekko::kFpscrVxsnan | (1u << 12)));
    cpu.set_fpscr(0);
    f.exec(fcmpo(3, 1, 4));
    CHECK((cpu.fpscr() & (Gekko::kFpscrVxsnan | Gekko::kFpscrVxvc)) == (Gekko::kFpscrVxsnan | Gekko::kFpscrVxvc));
    // The C bit of FPRF is left alone by compares.
    cpu.set_fpscr(0x10000);
    f.exec(fcmpu(0, 1, 2));
    CHECK(((cpu.fpscr() >> 12) & 0x1F) == 0x18);
}

TEST_CASE("mtfsf, mtfsfi, mtfsb0/1 and mffs") {
    FpFixture f;
    auto& cpu = f.cpu;
    // mtfsfi: field 7 = RN and NI... (bits 28-31) = 3.
    f.exec(mtfsfi(7, 3));
    CHECK(cpu.fpscr() == 3);
    f.exec(mtfsb1(29));  // NI
    CHECK(cpu.fpscr() == 7);
    f.exec(mtfsb0(30));  // RN bit 0
    CHECK(cpu.fpscr() == 5);
    // Setting an exception bit sets FX and the VX summary.
    f.exec(mtfsb1(7, true));  // VXSNAN, recording CR1
    CHECK(cpu.fpscr() == (Gekko::kFpscrFx | Gekko::kFpscrVx | Gekko::kFpscrVxsnan | 5u));
    CHECK(cr_field(cpu, 1) == 0xA);  // FX, FEX, VX, OX
    // FEX and VX cannot be cleared by hand: they follow the other bits.
    f.exec(mtfsb0(2));
    CHECK((cpu.fpscr() & Gekko::kFpscrVx) != 0);
    f.exec(mtfsb0(7));
    CHECK((cpu.fpscr() & Gekko::kFpscrVx) == 0);
    CHECK(cpu.fpscr() == (Gekko::kFpscrFx | 5u));
    f.exec(mtfsb0(0));
    CHECK(cpu.fpscr() == 5);

    // mffs: the high word reads 0xFFF80000 (hardware; Dolphin's mffsx); ps1 is unchanged.
    cpu.set_ps(5, 1.0, 2.0);
    f.exec(mffs(5));
    CHECK(bits(cpu.ps0(5)) == 0xFFF8000000000005ull);
    CHECK(cpu.ps1(5) == 2.0);

    // mtfsf with every field: FEX and VX come from the other bits, not from frB.
    cpu.set_ps(6, from_bits(0x00000000400000F8ull), 0.0);
    f.exec(mtfsf(0xFF, 6));
    CHECK(cpu.fpscr() == 0xF8);
    cpu.set_ps(6, from_bits(0x0000000001000080ull), 0.0);  // VXSNAN with VE
    f.exec(mtfsf(0xFF, 6));
    CHECK(cpu.fpscr() == (Gekko::kFpscrFex | Gekko::kFpscrVx | 0x01000080u));
    // Only the fields in FM change (FM bit 0x01 = field 7).
    cpu.set_ps(6, from_bits(0x00000000FFFFFFF2ull), 0.0);
    f.exec(mtfsf(0x01, 6, true));
    CHECK(cpu.fpscr() == (Gekko::kFpscrFex | Gekko::kFpscrVx | 0x01000082u));
    CHECK(cr_field(cpu, 1) == 0x6);

    // mcrfs copies a field to CR and clears the exception bits it read.
    cpu.set_fpscr(Gekko::kFpscrFx | Gekko::kFpscrOx | Gekko::kFpscrVxisi | 1u);
    f.exec(mcrfs(4, 0));
    CHECK(cr_field(cpu, 4) == 0xB);  // FX, VX, OX (FEX = 0: no enables)
    CHECK(cpu.fpscr() == (Gekko::kFpscrVx | Gekko::kFpscrVxisi | 1u));
    f.exec(mcrfs(5, 2));  // field 2 = VXISI, VXIDI, VXZDZ, VXIMZ
    CHECK(cr_field(cpu, 5) == 0x8);
    CHECK(cpu.fpscr() == 1);

    // set_fpscr drops the reserved bit 20.
    cpu.set_fpscr(0x00000800);
    CHECK(cpu.fpscr() == 0);
}

TEST_CASE("floating-point loads and stores") {
    FpFixture f;
    auto& cpu = f.cpu;
    auto& bus = f.bus;
    bus.write32(0x1000, 0x3FC00000);  // 1.5f
    bus.write64(0x1008, bits(-2.25));
    cpu.set_gpr(4, 0x1000);
    cpu.set_gpr(5, 8);

    // lfs converts to double and duplicates; lfd sets ps0 only.
    cpu.set_ps(1, 0.0, 9.0);
    f.exec(lfs(1, 0, 4));
    CHECK(cpu.ps0(1) == 1.5);
    CHECK(cpu.ps1(1) == 1.5);
    cpu.set_ps(2, 0.0, 9.0);
    f.exec(lfd(2, 8, 4));
    CHECK(cpu.ps0(2) == -2.25);
    CHECK(cpu.ps1(2) == 9.0);
    f.exec(lfdx(3, 4, 5));
    CHECK(cpu.ps0(3) == -2.25);
    f.exec(lfsx(3, 0, 4));
    CHECK(cpu.ps1(3) == 1.5);

    // Update forms write rA = EA.
    f.exec(lfsu(6, 0x8, 4));
    CHECK(cpu.gpr(4) == 0x1008);
    CHECK(cpu.ps0(6) == single_bits(0xC0020000));  // the high word of -2.25 as a single
    f.exec(lfdu(6, -8, 4));
    CHECK(cpu.gpr(4) == 0x1000);
    CHECK(bits(cpu.ps0(6)) == ((std::uint64_t{0x3FC00000} << 32) | 0));
    f.exec(lfsux(7, 4, 5));
    CHECK(cpu.gpr(4) == 0x1008);
    f.exec(lfdux(7, 4, 5));
    CHECK(cpu.gpr(4) == 0x1010);
    cpu.set_gpr(4, 0x1000);

    // A single's bits survive the trip: a signaling NaN, a denormal.
    for (const std::uint32_t word : {0x7F800001u, 0x00000001u, 0x807FFFFFu, 0xFF800000u, 0x80000000u}) {
        CAPTURE(word);
        bus.write32(0x1100, word);
        cpu.set_gpr(6, 0x1100);
        f.exec(lfs(8, 0, 6));
        f.exec(stfs(8, 4, 6));
        CHECK(bus.read32(0x1104) == word);
    }
    // lfs of a single signaling NaN: the double keeps it signaling (PEM's load conversion).
    bus.write32(0x1100, 0x7F800001);
    f.exec(lfs(8, 0, 6));
    CHECK(bits(cpu.ps0(8)) == 0x7FF0000020000000ull);

    // stfs converts (truncating the extra double bits, no rounding); stfd stores the bits.
    cpu.set_ps(9, 1.0 + 1.0 / 16777216.0 + 1.0 / 1073741824.0, 0.0);
    f.exec(stfs(9, 0x200, 4));
    CHECK(bus.read32(0x1200) == 0x3F800000);
    f.exec(stfd(9, 0x208, 4));
    CHECK(bus.read64(0x1208) == bits(cpu.ps0(9)));
    // A double too small for a normal single is denormalized.
    cpu.set_ps(9, static_cast<double>(1.0e-40f), 0.0);
    f.exec(stfsx(9, 4, 5));
    CHECK(bus.read32(0x1008) == std::bit_cast<std::uint32_t>(1.0e-40f));
    // Update and indexed store forms.
    cpu.set_ps(10, 2.0, 0.0);
    f.exec(stfsu(10, 0x300, 4));
    CHECK(cpu.gpr(4) == 0x1300);
    CHECK(bus.read32(0x1300) == 0x40000000);
    f.exec(stfdu(10, 0x8, 4));
    CHECK(cpu.gpr(4) == 0x1308);
    CHECK(bus.read64(0x1308) == bits(2.0));
    f.exec(stfsux(10, 4, 5));
    CHECK(cpu.gpr(4) == 0x1310);
    f.exec(stfdux(10, 4, 5));
    CHECK(cpu.gpr(4) == 0x1318);
    CHECK(bus.read64(0x1318) == bits(2.0));
    f.exec(stfdx(10, 0, 4));
    CHECK(bus.read64(0x1318) == bits(2.0));
    // stfiwx stores the low word of ps0.
    cpu.set_ps(11, from_bits(0xFFF80000DEADBEEFull), 0.0);
    f.exec(stfiwx(11, 0, 4));
    CHECK(bus.read32(0x1318) == 0xDEADBEEF);
}

TEST_CASE("an unaligned floating-point load or store raises alignment") {
    FpFixture f;
    auto& cpu = f.cpu;
    cpu.set_msr(Gekko::kMsrFp | Gekko::kMsrMe);
    cpu.set_gpr(4, 0x1002);
    cpu.set_ps(1, 5.0, 6.0);
    f.exec(lfdu(1, 0, 4));
    CHECK(cpu.pc() == 0x600);
    CHECK(cpu.spr(spr::kSrr0) == 0x3000);
    CHECK(cpu.spr(spr::kDar) == 0x1002);
    // D-form: opcode 51 = 110011 -> bit 5 (1) in DSISR 17, bits 1-4 (1001) in 18-21.
    CHECK(cpu.spr(spr::kDsisr) == ((1u << 14) | (9u << 10) | (1u << 5) | 4u));
    CHECK(cpu.ps0(1) == 5.0);
    CHECK(cpu.gpr(4) == 0x1002);
    // A doubleword at a word boundary is fine.
    cpu.set_msr(Gekko::kMsrFp);
    cpu.set_gpr(4, 0x1004);
    f.exec(lfd(1, 0, 4));
    CHECK(cpu.pc() == 0x3004);
    cpu.set_gpr(4, 0x1001);
    f.exec(stfiwx(1, 0, 4));
    CHECK(cpu.pc() == 0x600);
}

TEST_CASE("double arithmetic, moves and fsel") {
    FpFixture f;
    auto& cpu = f.cpu;
    cpu.set_ps(1, 6.0, 1.0);
    cpu.set_ps(2, 1.5, 2.0);
    cpu.set_ps(3, -0.0, 3.0);
    f.exec(fsub(4, 1, 2));
    CHECK(cpu.ps0(4) == 4.5);
    f.exec(fmul(4, 1, 2));
    CHECK(cpu.ps0(4) == 9.0);
    f.exec(fdiv(4, 1, 2));
    CHECK(cpu.ps0(4) == 4.0);
    f.exec(fdiv(4, 1, 1));
    CHECK(cpu.ps0(4) == 1.0);
    f.exec(fdivs(4, 2, 1));
    CHECK(cpu.ps0(4) == 0.25);
    CHECK(cpu.ps1(4) == 0.25);
    f.exec(fsubs(4, 2, 1));
    CHECK(cpu.ps1(4) == -4.5);
    f.exec(fsub(4, 2, 2));
    CHECK(bits(cpu.ps0(4)) == 0);
    CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == kPositiveZero);
    f.exec(fmul(4, 3, 1));
    CHECK((cpu.fpscr() & Gekko::kFpscrFprf) == kNegativeZero);

    // Moves change bits only, ps1 untouched, FPSCR untouched.
    cpu.set_ps(5, from_bits(kSignalingNan), 8.0);
    cpu.set_ps(6, 0.0, 7.0);
    const std::uint32_t before = cpu.fpscr();
    f.exec(fmr(6, 5));
    CHECK(bits(cpu.ps0(6)) == kSignalingNan);
    CHECK(cpu.ps1(6) == 7.0);
    f.exec(fneg(6, 5));
    CHECK(bits(cpu.ps0(6)) == (kSignalingNan | 0x8000000000000000ull));
    f.exec(fabs_(6, 6));
    CHECK(bits(cpu.ps0(6)) == kSignalingNan);
    f.exec(fnabs(6, 1));
    CHECK(cpu.ps0(6) == -6.0);
    CHECK(cpu.fpscr() == before);

    // fsel: frA >= 0 (including -0) picks frC, else (negative or NaN) frB.
    f.exec(fsel(7, 3, 1, 2));
    CHECK(cpu.ps0(7) == 6.0);
    f.exec(fsel(7, 6, 1, 2));
    CHECK(cpu.ps0(7) == 1.5);
    f.exec(fsel(7, 5, 1, 2));
    CHECK(cpu.ps0(7) == 1.5);

    // Rc = 1 copies FX, FEX, VX, OX into CR1.
    cpu.set_fpscr(Gekko::kFpscrFx | Gekko::kFpscrOx);
    cpu.set_cr(0);
    f.exec(fadd(8, 1, 2, true));
    CHECK(cr_field(cpu, 1) == 0x9);
}

TEST_CASE("invalid operations set the VX bits and give the default NaN") {
    FpFixture f;
    auto& cpu = f.cpu;
    const double inf = std::numeric_limits<double>::infinity();
    cpu.set_ps(1, inf, 0.0);
    cpu.set_ps(2, 0.0, 0.0);
    cpu.set_ps(3, 1.0, 0.0);
    cpu.set_ps(4, from_bits(kSignalingNan | 5), 0.0);
    cpu.set_ps(5, from_bits(kQuietNan | 7), 0.0);

    f.exec(fsub(6, 1, 1));  // inf - inf
    CHECK(bits(cpu.ps0(6)) == kQuietNan);
    CHECK(cpu.fpscr() == (Gekko::kFpscrFx | Gekko::kFpscrVx | Gekko::kFpscrVxisi | kQuietNanClass));
    cpu.set_fpscr(0);
    f.exec(fmul(6, 1, 2));  // inf * 0
    CHECK((cpu.fpscr() & Gekko::kFpscrVximz) != 0);
    cpu.set_fpscr(0);
    f.exec(fdiv(6, 2, 2));  // 0 / 0
    CHECK((cpu.fpscr() & Gekko::kFpscrVxzdz) != 0);
    cpu.set_fpscr(0);
    f.exec(fdiv(6, 1, 1));  // inf / inf
    CHECK((cpu.fpscr() & Gekko::kFpscrVxidi) != 0);
    cpu.set_fpscr(0);
    f.exec(fmadd(6, 1, 2, 3));  // inf * 0 + 1
    CHECK((cpu.fpscr() & Gekko::kFpscrVximz) != 0);
    cpu.set_fpscr(0);
    f.exec(fmadd(6, 1, 3, 1));  // inf * 1 + inf: fine
    CHECK(cpu.ps0(6) == inf);
    CHECK(cpu.fpscr() == kPositiveInfinity);
    cpu.set_fpscr(0);
    cpu.set_ps(7, -inf, 0.0);
    f.exec(fmadd(6, 1, 3, 7));  // inf * 1 - inf
    CHECK((cpu.fpscr() & Gekko::kFpscrVxisi) != 0);

    // x / 0: ZX and a signed infinity.
    cpu.set_fpscr(0);
    f.exec(fdiv(6, 3, 2));
    CHECK(cpu.ps0(6) == inf);
    CHECK(cpu.fpscr() == (Gekko::kFpscrFx | Gekko::kFpscrZx | kPositiveInfinity));

    // NaN operands: the first NaN of frA, frB, frC, made quiet; a signaling one sets VXSNAN.
    cpu.set_fpscr(0);
    f.exec(fadd(6, 3, 4));
    CHECK(bits(cpu.ps0(6)) == (kSignalingNan | 5 | 0x0008000000000000ull));
    CHECK((cpu.fpscr() & Gekko::kFpscrVxsnan) != 0);
    cpu.set_fpscr(0);
    f.exec(fmadd(6, 3, 4, 5));  // frA = 1, frC = SNaN, frB = QNaN: frB wins
    CHECK(bits(cpu.ps0(6)) == (kQuietNan | 7));
    CHECK((cpu.fpscr() & Gekko::kFpscrVxsnan) != 0);
    cpu.set_fpscr(0);
    f.exec(fadd(6, 5, 3));
    CHECK(bits(cpu.ps0(6)) == (kQuietNan | 7));
    CHECK(cpu.fpscr() == kQuietNanClass);

    // With VE = 1 an invalid operation leaves frD (and FPRF) alone.
    cpu.set_fpscr(Gekko::kFpscrVe);
    cpu.set_ps(6, 3.0, 4.0);
    f.exec(fsub(6, 1, 1));
    CHECK(cpu.ps0(6) == 3.0);
    CHECK((cpu.fpscr() & Gekko::kFpscrFex) != 0);
    // With ZE = 1 a divide by zero leaves frD alone.
    cpu.set_fpscr(Gekko::kFpscrZe);
    f.exec(fdiv(6, 3, 2));
    CHECK(cpu.ps0(6) == 3.0);

    // Overflow from finite operands sets OX and XX.
    cpu.set_fpscr(0);
    cpu.set_ps(8, 1.0e300, 0.0);
    f.exec(fmul(6, 8, 8));
    CHECK(cpu.ps0(6) == inf);
    CHECK((cpu.fpscr() & (Gekko::kFpscrOx | Gekko::kFpscrXx)) == (Gekko::kFpscrOx | Gekko::kFpscrXx));
    cpu.set_fpscr(0);
    cpu.set_ps(8, 1.0e30, 0.0);
    f.exec(fmuls(6, 8, 8));
    CHECK(cpu.ps0(6) == inf);
    CHECK((cpu.fpscr() & Gekko::kFpscrOx) != 0);
}

TEST_CASE("FPSCR[NI] flushes denormal results to zero") {
    FpFixture f;
    auto& cpu = f.cpu;
    const double tiny_single = 1.0e-40;  // a denormal as a single
    cpu.set_ps(1, tiny_single, 0.0);
    cpu.set_ps(2, 0.0, 0.0);
    f.exec(fadds(3, 1, 2));
    CHECK(cpu.ps0(3) == to_single(tiny_single));
    CHECK(cpu.ps0(3) != 0.0);
    cpu.set_fpscr(Gekko::kFpscrNi);
    f.exec(fadds(3, 1, 2));
    CHECK(bits(cpu.ps0(3)) == 0);
    cpu.set_ps(1, -1.0e-310, 0.0);  // a double denormal
    f.exec(fadd(3, 1, 2));
    CHECK(bits(cpu.ps0(3)) == 0x8000000000000000ull);
}

TEST_CASE("fres and frsqrte give Gekko's table estimates") {
    FpFixture f;
    auto& cpu = f.cpu;
    // Values from Dolphin's Common/FloatUtils.cpp (ApproximateReciprocal and
    // ApproximateReciprocalSquareRoot, which match hardware).
    struct Case {
        double in, out;
    };
    const Case reciprocal[] = {
        {1.0, 0.9998779296875},  {2.0, 0.49993896484375},       {3.0, 0.33331298828125},
        {-2.0, -0.49993896484375}, {1.0009765625, 0.9989309310913086},
    };
    for (const Case& c : reciprocal) {
        CAPTURE(c.in);
        cpu.set_ps(1, c.in, 0.0);
        cpu.set_ps(2, 0.0, 0.0);
        f.exec(fres(2, 1));
        CHECK(cpu.ps0(2) == c.out);
        CHECK(cpu.ps1(2) == c.out);  // single precision: duplicated
    }
    const Case rsqrt[] = {
        {1.0, 0.99981689453125},
        {4.0, 0.499908447265625},
        {2.0, 0.7069854736328125},
    };
    for (const Case& c : rsqrt) {
        CAPTURE(c.in);
        cpu.set_ps(1, c.in, 0.0);
        cpu.set_ps(2, 0.0, 5.0);
        f.exec(frsqrte(2, 1));
        CHECK(cpu.ps0(2) == c.out);
        CHECK(cpu.ps1(2) == 5.0);  // double precision: ps1 untouched
    }
    // Special cases: 0 (ZX, infinity), negative (VXSQRT, NaN), infinity (0).
    cpu.set_fpscr(0);
    cpu.set_ps(1, 0.0, 0.0);
    f.exec(fres(2, 1));
    CHECK(cpu.ps0(2) == std::numeric_limits<double>::infinity());
    CHECK((cpu.fpscr() & Gekko::kFpscrZx) != 0);
    cpu.set_fpscr(0);
    cpu.set_ps(1, -1.0, 0.0);
    f.exec(frsqrte(2, 1));
    CHECK(bits(cpu.ps0(2)) == kQuietNan);
    CHECK((cpu.fpscr() & Gekko::kFpscrVxsqrt) != 0);
    cpu.set_ps(1, std::numeric_limits<double>::infinity(), 0.0);
    f.exec(frsqrte(2, 1));
    CHECK(bits(cpu.ps0(2)) == 0);
    // Very small inputs give the largest single, very large ones 0.
    cpu.set_ps(1, 1.0e-300, 0.0);
    f.exec(fres(2, 1));
    CHECK(cpu.ps0(2) == static_cast<double>(std::numeric_limits<float>::max()));
}

TEST_CASE("an FPU instruction with MSR[FP]=0 raises FP unavailable") {
    GekkoFixture f;
    auto& cpu = f.cpu;
    cpu.set_spr(spr::kHid2, Gekko::kHid2Lsqe | Gekko::kHid2Pse);
    f.bus.write32(0x1000, 0x3F800000);
    cpu.set_gpr(4, 0x1000);
    for (const std::uint32_t word : {fadd(1, 2, 3), fadds(1, 2, 3), lfs(1, 0, 4), lfdu(1, 0, 4), stfd(1, 0, 4),
                                     lfsx(1, 0, 4), stfiwx(1, 0, 4), mffs(1), mtfsfi(7, 1), fmr(1, 2),
                                     fcmpu(0, 1, 2), ps_add(1, 2, 3), ps_merge00(1, 2, 3), psq_l(1, 0, 4, 0, 0),
                                     psq_lx(1, 0, 4, 0, 0), psq_stu(1, 0, 4, 0, 0)}) {
        CAPTURE(word);
        cpu.set_msr(Gekko::kMsrMe | Gekko::kMsrEe);
        cpu.set_ps(1, 2.0, 3.0);
        cpu.set_fpscr(0);
        f.exec(word);
        CHECK(cpu.pc() == 0x800);
        CHECK(cpu.spr(spr::kSrr0) == 0x3000);
        CHECK(cpu.spr(spr::kSrr1) == (Gekko::kMsrMe | Gekko::kMsrEe));
        CHECK(cpu.msr() == Gekko::kMsrMe);
        CHECK(cpu.ps0(1) == 2.0);
        CHECK(cpu.ps1(1) == 3.0);
        CHECK(cpu.fpscr() == 0);
        CHECK(cpu.gpr(4) == 0x1000);
    }
    CHECK(f.bus.read32(0x1000) == 0x3F800000);
    // dcbz_l is in opcode 4 but is a cache instruction: no FP check.
    cpu.set_msr(0);
    cpu.set_spr(spr::kHid0, Gekko::kHid0Dce);
    cpu.set_spr(spr::kHid2, Gekko::kHid2Lce);
    f.exec(dcbz_l(0, 4));
    CHECK(cpu.pc() == 0x3004);
    CHECK(f.bus.read32(0x1000) == 0);
}
