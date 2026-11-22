#pragma once

// Internal to the Gekko interpreter: the floating-point helpers gekko_fpu.cpp and
// gekko_paired.cpp share (value conversions, FPSCR updates, the arithmetic with the PEM's
// NaN and invalid-operation rules, the estimate tables, the host rounding mode).
//
// Precision of FPSCR tracking: VX* (with VXSNAN), ZX, OX (an infinite result from finite
// operands, with XX), FPRF and FPCC are set as the PEM says. Ordinary inexact results (XX,
// FI, FR) and underflow (UX) are not tracked for arithmetic, as in Dolphin's interpreter;
// frsp and fctiw[z] do set FI, FR and XX. Enabled exceptions (FE0/FE1) are not taken; with
// VE or ZE set, the scalar instructions leave frD alone, as the PEM says.

#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "gc/gekko/gekko.hpp"
#include "gc/gekko/gekko_ops.hpp"

// Floating-point environment access for the rest of the including file (only gekko_fpu.cpp
// and gekko_paired.cpp include this): the compiler must not fold or move guest arithmetic
// across HostRounding's mode changes. GCC has no such pragma (it would need -frounding-math).
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fenv_access(on)
#elif defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace ezy::gc::fp {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

inline constexpr u64 kSign = 0x8000000000000000, kExp = 0x7FF0000000000000, kFrac = 0x000FFFFFFFFFFFFF;
inline constexpr u64 kQuietBit = 0x0008000000000000;
inline constexpr u64 kDefaultNan = 0x7FF8000000000000;  // the PEM's default QNaN

inline u64 bits(double value) { return std::bit_cast<u64>(value); }
inline double from_bits(u64 value) { return std::bit_cast<double>(value); }
inline bool is_nan(double v) { return (bits(v) & ~kSign) > kExp; }
inline bool is_snan(double v) { return is_nan(v) && (bits(v) & kQuietBit) == 0; }
inline bool is_inf(double v) { return (bits(v) & ~kSign) == kExp; }
inline bool is_finite(double v) { return (bits(v) & kExp) != kExp; }
inline double make_quiet(double v) { return from_bits(bits(v) | kQuietBit); }

inline PairedSingle& fpr(Gekko& cpu, int index) { return cpu.state().fpr[static_cast<std::size_t>(index)]; }

// Every floating-point instruction (loads and stores, paired singles and the FPSCR moves
// included) raises FP unavailable while MSR[FP] = 0, before doing anything.
template <GekkoOp Op>
void needs_fp(Gekko& cpu, u32 word) {
    if ((cpu.state().msr & Gekko::kMsrFp) == 0) GekkoOps::raise(cpu, Gekko::Exception::FpUnavailable);
    Op(cpu, word);
}

// ---- Rounding --------------------------------------------------------------------------------

