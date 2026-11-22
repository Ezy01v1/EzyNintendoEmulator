// The Gekko's paired singles: two single-precision lanes per FPR (ps0, ps1), arithmetic lane
// by lane (rounded to single, frC rounded to 24 bits in multiplies like the scalar single
// forms), the cross-lane forms (ps_sum*, ps_muls*, ps_madds*, ps_merge*), and the quantized
// loads and stores that convert between memory integers or floats and the lanes using a GQR.
// Sources: the 750CL user manual (paired singles, GQR) and Dolphin's Interpreter_Paired.cpp
// and Interpreter_LoadStorePaired.cpp, matched against hardware, for the details the manual
// leaves open (cited where used). As in Dolphin, the paired arithmetic writes frD even when
// VE/ZE are set, and sets FPRF from ps0 (ps1 for ps_sum1).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

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

const PairedSingle& fra(Gekko& cpu, u32 w) { return fpr(cpu, ra(w)); }
const PairedSingle& frb(Gekko& cpu, u32 w) { return fpr(cpu, rb(w)); }
const PairedSingle& frc(Gekko& cpu, u32 w) { return fpr(cpu, rc_field(w)); }

// A lane copied through (ps_sum's other lane): rounded to single, no exception.
Result copied(double value) { return {value, 0, false}; }

// Writes both lanes rounded to single (in RN's mode: the caller holds a HostRounding),
// negated for ps_nm* (a NaN is not negated), with FPRF from lane `fprf_lane`.
void finish_pair(Gekko& cpu, u32 w, const Result& r0, const Result& r1, int fprf_lane = 0, bool negate = false) {
    GekkoState& s = cpu.state();
    double v0 = force_single(s.fpscr, r0.value);
    double v1 = force_single(s.fpscr, r1.value);
    if (negate) {
        if (!is_nan(v0)) v0 = -v0;
        if (!is_nan(v1)) v1 = -v1;
    }
    note_overflow(s, r0, v0);
    note_overflow(s, r1, v1);
    fpr(cpu, rd(w)) = {v0, v1};
    set_fprf(s, classify_single(fprf_lane == 0 ? v0 : v1));
    record(cpu, w);
}

// ---- Arithmetic --------------------------------------------------------------------------------

void op_ps_add(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w);
    const Result r0 = add(s, a.ps0, b.ps0);
    const Result r1 = add(s, a.ps1, b.ps1);
    finish_pair(cpu, w, r0, r1);
}
void op_ps_sub(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w);
    const Result r0 = sub(s, a.ps0, b.ps0);
    const Result r1 = sub(s, a.ps1, b.ps1);
    finish_pair(cpu, w, r0, r1);
}
void op_ps_div(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w);
    const Result r0 = div(s, a.ps0, b.ps0);
    const Result r1 = div(s, a.ps1, b.ps1);
    finish_pair(cpu, w, r0, r1);
}
// ps_mul: lane by lane. ps_muls0/ps_muls1: both lanes times frC's lane 0 / lane 1.
template <int CLane>  // -1: lane by lane
void op_ps_mul(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), c = frc(cpu, w);
    const double c0 = round_frc(CLane == 1 ? c.ps1 : c.ps0);
    const double c1 = round_frc(CLane == 0 ? c.ps0 : c.ps1);
    const Result r0 = mul(s, a.ps0, c0);
    const Result r1 = mul(s, a.ps1, c1);
    finish_pair(cpu, w, r0, r1);
}
// ps_madd/msub/nmadd/nmsub lane by lane; ps_madds0/1 with frC's lane 0 / lane 1 for both.
template <int CLane, bool Subtract, bool Negate>
void op_ps_madd(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w), c = frc(cpu, w);
    const double c0 = CLane == 1 ? c.ps1 : c.ps0;
    const double c1 = CLane == 0 ? c.ps0 : c.ps1;
    const Result r0 = madd<true>(s, a.ps0, c0, b.ps0, Subtract);
    const Result r1 = madd<true>(s, a.ps1, c1, b.ps1, Subtract);
    finish_pair(cpu, w, r0, r1, 0, Negate);
}
// ps_sum0: ps0 = frA.ps0 + frB.ps1, ps1 = frC.ps1. ps_sum1: ps0 = frC.ps0, ps1 = the sum.
void op_ps_sum0(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w), c = frc(cpu, w);
    finish_pair(cpu, w, add(s, a.ps0, b.ps1), copied(c.ps1));
}
void op_ps_sum1(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w), c = frc(cpu, w);
    finish_pair(cpu, w, copied(c.ps0), add(s, a.ps0, b.ps1), 1);
}

