// The Gekko's scalar floating-point side: loads and stores, arithmetic (double precision
// and the single-precision forms, which round to single and write both halves of frD),
// fused multiply-add, conversions, compares, estimates and the FPSCR moves. Semantics from
// the PEM (chapter 3 and the instruction pages); where the Gekko differs from it (frC
// rounding, ps1 duplication, estimate tables, mffs/fctiw high words), the code says so and
// follows Dolphin's Interpreter_FloatingPoint.cpp, which matches hardware tests.
//
// The 750 has no fsqrt/fsqrts: their slots stay unimplemented.

#include <cmath>
#include <cstdint>

#include "gc/gekko/gekko.hpp"
#include "gc/gekko/gekko_fp.hpp"
#include "gc/gekko/gekko_ops.hpp"

namespace ezy::gc {

namespace {

using namespace ppc_fields;
using namespace fp;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

// ---- Loads and stores ----------------------------------------------------------------------------

// The 750 raises alignment for a floating-point load or store whose EA is not word-aligned
// (750 manual, alignment exception causes; Dolphin's lfs/lfd/stfs/stfd/stfiwx). A word-
// aligned doubleword is fine (split into two words).
void check_word_aligned(Gekko& cpu, u32 ea, u32 w) {
    if ((ea & 3) != 0) GekkoOps::raise_alignment(cpu, ea, w);
}

// lfs: the single widened exactly, into both halves.
template <u32 (*Ea)(Gekko&, u32), bool Update>
void op_lfs(Gekko& cpu, u32 w) {
    const u32 ea = Ea(cpu, w);
    check_word_aligned(cpu, ea, w);
    const double value = from_bits(single_to_double(GekkoOps::read32(cpu, ea)));
    fpr(cpu, rd(w)) = {value, value};
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
// lfd: the doubleword's bits into ps0; ps1 unchanged.
template <u32 (*Ea)(Gekko&, u32), bool Update>
void op_lfd(Gekko& cpu, u32 w) {
    const u32 ea = Ea(cpu, w);
    check_word_aligned(cpu, ea, w);
    fpr(cpu, rd(w)).ps0 = from_bits(GekkoOps::read64(cpu, ea));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
// stfs: ps0 narrowed by bit selection (no rounding).
template <u32 (*Ea)(Gekko&, u32), bool Update>
void op_stfs(Gekko& cpu, u32 w) {
    const u32 ea = Ea(cpu, w);
    check_word_aligned(cpu, ea, w);
    GekkoOps::write32(cpu, ea, double_to_single(bits(fpr(cpu, rd(w)).ps0)));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
template <u32 (*Ea)(Gekko&, u32), bool Update>
void op_stfd(Gekko& cpu, u32 w) {
    const u32 ea = Ea(cpu, w);
    check_word_aligned(cpu, ea, w);
    GekkoOps::write64(cpu, ea, bits(fpr(cpu, rd(w)).ps0));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
// stfiwx: the low word of ps0's bits (an fctiw result).
void op_stfiwx(Gekko& cpu, u32 w) {
    const u32 ea = ea_x(cpu, w);
    check_word_aligned(cpu, ea, w);
    GekkoOps::write32(cpu, ea, static_cast<u32>(bits(fpr(cpu, rd(w)).ps0)));
}

// ---- Arithmetic --------------------------------------------------------------------------------

// The end of every A-form arithmetic instruction: unless VE/ZE suppress it, frD gets the
// result rounded to double (ps0 only) or to single (both halves: the Gekko duplicates single
// results into ps1), negated for the fnm* forms (a NaN is not negated), and FPRF its class.
template <bool Single>
void finish(Gekko& cpu, u32 w, const Result& r, bool negate = false) {
    GekkoState& s = cpu.state();
    const bool suppressed = ((r.exception & Gekko::kFpscrVxAny) != 0 && (s.fpscr & Gekko::kFpscrVe) != 0) ||
                            (r.exception == Gekko::kFpscrZx && (s.fpscr & Gekko::kFpscrZe) != 0);
    if (!suppressed) {
        double value = Single ? force_single(s.fpscr, r.value) : force_double(s.fpscr, r.value);
        if (negate && !is_nan(value)) value = -value;
        note_overflow(s, r, value);
        PairedSingle& d = fpr(cpu, rd(w));
        d.ps0 = value;
        if constexpr (Single) d.ps1 = value;
        set_fprf(s, Single ? classify_single(value) : classify_double(value));
    }
    record(cpu, w);
}

double fa(Gekko& cpu, u32 w) { return fpr(cpu, ra(w)).ps0; }
double fb(Gekko& cpu, u32 w) { return fpr(cpu, rb(w)).ps0; }
double fc(Gekko& cpu, u32 w) { return fpr(cpu, rc_field(w)).ps0; }

template <bool Single>
void op_fadd(Gekko& cpu, u32 w) {
    const HostRounding rounding(cpu.state().fpscr);
    finish<Single>(cpu, w, add(cpu.state(), fa(cpu, w), fb(cpu, w)));
}
template <bool Single>
void op_fsub(Gekko& cpu, u32 w) {
    const HostRounding rounding(cpu.state().fpscr);
    finish<Single>(cpu, w, sub(cpu.state(), fa(cpu, w), fb(cpu, w)));
}
template <bool Single>
void op_fmul(Gekko& cpu, u32 w) {
    const HostRounding rounding(cpu.state().fpscr);
    const double c = Single ? round_frc(fc(cpu, w)) : fc(cpu, w);
    finish<Single>(cpu, w, mul(cpu.state(), fa(cpu, w), c));
}
template <bool Single>
void op_fdiv(Gekko& cpu, u32 w) {
    const HostRounding rounding(cpu.state().fpscr);
    finish<Single>(cpu, w, div(cpu.state(), fa(cpu, w), fb(cpu, w)));
}
// fmadd = a*c + b, fmsub = a*c - b, fnmadd/fnmsub = their negations.
template <bool Single, bool Subtract, bool Negate>
void op_fmadd(Gekko& cpu, u32 w) {
    const HostRounding rounding(cpu.state().fpscr);
    finish<Single>(cpu, w, madd<Single>(cpu.state(), fa(cpu, w), fc(cpu, w), fb(cpu, w), Subtract), Negate);
}

// fsel: frD = frA >= 0 (-0 included) ? frC : frB; a NaN frA picks frB. No FPSCR change.
void op_fsel(Gekko& cpu, u32 w) {
    fpr(cpu, rd(w)).ps0 = fa(cpu, w) >= 0.0 ? fc(cpu, w) : fb(cpu, w);
    record(cpu, w);
}

// fres (single precision: both halves) and frsqrte (double: ps0), the table estimates.
// 0 raises ZX, a signaling NaN VXSNAN, a negative frsqrte operand VXSQRT; with ZE/VE set
// frD is left alone (Dolphin's fresx/frsqrtex).
void op_fres(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const double b = fb(cpu, w);
    if (b == 0.0) {
        set_exception(s, Gekko::kFpscrZx);
        if ((s.fpscr & Gekko::kFpscrZe) != 0) return record(cpu, w);
    } else if (is_snan(b)) {
        set_exception(s, Gekko::kFpscrVxsnan);
        if ((s.fpscr & Gekko::kFpscrVe) != 0) return record(cpu, w);
    }
    const double value = estimate_reciprocal(b);
    fpr(cpu, rd(w)) = {value, value};
    set_fprf(s, classify_single(value));
    record(cpu, w);
}
void op_frsqrte(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const double b = fb(cpu, w);
    if (b < 0.0) {
        set_exception(s, Gekko::kFpscrVxsqrt);
        if ((s.fpscr & Gekko::kFpscrVe) != 0) return record(cpu, w);
    } else if (b == 0.0) {
        set_exception(s, Gekko::kFpscrZx);
        if ((s.fpscr & Gekko::kFpscrZe) != 0) return record(cpu, w);
    } else if (is_snan(b)) {
        set_exception(s, Gekko::kFpscrVxsnan);
        if ((s.fpscr & Gekko::kFpscrVe) != 0) return record(cpu, w);
    }
    const double value = estimate_reciprocal_sqrt(b);
    fpr(cpu, rd(w)).ps0 = value;
    set_fprf(s, classify_double(value));
    record(cpu, w);
}

// ---- Moves (bit operations on ps0: no FPSCR change, ps1 unchanged) ----------------------

void op_fmr(Gekko& cpu, u32 w) {
    fpr(cpu, rd(w)).ps0 = fb(cpu, w);
    record(cpu, w);
}
void op_fneg(Gekko& cpu, u32 w) {
    fpr(cpu, rd(w)).ps0 = from_bits(bits(fb(cpu, w)) ^ kSign);
    record(cpu, w);
}
void op_fabs(Gekko& cpu, u32 w) {
    fpr(cpu, rd(w)).ps0 = from_bits(bits(fb(cpu, w)) & ~kSign);
    record(cpu, w);
}
void op_fnabs(Gekko& cpu, u32 w) {
    fpr(cpu, rd(w)).ps0 = from_bits(bits(fb(cpu, w)) | kSign);
    record(cpu, w);
}

// ---- Conversions -------------------------------------------------------------------------------

// frsp: frB rounded to single in RN's mode, into both halves (Dolphin: games rely on ps1).
// FI = inexact (with XX), FR = rounded up in magnitude. A NaN gives FI = FR = 0 and, if
// signaling, VXSNAN (frD left alone if VE).
void op_frsp(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const double b = fb(cpu, w);
    const HostRounding rounding(s.fpscr);
    const double value = force_single(s.fpscr, b);
    if (is_nan(b)) {
        clear_fi_fr(s);
        const bool signaling = is_snan(b);
        if (signaling) set_exception(s, Gekko::kFpscrVxsnan);
        if (!signaling || (s.fpscr & Gekko::kFpscrVe) == 0) {
            fpr(cpu, rd(w)) = {value, value};
            set_fprf(s, classify_single(value));
        }
        return record(cpu, w);
    }
    clear_fi_fr(s);
    if (value != b) {
        set_exception(s, Gekko::kFpscrXx);
        s.fpscr |= Gekko::kFpscrFi;
        if (std::fabs(value) > std::fabs(b)) s.fpscr |= Gekko::kFpscrFr;
    }
    if (is_finite(b) && is_inf(value)) set_exception(s, Gekko::kFpscrOx);
    fpr(cpu, rd(w)) = {value, value};
    set_fprf(s, classify_single(value));
    record(cpu, w);
}

// Rounds to an integer by FPSCR[RN] (in software, so the host mode does not matter).
double round_to_integer(double value, u32 rn) {
    switch (rn) {
    case 1: return std::trunc(value);
    case 2: return std::ceil(value);
    case 3: return std::floor(value);
    default: {
        // To nearest, ties to even. x - trunc(x) is exact.
        const double rounded = std::round(value);  // ties away from zero
        return std::fabs(value - std::trunc(value)) == 0.5 ? 2.0 * std::round(value / 2.0) : rounded;
    }
    }
}

// fctiw/fctiwz (PEM C.4.2): frB rounded to a 32-bit signed integer, saturating to
// 0x7FFFFFFF / 0x80000000 with VXCVI (a NaN gives 0x80000000, plus VXSNAN if signaling).
// The result goes in ps0's low word; the high word is 0xFFF80000, with bit 32 set for a
// zero result from a negative operand (hardware results: Dolphin's ConvertToInteger). FPRF
// is not changed; ps1 is unchanged.
void convert_to_integer(Gekko& cpu, u32 w, u32 rn) {
    GekkoState& s = cpu.state();
    const double b = fb(cpu, w);
    u32 value = 0;
    bool invalid = false;
    if (is_nan(b)) {
        if (is_snan(b)) set_exception(s, Gekko::kFpscrVxsnan);
        value = 0x80000000;
        invalid = true;
    } else {
        const double rounded = round_to_integer(b, rn);
        if (rounded >= 2147483648.0) {
            value = 0x7FFFFFFF;
            invalid = true;
        } else if (rounded < -2147483648.0) {
            value = 0x80000000;
            invalid = true;
        } else {
            const auto integer = static_cast<s32>(rounded);
            value = static_cast<u32>(integer);
            clear_fi_fr(s);
            if (static_cast<double>(integer) != b) {
                set_exception(s, Gekko::kFpscrXx);
                s.fpscr |= Gekko::kFpscrFi;
                if (std::fabs(static_cast<double>(integer)) > std::fabs(b)) s.fpscr |= Gekko::kFpscrFr;
            }
        }
    }
    if (invalid) {
        set_exception(s, Gekko::kFpscrVxcvi);
        clear_fi_fr(s);
    }
    if (!invalid || (s.fpscr & Gekko::kFpscrVe) == 0) {
        u64 result = 0xFFF8000000000000 | value;
        if (value == 0 && (bits(b) & kSign) != 0) result |= 0x100000000;
        fpr(cpu, rd(w)).ps0 = from_bits(result);
    }
    record(cpu, w);
}
void op_fctiw(Gekko& cpu, u32 w) { convert_to_integer(cpu, w, cpu.state().fpscr & Gekko::kFpscrRn); }
void op_fctiwz(Gekko& cpu, u32 w) { convert_to_integer(cpu, w, 1); }

// ---- Compares --------------------------------------------------------------------------------

void op_fcmpu(Gekko& cpu, u32 w) { compare(cpu, w, fa(cpu, w), fb(cpu, w), false); }
void op_fcmpo(Gekko& cpu, u32 w) { compare(cpu, w, fa(cpu, w), fb(cpu, w), true); }

// ---- FPSCR ---------------------------------------------------------------------------------------

// mffs: ps0 = FPSCR in the low word, 0xFFF80000 in the high one (hardware result: Dolphin's
// mffsx; the PEM leaves it undefined). ps1 unchanged.
void op_mffs(Gekko& cpu, u32 w) {
    fpr(cpu, rd(w)).ps0 = from_bits(0xFFF8000000000000 | cpu.state().fpscr);
    record(cpu, w);
}
// mtfsf: the FPSCR fields selected by FM (bit 7 of FM = field 0) from frB's low word. FEX
// and VX are recomputed, never copied.
void op_mtfsf(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const u32 fm = (w >> 17) & 0xFF;
    u32 mask = 0;
    for (u32 i = 0; i < 8; ++i) {
        if ((fm & (1u << i)) != 0) mask |= 0xFu << (4 * i);
    }
    const auto source = static_cast<u32>(bits(fb(cpu, w)));
    s.fpscr = Gekko::fpscr_with_summary(((s.fpscr & ~mask) | (source & mask)) & Gekko::kFpscrWritable);
    record(cpu, w);
}
// mtfsfi: FPSCR field crfD = IMM (instruction bits 16-19).
void op_mtfsfi(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const int shift = 28 - 4 * crfd(w);
    const u32 imm = (w >> 12) & 0xF;
    s.fpscr = Gekko::fpscr_with_summary(((s.fpscr & ~(0xFu << shift)) | (imm << shift)) & Gekko::kFpscrWritable);
    record(cpu, w);
}
// mtfsb0/mtfsb1: FPSCR bit crbD. Setting an exception bit sets FX as an exception would;
// FEX and VX stay summaries (PEM: they cannot be set or cleared explicitly).
void op_mtfsb0(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    s.fpscr = Gekko::fpscr_with_summary(s.fpscr & ~(0x80000000u >> rd(w)));
    record(cpu, w);
}
void op_mtfsb1(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const u32 bit = (0x80000000u >> rd(w)) & Gekko::kFpscrWritable;
    if ((bit & Gekko::kFpscrAnyException) != 0) {
        set_exception(s, bit);
    } else {
        s.fpscr = Gekko::fpscr_with_summary(s.fpscr | bit);
    }
    record(cpu, w);
}
// mcrfs: CR field crfD = FPSCR field crfS; the exception bits it copied (FX and the sticky
// ones) are cleared.
void op_mcrfs(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const int shift = 28 - 4 * crfs(w);
    const u32 field = (s.fpscr >> shift) & 0xF;
    s.fpscr = Gekko::fpscr_with_summary(s.fpscr & ~((0xFu << shift) & (Gekko::kFpscrFx | Gekko::kFpscrAnyException)));
    set_cr_field(s, crfd(w), field);
}

}  // namespace

void install_fpu_ops(GekkoDecodeTables& t) {
    auto& p = t.primary;
    p[48] = needs_fp<op_lfs<ea_d, false>>;
    p[49] = needs_fp<op_lfs<ea_du, true>>;
    p[50] = needs_fp<op_lfd<ea_d, false>>;
    p[51] = needs_fp<op_lfd<ea_du, true>>;
    p[52] = needs_fp<op_stfs<ea_d, false>>;
    p[53] = needs_fp<op_stfs<ea_du, true>>;
    p[54] = needs_fp<op_stfd<ea_d, false>>;
    p[55] = needs_fp<op_stfd<ea_du, true>>;

    auto& x31 = t.op31;
    x31[535] = needs_fp<op_lfs<ea_x, false>>;    // lfsx
    x31[567] = needs_fp<op_lfs<ea_xu, true>>;    // lfsux
    x31[599] = needs_fp<op_lfd<ea_x, false>>;    // lfdx
    x31[631] = needs_fp<op_lfd<ea_xu, true>>;    // lfdux
    x31[663] = needs_fp<op_stfs<ea_x, false>>;   // stfsx
    x31[695] = needs_fp<op_stfs<ea_xu, true>>;   // stfsux
    x31[727] = needs_fp<op_stfd<ea_x, false>>;   // stfdx
    x31[759] = needs_fp<op_stfd<ea_xu, true>>;   // stfdux
    x31[983] = needs_fp<op_stfiwx>;

    auto& x59 = t.op59;
    fill_a_form(x59, 18, needs_fp<op_fdiv<true>>);
    fill_a_form(x59, 20, needs_fp<op_fsub<true>>);
    fill_a_form(x59, 21, needs_fp<op_fadd<true>>);
    fill_a_form(x59, 24, needs_fp<op_fres>);
    fill_a_form(x59, 25, needs_fp<op_fmul<true>>);
    fill_a_form(x59, 28, needs_fp<op_fmadd<true, true, false>>);   // fmsubs
    fill_a_form(x59, 29, needs_fp<op_fmadd<true, false, false>>);  // fmadds
    fill_a_form(x59, 30, needs_fp<op_fmadd<true, true, true>>);    // fnmsubs
    fill_a_form(x59, 31, needs_fp<op_fmadd<true, false, true>>);   // fnmadds

    auto& x63 = t.op63;
    fill_a_form(x63, 18, needs_fp<op_fdiv<false>>);
    fill_a_form(x63, 20, needs_fp<op_fsub<false>>);
    fill_a_form(x63, 21, needs_fp<op_fadd<false>>);
    fill_a_form(x63, 23, needs_fp<op_fsel>);
    fill_a_form(x63, 25, needs_fp<op_fmul<false>>);
    fill_a_form(x63, 26, needs_fp<op_frsqrte>);
    fill_a_form(x63, 28, needs_fp<op_fmadd<false, true, false>>);   // fmsub
    fill_a_form(x63, 29, needs_fp<op_fmadd<false, false, false>>);  // fmadd
    fill_a_form(x63, 30, needs_fp<op_fmadd<false, true, true>>);    // fnmsub
    fill_a_form(x63, 31, needs_fp<op_fmadd<false, false, true>>);   // fnmadd
    x63[0] = needs_fp<op_fcmpu>;
    x63[12] = needs_fp<op_frsp>;
    x63[14] = needs_fp<op_fctiw>;
    x63[15] = needs_fp<op_fctiwz>;
    x63[32] = needs_fp<op_fcmpo>;
    x63[38] = needs_fp<op_mtfsb1>;
    x63[40] = needs_fp<op_fneg>;
    x63[64] = needs_fp<op_mcrfs>;
    x63[70] = needs_fp<op_mtfsb0>;
    x63[72] = needs_fp<op_fmr>;
    x63[134] = needs_fp<op_mtfsfi>;
    x63[136] = needs_fp<op_fnabs>;
    x63[264] = needs_fp<op_fabs>;
    x63[583] = needs_fp<op_mffs>;
    x63[711] = needs_fp<op_mtfsf>;
}

}  // namespace ezy::gc