// Runs the host's floating-point unit in FPSCR[RN]'s rounding mode for its lifetime (one
// instruction). The host runs to nearest (the C++ default), so only the three directed
// modes switch, and the common case costs nothing.
class HostRounding {
public:
    explicit HostRounding(u32 fpscr) : rn_(fpscr & Gekko::kFpscrRn) {
        if (rn_ != 0) {
            saved_ = std::fegetround();
            std::fesetround(kHostMode[rn_]);
        }
    }
    ~HostRounding() {
        if (rn_ != 0) std::fesetround(saved_);
    }
    HostRounding(const HostRounding&) = delete;
    HostRounding& operator=(const HostRounding&) = delete;

private:
    // RN: 0 nearest (ties to even), 1 toward zero, 2 toward +infinity, 3 toward -infinity.
    static constexpr int kHostMode[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    u32 rn_;
    int saved_ = FE_TONEAREST;
};

// A result rounded to single precision (in the current rounding mode), as a double. With
// FPSCR[NI] a value below the smallest normal single before rounding becomes a signed zero,
// even when rounding would have made it normal (Dolphin's ForceSingle, from hardware tests).
inline double force_single(u32 fpscr, double value) {
    if ((fpscr & Gekko::kFpscrNi) != 0 && (bits(value) & ~kSign) < 0x3810000000000000) {
        return from_bits(bits(value) & kSign);
    }
    return static_cast<double>(static_cast<float>(value));
}
// A double-precision result: with FPSCR[NI] a denormal becomes a signed zero.
inline double force_double(u32 fpscr, double value) {
    if ((fpscr & Gekko::kFpscrNi) != 0 && (bits(value) & kExp) == 0) return from_bits(bits(value) & kSign);
    return value;
}

// The Gekko's single-precision multiplies (fmuls, fmadds and family, ps_mul, ps_madd...)
// first round frC's mantissa to 24 bits, to nearest with ties away from zero, whatever RN
// says; a denormal is rounded as if normalized. (Dolphin's Force25Bit, matched against
// hardware.)
inline double round_frc(double c) {
    u64 b = bits(c);
    u64 keep = 0xFFFFFFFFF8000000;
    u64 round = 0x8000000;
    if ((b & kExp) == 0 && (b & kFrac) != 0) {
        const int shift = std::countl_zero(b & kFrac) - 11;
        keep = static_cast<u64>(static_cast<s64>(keep) >> shift);
        round >>= shift;
    }
    b = (b & keep) + (b & round);
    return from_bits(b);
}

// ---- Value conversions (PEM 3.x, "Floating-Point Load/Store Instructions") ---------------

// lfs and psq_l's float type: a single widened to a double exactly. No rounding and no
// quieting: a signaling NaN stays signaling; a denormal single becomes a normal double.
inline u64 single_to_double(u32 word) {
    const u64 x = word;
    const u32 exp = (word >> 23) & 0xFF;
    const u32 frac = word & 0x7FFFFF;
    if (exp == 0 && frac != 0) {
        u64 e = 1023 - 126;
        u64 f = frac;
        do {
            f <<= 1;
            --e;
        } while ((f & 0x800000) == 0);
        return ((x & 0x80000000) << 32) | (e << 52) | ((f & 0x7FFFFF) << 29);
    }
    // Sign and exponent MSB copied, then three copies of the complemented MSB (a normal
    // number) or of the MSB itself (zero, infinity, NaN), then the rest of the word.
    const u64 msb = (x >> 30) & 1;
    const u64 fill = (exp != 0 && exp != 0xFF) ? (msb ^ 1) : msb;
    return ((x & 0xC0000000) << 32) | (fill * 0x3800000000000000) | ((x & 0x3FFFFFFF) << 29);
}

// stfs: a double narrowed to a single by selecting bits, with no rounding. Values in the
// single denormal range are denormalized (truncating); smaller ones are undefined in the PEM
// and give the plain bit selection, as hardware does (Dolphin's ConvertToSingle).
inline u32 double_to_single(u64 x) {
    const auto exp = static_cast<u32>((x >> 52) & 0x7FF);
    if (exp > 896 || (x & ~kSign) == 0 || exp < 874) {
        return static_cast<u32>(((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF));
    }
    const u32 t = (0x80000000u | static_cast<u32>((x & kFrac) >> 21)) >> (905 - exp);
    return t | static_cast<u32>((x >> 32) & 0x80000000);
}

// psq_st's float type: like stfs, but anything below the normal single range stores as a
// signed zero (Dolphin's ConvertToSingleFTZ).
inline u32 double_to_single_ftz(u64 x) {
    const auto exp = static_cast<u32>((x >> 52) & 0x7FF);
    if (exp > 896 || (x & ~kSign) == 0) {
        return static_cast<u32>(((x >> 32) & 0xC0000000) | ((x >> 29) & 0x3FFFFFFF));
    }
    return static_cast<u32>((x >> 32) & 0x80000000);
}

// ---- FPSCR -------------------------------------------------------------------------------------

// Sets sticky exception bits: FX too when one of them was clear; then the summaries.
inline void set_exception(GekkoState& s, u32 mask) {
    if ((s.fpscr & mask) != mask) s.fpscr |= Gekko::kFpscrFx;
    s.fpscr = Gekko::fpscr_with_summary(s.fpscr | mask);
}
inline void clear_fi_fr(GekkoState& s) { s.fpscr &= ~(Gekko::kFpscrFi | Gekko::kFpscrFr); }

// FPRF result classes (PEM table 2-x): C, FL, FG, FE, FU.
inline constexpr u32 kClassQuietNan = 0x11, kClassNegInfinity = 0x09, kClassNegNormal = 0x08,
                     kClassNegDenormal = 0x18, kClassNegZero = 0x12, kClassPosZero = 0x02,
                     kClassPosDenormal = 0x14, kClassPosNormal = 0x04, kClassPosInfinity = 0x05;

inline u32 classify(bool negative, bool max_exp, bool zero_exp, bool zero_frac) {
    if (max_exp) return !zero_frac ? kClassQuietNan : negative ? kClassNegInfinity : kClassPosInfinity;
    if (!zero_exp) return negative ? kClassNegNormal : kClassPosNormal;
    if (!zero_frac) return negative ? kClassNegDenormal : kClassPosDenormal;
    return negative ? kClassNegZero : kClassPosZero;
}
inline u32 classify_double(double v) {
    const u64 b = bits(v);
    return classify((b & kSign) != 0, (b & kExp) == kExp, (b & kExp) == 0, (b & kFrac) == 0);
}
// For a value that is a single: its class as a single (a single denormal is a normal double).
inline u32 classify_single(double v) {
    const auto b = std::bit_cast<u32>(static_cast<float>(v));
    return classify((b & 0x80000000) != 0, (b & 0x7F800000) == 0x7F800000, (b & 0x7F800000) == 0,
                    (b & 0x7FFFFF) == 0);
}
inline void set_fprf(GekkoState& s, u32 result_class) {
    s.fpscr = (s.fpscr & ~Gekko::kFpscrFprf) | (result_class << 12);
}

inline void set_cr_field(GekkoState& s, int field, u32 value) {
    const int shift = 28 - 4 * field;
    s.cr = (s.cr & ~(0xFu << shift)) | ((value & 0xF) << shift);
}
// Rc = 1: CR1 = FPSCR[FX, FEX, VX, OX].
inline void record(Gekko& cpu, u32 word) {
    if (ppc_fields::rc(word)) set_cr_field(cpu.state(), 1, cpu.state().fpscr >> 28);
}

// ---- Arithmetic ----------------------------------------------------------------------------------

// An operation's result before the final rounding: the value, the exception bit it raised
// (a VX* bit or ZX, 0 for none) and whether every operand was finite (for overflow).
struct Result {
    double value;
    u32 exception = 0;
    bool finite_operands = true;
};

// The result of an operation that gave a NaN (PEM 3.3.6.1): the first NaN operand in the
// order given (frA, frB, frC), made quiet, with VXSNAN if any operand is signaling;
// otherwise the operation itself was invalid: the default NaN and `invalid`.
inline Result nan_result(GekkoState& s, u32 invalid, double x, double y, double z = 0.0) {
    u32 raised = 0;
    if (is_snan(x) || is_snan(y) || is_snan(z)) {
        raised = Gekko::kFpscrVxsnan;
        set_exception(s, raised);
    }
    for (const double v : {x, y, z}) {
        if (is_nan(v)) return {make_quiet(v), raised, false};
    }
    set_exception(s, invalid);
    return {from_bits(kDefaultNan), invalid, false};
}

inline Result add(GekkoState& s, double a, double b) {
    const double r = a + b;
    if (is_nan(r)) return nan_result(s, Gekko::kFpscrVxisi, a, b);
    return {r, 0, is_finite(a) && is_finite(b)};
}
inline Result sub(GekkoState& s, double a, double b) {
    const double r = a - b;
    if (is_nan(r)) return nan_result(s, Gekko::kFpscrVxisi, a, b);
    return {r, 0, is_finite(a) && is_finite(b)};
}
inline Result mul(GekkoState& s, double a, double c) {
    const double r = a * c;
    if (is_nan(r)) return nan_result(s, Gekko::kFpscrVximz, a, c);
    return {r, 0, is_finite(a) && is_finite(c)};
}
inline Result div(GekkoState& s, double a, double b) {
    const double r = a / b;
    if (is_nan(r)) return nan_result(s, b == 0.0 ? Gekko::kFpscrVxzdz : Gekko::kFpscrVxidi, a, b);
    // A finite nonzero number over zero: ZX and a signed infinity.
    if (b == 0.0 && is_finite(a)) {
        set_exception(s, Gekko::kFpscrZx);
        return {r, Gekko::kFpscrZx, false};
    }
    return {r, 0, is_finite(a) && is_finite(b)};
}

// a * c + b (or a * c - b), rounded once. NaN priority is still frA, frB, frC.
// Single = true for the single-precision forms: frC is rounded first (round_frc), and the
// result, which the caller rounds to single, must round as the exact value would. A double
// FMA rounded again to single differs from one rounding only when the double lands exactly
// halfway between two singles (low 29 bits = 0x10000000); then the FMA's own error
// (recovered with an error-free 2Sum) shows which side the exact value is on, and the
// double moves one ulp that way. (Dolphin's NI_madd_msub, checked against hardware: the
// Mario Strikers Charged case gives 0xBF55BF17, not 0xBF55BF18.)
template <bool Single>
Result madd(GekkoState& s, double a, double c, double b, bool subtract) {
    const double c_used = Single ? round_frc(c) : c;
    const double addend = subtract ? -b : b;
    double r = std::fma(a, c_used, addend);
    if constexpr (Single) {
        const u64 rb = bits(r);
        if ((rb & 0x1FFFFFFF) == 0x10000000) {
            const double a_prime = addend - r;
            const double b_prime = r + a_prime;
            const double delta_a = std::fma(a, c_used, a_prime);
            const double delta_b = addend - b_prime;
            const double error = delta_a + delta_b;
            if (error != 0.0) r = from_bits((error > 0.0) == (r > 0.0) ? rb + 1 : rb - 1);
        }
    }
    if (is_nan(r)) {
        return nan_result(s, is_nan(a * c) ? Gekko::kFpscrVximz : Gekko::kFpscrVxisi, a, b, c);
    }
    return {r, 0, is_finite(a) && is_finite(b) && is_finite(c)};
}

// An infinite result from finite operands is an overflow: OX, and XX (PEM 3.3.6.2.1).
inline void note_overflow(GekkoState& s, const Result& r, double rounded) {
    if (r.finite_operands && is_inf(rounded)) set_exception(s, Gekko::kFpscrOx | Gekko::kFpscrXx);
}

// ---- Compares ------------------------------------------------------------------------------------

// fcmpu/fcmpo and ps_cmp*: FPCC and CR field crfD = FL, FG, FE or FU. A NaN is unordered;
// a signaling one raises VXSNAN; the ordered compares also raise VXVC for a quiet NaN (for
// a signaling one only while VE = 0). FPRF's C bit is left alone.
inline void compare(Gekko& cpu, u32 word, double a, double b, bool ordered) {
    GekkoState& s = cpu.state();
    u32 result = 0;
    if (is_nan(a) || is_nan(b)) {
        result = 1;
        if (is_snan(a) || is_snan(b)) {
            set_exception(s, Gekko::kFpscrVxsnan);
            if (ordered && (s.fpscr & Gekko::kFpscrVe) == 0) set_exception(s, Gekko::kFpscrVxvc);
        } else if (ordered) {
            set_exception(s, Gekko::kFpscrVxvc);
        }
    } else {
        result = a < b ? 8u : a > b ? 4u : 2u;
    }
    s.fpscr = (s.fpscr & ~Gekko::kFpscrFpcc) | (result << 12);
    set_cr_field(s, ppc_fields::crfd(word), result);
}

// ---- Estimates (fres/ps_res, frsqrte/ps_rsqrte) ----------------------------------------------

// The Gekko computes these from tables of a base and a slope per interval, not as IEEE
// operations. Tables and algorithm: Dolphin's Common/FloatUtils.cpp (ApproximateReciprocal,
// ApproximateReciprocalSquareRoot), derived from hardware results.
struct BaseAndSlope {
    s64 base;
    s64 slope;
};

inline constexpr std::array<BaseAndSlope, 32> kReciprocalSqrtTable = {{
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
}};

inline constexpr std::array<BaseAndSlope, 32> kReciprocalTable = {{
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
}};

// fres: 1/x estimate. 0 -> signed infinity, infinity -> signed zero, NaN -> made quiet;
// inputs too small for a single reciprocal give the largest single, too large give zero.
inline double estimate_reciprocal(double value) {
    const auto x = static_cast<s64>(bits(value));
    const s64 mantissa = x & static_cast<s64>(kFrac);
    const s64 sign = x & static_cast<s64>(kSign);
    s64 exponent = x & static_cast<s64>(kExp);
    if (mantissa == 0 && exponent == 0) return std::copysign(std::numeric_limits<double>::infinity(), value);
    if (exponent == static_cast<s64>(kExp)) return mantissa == 0 ? std::copysign(0.0, value) : make_quiet(value);
    if (exponent < (s64{895} << 52)) return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), value);
    if (exponent >= (s64{1149} << 52)) return std::copysign(0.0, value);
    exponent = (s64{0x7FD} << 52) - exponent;
    const auto i = static_cast<int>(mantissa >> 37);
    const BaseAndSlope& entry = kReciprocalTable[static_cast<std::size_t>(i / 1024)];
    const s64 fraction = entry.base - (entry.slope * (i % 1024) + 1) / 2;
    return from_bits(static_cast<u64>(sign | exponent | (fraction << 29)));
}