// ps_res/ps_rsqrte: the table estimates per lane. ZX if a lane is 0, VXSQRT if one is
// negative (ps_rsqrte), VXSNAN if one is a signaling NaN (Dolphin's ps_res/ps_rsqrte).
void op_ps_res(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const PairedSingle b = frb(cpu, w);
    if (b.ps0 == 0.0 || b.ps1 == 0.0) set_exception(s, Gekko::kFpscrZx);
    if (is_snan(b.ps0) || is_snan(b.ps1)) set_exception(s, Gekko::kFpscrVxsnan);
    const double v0 = estimate_reciprocal(b.ps0), v1 = estimate_reciprocal(b.ps1);
    fpr(cpu, rd(w)) = {v0, v1};
    set_fprf(s, classify_single(v0));
    record(cpu, w);
}
void op_ps_rsqrte(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const HostRounding rounding(s.fpscr);
    const PairedSingle b = frb(cpu, w);
    if (b.ps0 == 0.0 || b.ps1 == 0.0) set_exception(s, Gekko::kFpscrZx);
    if (b.ps0 < 0.0 || b.ps1 < 0.0) set_exception(s, Gekko::kFpscrVxsqrt);
    if (is_snan(b.ps0) || is_snan(b.ps1)) set_exception(s, Gekko::kFpscrVxsnan);
    const double v0 = force_single(s.fpscr, estimate_reciprocal_sqrt(b.ps0));
    const double v1 = force_single(s.fpscr, estimate_reciprocal_sqrt(b.ps1));
    fpr(cpu, rd(w)) = {v0, v1};
    set_fprf(s, classify_single(v0));
    record(cpu, w);
}

// ---- Moves, selects, merges (bit copies: no rounding, no FPSCR change) -----------------------

template <u64 (*Op)(u64)>
void op_ps_bits(Gekko& cpu, u32 w) {
    const PairedSingle b = frb(cpu, w);
    fpr(cpu, rd(w)) = {from_bits(Op(bits(b.ps0))), from_bits(Op(bits(b.ps1)))};
    record(cpu, w);
}
u64 move_bits(u64 x) { return x; }
u64 negate_bits(u64 x) { return x ^ kSign; }
u64 abs_bits(u64 x) { return x & ~kSign; }
u64 nabs_bits(u64 x) { return x | kSign; }

// ps_sel: per lane, frA >= 0 (-0 included) ? frC : frB; a NaN picks frB.
void op_ps_sel(Gekko& cpu, u32 w) {
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w), c = frc(cpu, w);
    fpr(cpu, rd(w)) = {a.ps0 >= 0.0 ? c.ps0 : b.ps0, a.ps1 >= 0.0 ? c.ps1 : b.ps1};
    record(cpu, w);
}

// ps_mergeXY: ps0 = frA's lane X, ps1 = frB's lane Y.
template <int ALane, int BLane>
void op_ps_merge(Gekko& cpu, u32 w) {
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w);
    fpr(cpu, rd(w)) = {ALane == 0 ? a.ps0 : a.ps1, BLane == 0 ? b.ps0 : b.ps1};
    record(cpu, w);
}

// ps_cmpu0/ps_cmpo0 compare the ps0 lanes, ps_cmpu1/ps_cmpo1 the ps1 lanes, as fcmpu/fcmpo.
template <int Lane, bool Ordered>
void op_ps_cmp(Gekko& cpu, u32 w) {
    const PairedSingle a = fra(cpu, w), b = frb(cpu, w);
    compare(cpu, w, Lane == 0 ? a.ps0 : a.ps1, Lane == 0 ? b.ps0 : b.ps1, Ordered);
}

// ---- Quantized loads and stores ----------------------------------------------------------------

// GQR fields (750CL manual; Dolphin's UGQR), bit 0 = LSB: store type 0-2, store scale 8-13,
// load type 16-18, load scale 24-29. Types: 0 single float, 4 u8, 5 u16, 6 s8, 7 s16
// (1-3 reserved). The scale is signed 6-bit: a load gives value * 2^-scale, a store writes
// value * 2^scale. The float type ignores the scale.
enum : u32 { kQuantFloat = 0, kQuantU8 = 4, kQuantU16 = 5, kQuantS8 = 6, kQuantS16 = 7 };

// Bytes per value of a type. A reserved type stops emulation (the manual leaves it undefined
// and no known code uses it).
u32 quantized_size(Gekko& cpu, u32 w, u32 type) {
    switch (type) {
    case kQuantFloat: return 4;
    case kQuantU8:
    case kQuantS8: return 1;
    case kQuantU16:
    case kQuantS16: return 2;
    default: throw GekkoError("reserved GQR type " + std::to_string(type), cpu.pc(), w);
    }
}
// 2^scale (store) or 2^-scale (load), as a single like the hardware's scaling.
float scale_factor(u32 field, bool load) {
    const int scale = static_cast<int>(field ^ 32u) - 32;
    return std::ldexp(1.0f, load ? -scale : scale);
}

// A memory value (in the low `size` bytes of raw) to a lane. Integers convert exactly to
// single and are scaled in single precision (Dolphin's LoadAndDequantize).
double dequantize(u32 raw, u32 type, float factor) {
    switch (type) {
    case kQuantU8: return static_cast<double>(static_cast<float>(raw & 0xFF) * factor);
    case kQuantS8: return static_cast<double>(static_cast<float>(static_cast<std::int8_t>(raw)) * factor);
    case kQuantU16: return static_cast<double>(static_cast<float>(raw & 0xFFFF) * factor);
    case kQuantS16: return static_cast<double>(static_cast<float>(static_cast<std::int16_t>(raw)) * factor);
    default: return from_bits(single_to_double(raw));  // float: widened exactly, like lfs
    }
}
// A lane to a memory value: the float type narrows like stfs but flushes values below the
// normal range to zero; the integer types convert to single, scale, saturate to the type's
// range and truncate toward zero; NaN stores as 0 (Dolphin's ScaleAndClamp, whose x86
// conversion gives 0 there). The caller holds a HostRounding: the conversion to single
// rounds in FPSCR[RN]'s mode, as in Dolphin, which runs the host in the guest's mode.
u32 quantize(double value, u32 type, float factor) {
    if (type == kQuantFloat) return double_to_single_ftz(bits(value));
    const float scaled = static_cast<float>(value) * factor;
    if (std::isnan(scaled)) return 0;
    float low = 0.0f, high = 0.0f;
    switch (type) {
    case kQuantU8: high = 255.0f; break;
    case kQuantS8: low = -128.0f; high = 127.0f; break;
    case kQuantU16: high = 65535.0f; break;
    default: low = -32768.0f; high = 32767.0f; break;  // s16
    }
    return static_cast<u32>(static_cast<s32>(std::clamp(scaled, low, high)));
}

u64 read_sized(Gekko& cpu, u32 ea, u32 bytes) {
    switch (bytes) {
    case 1: return GekkoOps::read8(cpu, ea);
    case 2: return GekkoOps::read16(cpu, ea);
    case 4: return GekkoOps::read32(cpu, ea);
    default: return GekkoOps::read64(cpu, ea);
    }
}
void write_sized(Gekko& cpu, u32 ea, u32 bytes, u64 value) {
    switch (bytes) {
    case 1: return GekkoOps::write8(cpu, ea, static_cast<std::uint8_t>(value));
    case 2: return GekkoOps::write16(cpu, ea, static_cast<std::uint16_t>(value));
    case 4: return GekkoOps::write32(cpu, ea, static_cast<u32>(value));
    default: return GekkoOps::write64(cpu, ea, value);
    }
}