// frsqrte: 1/sqrt(x) estimate. +-0 -> signed infinity, +infinity -> 0, negative (including
// -infinity) -> the default NaN, NaN -> made quiet; denormals are normalized first.
inline double estimate_reciprocal_sqrt(double value) {
    const auto x = static_cast<s64>(bits(value));
    s64 mantissa = x & static_cast<s64>(kFrac);
    const s64 sign = x & static_cast<s64>(kSign);
    s64 exponent = x & static_cast<s64>(kExp);
    if (mantissa == 0 && exponent == 0) return std::copysign(std::numeric_limits<double>::infinity(), value);
    if (exponent == static_cast<s64>(kExp)) {
        if (mantissa != 0) return make_quiet(value);
        return sign != 0 ? from_bits(kDefaultNan) : 0.0;
    }
    if (sign != 0) return from_bits(kDefaultNan);
    if (exponent == 0) {
        do {
            exponent -= s64{1} << 52;
            mantissa <<= 1;
        } while ((mantissa & (s64{1} << 52)) == 0);
        mantissa &= static_cast<s64>(kFrac);
        exponent += s64{1} << 52;
    }
    const s64 exponent_lsb = exponent & (s64{1} << 52);
    exponent = ((s64{0x3FF} << 52) - ((exponent - (s64{0x3FE} << 52)) / 2)) & static_cast<s64>(kExp);
    const auto i = static_cast<int>((exponent_lsb | mantissa) >> 37);
    const BaseAndSlope& entry = kReciprocalSqrtTable[static_cast<std::size_t>(i / 2048)];
    const s64 fraction = entry.base + entry.slope * (i % 2048);
    return from_bits(static_cast<u64>(sign | exponent | (fraction << 26)));
}

}  // namespace ezy::gc::fp