// psq_l*: one value (W = 1; ps1 = 1.0) or two consecutive ones, through GQR[i]'s load type
// and scale. A pair is one memory access, so a DSI leaves frD (and rA) untouched; an
// unaligned access is split like any other (no alignment exception, as in Dolphin).
void load_quantized(Gekko& cpu, u32 w, u32 ea, bool one, u32 gqr_index) {
    const u32 gqr = cpu.state().gqr[gqr_index];
    const u32 type = (gqr >> 16) & 7;
    const u32 size = quantized_size(cpu, w, type);
    const float factor = scale_factor((gqr >> 24) & 63, true);
    PairedSingle value{};
    if (one) {
        value.ps0 = dequantize(static_cast<u32>(read_sized(cpu, ea, size)), type, factor);
        value.ps1 = 1.0;
    } else {
        const u64 both = read_sized(cpu, ea, 2 * size);
        value.ps0 = dequantize(static_cast<u32>(both >> (8 * size)), type, factor);
        value.ps1 = dequantize(static_cast<u32>(both), type, factor);
    }
    fpr(cpu, rd(w)) = value;
}
// psq_st*: ps0 (W = 1) or both lanes through GQR[i]'s store type and scale, in one access.
void store_quantized(Gekko& cpu, u32 w, u32 ea, bool one, u32 gqr_index) {
    const u32 gqr = cpu.state().gqr[gqr_index];
    const u32 type = gqr & 7;
    const u32 size = quantized_size(cpu, w, type);
    const float factor = scale_factor((gqr >> 8) & 63, false);
    const PairedSingle& source = fpr(cpu, rd(w));
    const HostRounding rounding(cpu.state().fpscr);  // the conversion to single follows RN
    const u64 mask = size == 4 ? 0xFFFFFFFF : (u64{1} << (8 * size)) - 1;
    const u64 first = quantize(source.ps0, type, factor) & mask;
    if (one) return write_sized(cpu, ea, size, first);
    const u64 second = quantize(source.ps1, type, factor) & mask;
    write_sized(cpu, ea, 2 * size, (first << (8 * size)) | second);
}

// D-form fields: W (bit 16), I (17-19), a 12-bit signed displacement. The D-forms are
// illegal unless HID2[LSQE] (750CL manual; Dolphin's psq_l/psq_st check it, the indexed
// forms do not).
void require_lsqe(Gekko& cpu) {
    if ((cpu.state().hid2 & Gekko::kHid2Lsqe) == 0) {
        GekkoOps::raise(cpu, Gekko::Exception::Program, Gekko::kProgramIllegal);
    }
}
u32 psq_d_ea(Gekko& cpu, u32 w, bool update) {
    const auto displacement = static_cast<u32>(static_cast<s32>(w << 20) >> 20);
    const u32 base = update ? cpu.state().gpr[static_cast<std::size_t>(ra(w))] : gpr_or_zero(cpu, ra(w));
    return base + displacement;
}
bool d_one(u32 w) { return ((w >> 15) & 1) != 0; }
u32 d_gqr(u32 w) { return (w >> 12) & 7; }
// Indexed fields (opcode 4): W (bit 21), I (22-24).
bool x_one(u32 w) { return ((w >> 10) & 1) != 0; }
u32 x_gqr(u32 w) { return (w >> 7) & 7; }

template <bool Update>
void op_psq_l(Gekko& cpu, u32 w) {
    require_lsqe(cpu);
    const u32 ea = psq_d_ea(cpu, w, Update);
    load_quantized(cpu, w, ea, d_one(w), d_gqr(w));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
template <bool Update>
void op_psq_st(Gekko& cpu, u32 w) {
    require_lsqe(cpu);
    const u32 ea = psq_d_ea(cpu, w, Update);
    store_quantized(cpu, w, ea, d_one(w), d_gqr(w));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
template <bool Update>
void op_psq_lx(Gekko& cpu, u32 w) {
    const u32 ea = Update ? ea_xu(cpu, w) : ea_x(cpu, w);
    load_quantized(cpu, w, ea, x_one(w), x_gqr(w));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}
template <bool Update>
void op_psq_stx(Gekko& cpu, u32 w) {
    const u32 ea = Update ? ea_xu(cpu, w) : ea_x(cpu, w);
    store_quantized(cpu, w, ea, x_one(w), x_gqr(w));
    if constexpr (Update) cpu.state().gpr[static_cast<std::size_t>(ra(w))] = ea;
}

}  // namespace

void install_paired_ops(GekkoDecodeTables& t) {
    t.primary[56] = needs_fp<op_psq_l<false>>;
    t.primary[57] = needs_fp<op_psq_l<true>>;
    t.primary[60] = needs_fp<op_psq_st<false>>;
    t.primary[61] = needs_fp<op_psq_st<true>>;

    auto& x4 = t.op4;
    fill_a_form(x4, 10, needs_fp<op_ps_sum0>);
    fill_a_form(x4, 11, needs_fp<op_ps_sum1>);
    fill_a_form(x4, 12, needs_fp<op_ps_mul<0>>);                   // ps_muls0
    fill_a_form(x4, 13, needs_fp<op_ps_mul<1>>);                   // ps_muls1
    fill_a_form(x4, 14, needs_fp<op_ps_madd<0, false, false>>);    // ps_madds0
    fill_a_form(x4, 15, needs_fp<op_ps_madd<1, false, false>>);    // ps_madds1
    fill_a_form(x4, 18, needs_fp<op_ps_div>);
    fill_a_form(x4, 20, needs_fp<op_ps_sub>);
    fill_a_form(x4, 21, needs_fp<op_ps_add>);
    fill_a_form(x4, 23, needs_fp<op_ps_sel>);
    fill_a_form(x4, 24, needs_fp<op_ps_res>);
    fill_a_form(x4, 25, needs_fp<op_ps_mul<-1>>);                  // ps_mul
    fill_a_form(x4, 26, needs_fp<op_ps_rsqrte>);
    fill_a_form(x4, 28, needs_fp<op_ps_madd<-1, true, false>>);    // ps_msub
    fill_a_form(x4, 29, needs_fp<op_ps_madd<-1, false, false>>);   // ps_madd
    fill_a_form(x4, 30, needs_fp<op_ps_madd<-1, true, true>>);     // ps_nmsub
    fill_a_form(x4, 31, needs_fp<op_ps_madd<-1, false, true>>);    // ps_nmadd
    x4[0] = needs_fp<op_ps_cmp<0, false>>;    // ps_cmpu0
    x4[32] = needs_fp<op_ps_cmp<0, true>>;    // ps_cmpo0
    x4[64] = needs_fp<op_ps_cmp<1, false>>;   // ps_cmpu1
    x4[96] = needs_fp<op_ps_cmp<1, true>>;    // ps_cmpo1
    x4[40] = needs_fp<op_ps_bits<negate_bits>>;  // ps_neg
    x4[72] = needs_fp<op_ps_bits<move_bits>>;    // ps_mr
    x4[136] = needs_fp<op_ps_bits<nabs_bits>>;   // ps_nabs
    x4[264] = needs_fp<op_ps_bits<abs_bits>>;    // ps_abs
    x4[528] = needs_fp<op_ps_merge<0, 0>>;
    x4[560] = needs_fp<op_ps_merge<0, 1>>;
    x4[592] = needs_fp<op_ps_merge<1, 0>>;
    x4[624] = needs_fp<op_ps_merge<1, 1>>;
    // Indexed quantized loads/stores: 6-bit extended opcode in bits 25-30, W and I above it.
    for (int wi = 0; wi < 16; ++wi) {
        const auto slot = [wi](int xo) { return static_cast<std::size_t>((wi << 6) | xo); };
        x4[slot(6)] = needs_fp<op_psq_lx<false>>;
        x4[slot(7)] = needs_fp<op_psq_stx<false>>;
        x4[slot(38)] = needs_fp<op_psq_lx<true>>;
        x4[slot(39)] = needs_fp<op_psq_stx<true>>;
    }
    // x4[1014] is dcbz_l (gekko_integer.cpp): a cache instruction, no MSR[FP] check.
}

}  // namespace ezy::gc
