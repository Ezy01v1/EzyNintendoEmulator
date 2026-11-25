// The Gekko's integer side: arithmetic, logic, rotates, compares, branches, loads and
// stores, condition register, SPR, system and cache instructions. Semantics from the PEM
// (chapter 8, instruction set) unless a comment says otherwise.

#include <array>
#include <bit>
#include <cstdint>

#include "gc/gekko/gekko.hpp"
#include "gc/gekko/gekko_ops.hpp"

namespace ezy::gc {

namespace {

using namespace ppc_fields;
using u32 = std::uint32_t;
using s32 = std::int32_t;

u32& reg(Gekko& cpu, int index) { return cpu.state().gpr[static_cast<std::size_t>(index)]; }
// (rA|0): register 0 reads as zero in address and addi/addis computations.
u32 ra_or_zero(Gekko& cpu, u32 w) { return ra(w) == 0 ? 0 : reg(cpu, ra(w)); }

void set_ca(Gekko& cpu, bool carry) {
    u32& xer = cpu.state().xer;
    xer = carry ? (xer | Gekko::kXerCa) : (xer & ~Gekko::kXerCa);
}
// OE = 1: OV = overflow, and SO accumulates it.
void set_ov(Gekko& cpu, bool overflow) {
    u32& xer = cpu.state().xer;
    xer = overflow ? (xer | Gekko::kXerOv | Gekko::kXerSo) : (xer & ~Gekko::kXerOv);
}
bool carry_in(Gekko& cpu) { return (cpu.state().xer & Gekko::kXerCa) != 0; }

// One CR field (0..7) = value (LT GT EQ SO from bit 3 down).
void set_cr_field(Gekko& cpu, int field, u32 value) {
    const int shift = 28 - 4 * field;
    u32& cr = cpu.state().cr;
    cr = (cr & ~(0xFu << shift)) | ((value & 0xF) << shift);
}
// A compare result for a CR field, with XER[SO] copied in.
u32 compare_field(Gekko& cpu, bool less, bool greater) {
    return (less ? 8u : greater ? 4u : 2u) | (cpu.state().xer >> 31);
}

// x + y + carry with the carry out and the signed overflow. Every add/subtract form is one
// of these: subtract-from computes ~rA + rB + 1.
struct Sum {
    u32 value;
    bool carry;
    bool overflow;
};
Sum add3(u32 x, u32 y, bool carry) {
    const std::uint64_t wide = std::uint64_t{x} + y + (carry ? 1u : 0u);
    const auto value = static_cast<u32>(wide);
    return {value, (wide >> 32) != 0, (((x ^ value) & (y ^ value)) >> 31) != 0};
}

// The end of every XO-form instruction: rD, then OV/SO if OE, then CR0 if Rc.
void finish_xo(Gekko& cpu, u32 w, u32 value, bool overflow) {
    reg(cpu, rd(w)) = value;
    if (oe(w)) set_ov(cpu, overflow);
    if (rc(w)) cpu.update_cr0(value);
}
// Logic and rotates write rA and record into CR0 if Rc.
void finish_logic(Gekko& cpu, u32 w, u32 value) {
    reg(cpu, ra(w)) = value;
    if (rc(w)) cpu.update_cr0(value);
}

u32 rotl(u32 value, int amount) { return std::rotl(value, amount); }
// The rotate mask from MB to ME (bit 0 = MSB), wrapping around when MB > ME.
u32 rotate_mask(int mb_, int me_) {
    const u32 from_mb = 0xFFFFFFFFu >> mb_;
    const u32 to_me = 0xFFFFFFFFu << (31 - me_);
    return mb_ <= me_ ? (from_mb & to_me) : (from_mb | to_me);
}

// ---- Arithmetic --------------------------------------------------------------------------

void op_addi(Gekko& cpu, u32 w) { reg(cpu, rd(w)) = ra_or_zero(cpu, w) + simm(w); }
void op_addis(Gekko& cpu, u32 w) { reg(cpu, rd(w)) = ra_or_zero(cpu, w) + (uimm(w) << 16); }
void op_addic(Gekko& cpu, u32 w) {
    const Sum sum = add3(reg(cpu, ra(w)), simm(w), false);
    reg(cpu, rd(w)) = sum.value;
    set_ca(cpu, sum.carry);
}
void op_addic_rc(Gekko& cpu, u32 w) {
    op_addic(cpu, w);
    cpu.update_cr0(reg(cpu, rd(w)));
}
void op_subfic(Gekko& cpu, u32 w) {
    const Sum sum = add3(~reg(cpu, ra(w)), simm(w), true);
    reg(cpu, rd(w)) = sum.value;
    set_ca(cpu, sum.carry);
}
void op_mulli(Gekko& cpu, u32 w) {
    reg(cpu, rd(w)) = static_cast<u32>(static_cast<s32>(reg(cpu, ra(w))) * static_cast<s32>(simm(w)));
}

void op_add(Gekko& cpu, u32 w) {
    const Sum sum = add3(reg(cpu, ra(w)), reg(cpu, rb(w)), false);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_addc(Gekko& cpu, u32 w) {
    const Sum sum = add3(reg(cpu, ra(w)), reg(cpu, rb(w)), false);
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_adde(Gekko& cpu, u32 w) {
    const Sum sum = add3(reg(cpu, ra(w)), reg(cpu, rb(w)), carry_in(cpu));
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_addze(Gekko& cpu, u32 w) {
    const Sum sum = add3(reg(cpu, ra(w)), 0, carry_in(cpu));
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_addme(Gekko& cpu, u32 w) {
    const Sum sum = add3(reg(cpu, ra(w)), 0xFFFFFFFF, carry_in(cpu));
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_subf(Gekko& cpu, u32 w) {
    const Sum sum = add3(~reg(cpu, ra(w)), reg(cpu, rb(w)), true);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_subfc(Gekko& cpu, u32 w) {
    const Sum sum = add3(~reg(cpu, ra(w)), reg(cpu, rb(w)), true);
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_subfe(Gekko& cpu, u32 w) {
    const Sum sum = add3(~reg(cpu, ra(w)), reg(cpu, rb(w)), carry_in(cpu));
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_subfze(Gekko& cpu, u32 w) {
    const Sum sum = add3(~reg(cpu, ra(w)), 0, carry_in(cpu));
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_subfme(Gekko& cpu, u32 w) {
    const Sum sum = add3(~reg(cpu, ra(w)), 0xFFFFFFFF, carry_in(cpu));
    set_ca(cpu, sum.carry);
    finish_xo(cpu, w, sum.value, sum.overflow);
}
void op_neg(Gekko& cpu, u32 w) {
    // ~rA + 1 without touching CA; overflows only for 0x80000000.
    const Sum sum = add3(~reg(cpu, ra(w)), 0, true);
    finish_xo(cpu, w, sum.value, sum.overflow);
}

void op_mullw(Gekko& cpu, u32 w) {
    const std::int64_t product =
        std::int64_t{static_cast<s32>(reg(cpu, ra(w)))} * static_cast<s32>(reg(cpu, rb(w)));
    finish_xo(cpu, w, static_cast<u32>(product), product < INT32_MIN || product > INT32_MAX);
}
void op_mulhw(Gekko& cpu, u32 w) {
    const std::int64_t product =
        std::int64_t{static_cast<s32>(reg(cpu, ra(w)))} * static_cast<s32>(reg(cpu, rb(w)));
    finish_xo(cpu, w & ~0x400u, static_cast<u32>(static_cast<std::uint64_t>(product) >> 32), false);
}
void op_mulhwu(Gekko& cpu, u32 w) {
    const std::uint64_t product = std::uint64_t{reg(cpu, ra(w))} * reg(cpu, rb(w));
    finish_xo(cpu, w & ~0x400u, static_cast<u32>(product >> 32), false);
}

// Division by zero and 0x80000000 / -1 leave an undefined rD in the PEM. The Gekko gives
// 0xFFFFFFFF for a negative dividend and 0 otherwise (signed), and 0 (unsigned): Dolphin's
// Interpreter_Integer.cpp divwx/divwux, which match hardware tests.
void op_divw(Gekko& cpu, u32 w) {
    const auto a = static_cast<s32>(reg(cpu, ra(w)));
    const auto b = static_cast<s32>(reg(cpu, rb(w)));
    const bool overflow = b == 0 || (a == INT32_MIN && b == -1);
    const u32 value = overflow ? (a < 0 ? 0xFFFFFFFFu : 0u) : static_cast<u32>(a / b);
    finish_xo(cpu, w, value, overflow);
}
void op_divwu(Gekko& cpu, u32 w) {
    const u32 a = reg(cpu, ra(w)), b = reg(cpu, rb(w));
    finish_xo(cpu, w, b == 0 ? 0 : a / b, b == 0);
}

// ---- Compares and traps ------------------------------------------------------------------

void op_cmpi(Gekko& cpu, u32 w) {
    const auto a = static_cast<s32>(reg(cpu, ra(w))), b = static_cast<s32>(simm(w));
    set_cr_field(cpu, crfd(w), compare_field(cpu, a < b, a > b));
}
void op_cmpli(Gekko& cpu, u32 w) {
    const u32 a = reg(cpu, ra(w)), b = uimm(w);
    set_cr_field(cpu, crfd(w), compare_field(cpu, a < b, a > b));
}
void op_cmp(Gekko& cpu, u32 w) {
    const auto a = static_cast<s32>(reg(cpu, ra(w))), b = static_cast<s32>(reg(cpu, rb(w)));
    set_cr_field(cpu, crfd(w), compare_field(cpu, a < b, a > b));
}
void op_cmpl(Gekko& cpu, u32 w) {
    const u32 a = reg(cpu, ra(w)), b = reg(cpu, rb(w));
    set_cr_field(cpu, crfd(w), compare_field(cpu, a < b, a > b));
}

// TO bits: 16 signed less, 8 signed greater, 4 equal, 2 unsigned less, 1 unsigned greater.
void trap_if(Gekko& cpu, u32 w, u32 a, u32 b) {
    const int to = rd(w);
    const auto sa = static_cast<s32>(a), sb = static_cast<s32>(b);
    if (((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) || ((to & 2) && a < b) ||
        ((to & 1) && a > b))
        GekkoOps::raise(cpu, Gekko::Exception::Program, Gekko::kProgramTrap);
}
void op_twi(Gekko& cpu, u32 w) { trap_if(cpu, w, reg(cpu, ra(w)), simm(w)); }
void op_tw(Gekko& cpu, u32 w) { trap_if(cpu, w, reg(cpu, ra(w)), reg(cpu, rb(w))); }

// ---- Logic, rotates and shifts -------------------------------------------------------------

void op_ori(Gekko& cpu, u32 w) { reg(cpu, ra(w)) = reg(cpu, rd(w)) | uimm(w); }
void op_oris(Gekko& cpu, u32 w) { reg(cpu, ra(w)) = reg(cpu, rd(w)) | (uimm(w) << 16); }
void op_xori(Gekko& cpu, u32 w) { reg(cpu, ra(w)) = reg(cpu, rd(w)) ^ uimm(w); }
void op_xoris(Gekko& cpu, u32 w) { reg(cpu, ra(w)) = reg(cpu, rd(w)) ^ (uimm(w) << 16); }
void op_andi_rc(Gekko& cpu, u32 w) {
    reg(cpu, ra(w)) = reg(cpu, rd(w)) & uimm(w);
    cpu.update_cr0(reg(cpu, ra(w)));
}
void op_andis_rc(Gekko& cpu, u32 w) {
    reg(cpu, ra(w)) = reg(cpu, rd(w)) & (uimm(w) << 16);
    cpu.update_cr0(reg(cpu, ra(w)));
}

void op_and(Gekko& cpu, u32 w) { finish_logic(cpu, w, reg(cpu, rd(w)) & reg(cpu, rb(w))); }
void op_andc(Gekko& cpu, u32 w) { finish_logic(cpu, w, reg(cpu, rd(w)) & ~reg(cpu, rb(w))); }
void op_or(Gekko& cpu, u32 w) { finish_logic(cpu, w, reg(cpu, rd(w)) | reg(cpu, rb(w))); }
void op_orc(Gekko& cpu, u32 w) { finish_logic(cpu, w, reg(cpu, rd(w)) | ~reg(cpu, rb(w))); }
void op_xor(Gekko& cpu, u32 w) { finish_logic(cpu, w, reg(cpu, rd(w)) ^ reg(cpu, rb(w))); }
void op_nand(Gekko& cpu, u32 w) { finish_logic(cpu, w, ~(reg(cpu, rd(w)) & reg(cpu, rb(w)))); }
void op_nor(Gekko& cpu, u32 w) { finish_logic(cpu, w, ~(reg(cpu, rd(w)) | reg(cpu, rb(w)))); }
void op_eqv(Gekko& cpu, u32 w) { finish_logic(cpu, w, ~(reg(cpu, rd(w)) ^ reg(cpu, rb(w)))); }
void op_extsb(Gekko& cpu, u32 w) {
    finish_logic(cpu, w, static_cast<u32>(static_cast<s32>(static_cast<std::int8_t>(reg(cpu, rd(w))))));
}
void op_extsh(Gekko& cpu, u32 w) {
    finish_logic(cpu, w, static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(reg(cpu, rd(w))))));
}
void op_cntlzw(Gekko& cpu, u32 w) { finish_logic(cpu, w, static_cast<u32>(std::countl_zero(reg(cpu, rd(w))))); }

void op_rlwimi(Gekko& cpu, u32 w) {
    const u32 mask = rotate_mask(mb(w), me(w));
    finish_logic(cpu, w, (rotl(reg(cpu, rd(w)), rb(w)) & mask) | (reg(cpu, ra(w)) & ~mask));
}
void op_rlwinm(Gekko& cpu, u32 w) { finish_logic(cpu, w, rotl(reg(cpu, rd(w)), rb(w)) & rotate_mask(mb(w), me(w))); }
void op_rlwnm(Gekko& cpu, u32 w) {
    const int amount = static_cast<int>(reg(cpu, rb(w)) & 31);
    finish_logic(cpu, w, rotl(reg(cpu, rd(w)), amount) & rotate_mask(mb(w), me(w)));
}

// Shift amounts are the low 6 bits of rB: 32..63 shift everything out.
void op_slw(Gekko& cpu, u32 w) {
    const u32 amount = reg(cpu, rb(w)) & 63;
    finish_logic(cpu, w, amount >= 32 ? 0 : reg(cpu, rd(w)) << amount);
}
void op_srw(Gekko& cpu, u32 w) {
    const u32 amount = reg(cpu, rb(w)) & 63;
    finish_logic(cpu, w, amount >= 32 ? 0 : reg(cpu, rd(w)) >> amount);
}
// Arithmetic shift right: CA = the source is negative and a 1 bit was shifted out.
void shift_right_algebraic(Gekko& cpu, u32 w, u32 amount) {
    const auto value = static_cast<s32>(reg(cpu, rd(w)));
    if (amount >= 32) {
        set_ca(cpu, value < 0);
        finish_logic(cpu, w, value < 0 ? 0xFFFFFFFFu : 0u);
        return;
    }
    const u32 lost = amount == 0 ? 0 : static_cast<u32>(value) & ((1u << amount) - 1);
    set_ca(cpu, value < 0 && lost != 0);
    finish_logic(cpu, w, static_cast<u32>(value >> amount));
}
void op_sraw(Gekko& cpu, u32 w) { shift_right_algebraic(cpu, w, reg(cpu, rb(w)) & 63); }
void op_srawi(Gekko& cpu, u32 w) { shift_right_algebraic(cpu, w, static_cast<u32>(rb(w))); }

// ---- Branches --------------------------------------------------------------------------------

// BO: 16 = ignore the condition, 8 = the condition value to branch on, 4 = do not decrement
// CTR, 2 = branch when CTR == 0 (else when != 0); 1 is a prediction hint.
bool branch_condition(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const int bo = rd(w);
    if ((bo & 4) == 0) --s.ctr;
    const bool ctr_ok = (bo & 4) != 0 || ((s.ctr == 0) == ((bo & 2) != 0));
    const bool cond_ok = (bo & 16) != 0 || (((s.cr >> (31 - ra(w))) & 1) == static_cast<u32>((bo >> 3) & 1));
    return ctr_ok && cond_ok;
}

void op_b(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const u32 offset = static_cast<u32>(static_cast<s32>(w << 6) >> 6) & ~3u;
    s.npc = aa(w) ? offset : s.pc + offset;
    if (rc(w)) s.lr = s.pc + 4;
}
void op_bc(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const bool taken = branch_condition(cpu, w);
    if (taken) {
        const u32 offset = simm(w) & ~3u;
        s.npc = aa(w) ? offset : s.pc + offset;
    }
    if (rc(w)) s.lr = s.pc + 4;
}
void op_bclr(Gekko& cpu, u32 w) {
    GekkoState& s = cpu.state();
    const u32 target = s.lr & ~3u;  // read before LK replaces it
    if (branch_condition(cpu, w)) s.npc = target;
    if (rc(w)) s.lr = s.pc + 4;
}
void op_bcctr(Gekko& cpu, u32 w) {
    // bcctr with BO[2] = 0 (decrement CTR) is an invalid form whose result the PEM leaves
    // undefined, and compilers never emit it: stop rather than guess (policy in gekko_ops.hpp;
    // Dolphin only asserts and ignores the CTR part).
    if ((rd(w) & 4) == 0) cpu.unimplemented(w);
    GekkoState& s = cpu.state();
    if (branch_condition(cpu, w)) s.npc = s.ctr & ~3u;
    if (rc(w)) s.lr = s.pc + 4;
}

// ---- Condition register ------------------------------------------------------------------------

void op_mcrf(Gekko& cpu, u32 w) { set_cr_field(cpu, crfd(w), cpu.state().cr >> (28 - 4 * crfs(w))); }
// CR bit operations: crbD = op(crbA, crbB).
template <typename Fn>
void cr_logic(Gekko& cpu, u32 w, Fn fn) {
    u32& cr = cpu.state().cr;
    const bool a = ((cr >> (31 - ra(w))) & 1) != 0, b = ((cr >> (31 - rb(w))) & 1) != 0;
    const u32 bit = 1u << (31 - rd(w));
    cr = fn(a, b) ? (cr | bit) : (cr & ~bit);
}
void op_crand(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return a && b; }); }
void op_crandc(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return a && !b; }); }
void op_cror(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return a || b; }); }
void op_crorc(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return a || !b; }); }
void op_crxor(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return a != b; }); }
void op_crnand(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return !(a && b); }); }
void op_crnor(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return !(a || b); }); }
void op_creqv(Gekko& cpu, u32 w) { cr_logic(cpu, w, [](bool a, bool b) { return a == b; }); }

void op_mfcr(Gekko& cpu, u32 w) { reg(cpu, rd(w)) = cpu.state().cr; }
void op_mtcrf(Gekko& cpu, u32 w) {
    const u32 crm = (w >> 12) & 0xFF;
    u32 mask = 0;
    for (int field = 0; field < 8; ++field)
        if ((crm & (0x80u >> field)) != 0) mask |= 0xFu << (28 - 4 * field);
    u32& cr = cpu.state().cr;
    cr = (reg(cpu, rd(w)) & mask) | (cr & ~mask);
}
void op_mcrxr(Gekko& cpu, u32 w) {
    u32& xer = cpu.state().xer;
    set_cr_field(cpu, crfd(w), xer >> 28);
    xer &= 0x0FFFFFFF;
}

// ---- Loads and stores ------------------------------------------------------------------------

// Effective addresses: ea_d, ea_x, ea_du, ea_xu (gekko_ops.hpp, with the invalid update forms).

// C++20 has no std::byteswap yet.
u32 swap32(u32 value) {
    return (value >> 24) | ((value >> 8) & 0xFF00) | ((value << 8) & 0xFF0000) | (value << 24);
}
std::uint16_t swap16(std::uint16_t value) { return static_cast<std::uint16_t>((value >> 8) | (value << 8)); }
u32 sign16(std::uint16_t value) { return static_cast<u32>(static_cast<s32>(static_cast<std::int16_t>(value))); }

// The loaded value is computed before any register changes, so a DSI leaves rD and rA alone.
template <u32 (*Ea)(Gekko&, u32), u32 (*Load)(Gekko&, u32), bool Update>
void load_op(Gekko& cpu, u32 w) {
    const u32 ea = Ea(cpu, w);
    const u32 value = Load(cpu, ea);
    reg(cpu, rd(w)) = value;
    if constexpr (Update) reg(cpu, ra(w)) = ea;
}
template <u32 (*Ea)(Gekko&, u32), void (*Store)(Gekko&, u32, u32), bool Update>
void store_op(Gekko& cpu, u32 w) {
    const u32 ea = Ea(cpu, w);
    Store(cpu, ea, reg(cpu, rd(w)));
    if constexpr (Update) reg(cpu, ra(w)) = ea;
}

u32 load_word(Gekko& cpu, u32 ea) { return GekkoOps::read32(cpu, ea); }
u32 load_half(Gekko& cpu, u32 ea) { return GekkoOps::read16(cpu, ea); }
u32 load_half_algebraic(Gekko& cpu, u32 ea) { return sign16(GekkoOps::read16(cpu, ea)); }
u32 load_byte(Gekko& cpu, u32 ea) { return GekkoOps::read8(cpu, ea); }
u32 load_word_reversed(Gekko& cpu, u32 ea) { return swap32(GekkoOps::read32(cpu, ea)); }
u32 load_half_reversed(Gekko& cpu, u32 ea) { return swap16(GekkoOps::read16(cpu, ea)); }
void store_word(Gekko& cpu, u32 ea, u32 value) { GekkoOps::write32(cpu, ea, value); }
void store_half(Gekko& cpu, u32 ea, u32 value) { GekkoOps::write16(cpu, ea, static_cast<std::uint16_t>(value)); }
void store_byte(Gekko& cpu, u32 ea, u32 value) { GekkoOps::write8(cpu, ea, static_cast<std::uint8_t>(value)); }
void store_word_reversed(Gekko& cpu, u32 ea, u32 value) { GekkoOps::write32(cpu, ea, swap32(value)); }
void store_half_reversed(Gekko& cpu, u32 ea, u32 value) {
    GekkoOps::write16(cpu, ea, swap16(static_cast<std::uint16_t>(value)));
}

// lmw/stmw: rD..r31, word-aligned only (an unaligned address raises alignment on the 750).
// lmw with rA among the loaded registers (an invalid form) uses the EA computed first and
// leaves rA with its loaded word, as Dolphin's lmw does.
void op_lmw(Gekko& cpu, u32 w) {
    const u32 ea = ea_d(cpu, w);
    if ((ea & 3) != 0) GekkoOps::raise_alignment(cpu, ea, w);
    std::array<u32, 32> words{};
    const int first = rd(w);
    for (int r = first; r < 32; ++r) words[static_cast<std::size_t>(r)] = GekkoOps::read32(cpu, ea + 4 * static_cast<u32>(r - first));
    for (int r = first; r < 32; ++r) reg(cpu, r) = words[static_cast<std::size_t>(r)];
}
void op_stmw(Gekko& cpu, u32 w) {
    const u32 ea = ea_d(cpu, w);
    if ((ea & 3) != 0) GekkoOps::raise_alignment(cpu, ea, w);
    const int first = rd(w);
    for (int r = first; r < 32; ++r) GekkoOps::write32(cpu, ea + 4 * static_cast<u32>(r - first), reg(cpu, r));
}

// String loads: `count` bytes into rD, rD+1... (wrapping r31 -> r0), 4 per register from
// the high byte; the last register's unused low bytes become 0. All bytes are read first.
// rA or rB among the loaded registers (an invalid form) works like lmw: the EA comes first.
void load_string(Gekko& cpu, u32 w, u32 ea, u32 count) {
    std::array<std::uint8_t, 128> bytes{};
    for (u32 i = 0; i < count; ++i) bytes[i] = GekkoOps::read8(cpu, ea + i);
    int r = rd(w);
    for (u32 i = 0; i < count; i += 4) {
        u32 value = 0;
        for (u32 j = 0; j < 4; ++j) value = (value << 8) | (i + j < count ? bytes[i + j] : 0u);
        reg(cpu, r) = value;
        r = (r + 1) & 31;
    }
}
void store_string(Gekko& cpu, u32 w, u32 ea, u32 count) {
    int r = rd(w);
    for (u32 i = 0; i < count; ++i) {
        GekkoOps::write8(cpu, ea + i, static_cast<std::uint8_t>(reg(cpu, r) >> (24 - 8 * (i & 3))));
        if ((i & 3) == 3) r = (r + 1) & 31;
    }
}
void op_lswi(Gekko& cpu, u32 w) { load_string(cpu, w, ra_or_zero(cpu, w), rb(w) == 0 ? 32u : static_cast<u32>(rb(w))); }
void op_stswi(Gekko& cpu, u32 w) {
    store_string(cpu, w, ra_or_zero(cpu, w), rb(w) == 0 ? 32u : static_cast<u32>(rb(w)));
}
// The indexed forms take the count from XER[25-31]; 0 moves nothing.
void op_lswx(Gekko& cpu, u32 w) { load_string(cpu, w, ea_x(cpu, w), cpu.state().xer & 0x7F); }
void op_stswx(Gekko& cpu, u32 w) { store_string(cpu, w, ea_x(cpu, w), cpu.state().xer & 0x7F); }

void op_lwarx(Gekko& cpu, u32 w) {
    const u32 ea = ea_x(cpu, w);
    if ((ea & 3) != 0) GekkoOps::raise_alignment(cpu, ea, w);
    const u32 value = GekkoOps::read32(cpu, ea);
    GekkoState& s = cpu.state();
    reg(cpu, rd(w)) = value;
    s.reservation = true;
    s.reservation_address = ea;
}
// Stores only while the reservation lwarx made still holds and the EA is the one lwarx
// used; CR0 = EQ if it stored, + SO. A store clears the reservation; a different address
// stores nothing and keeps it. This is a Dolphin-parity choice (its stwcxd compares the
// exact EA) that differs from the PEM: there the reservation is cleared either way, and
// only whether the store happens to a different address is undefined. (The 750's
// reservation granule is also a 32-byte block, not the exact EA.) Nothing observed relies on
// the difference. The Rc = 0 form (invalid) shares the table slot and behaves the same,
// setting CR0, as in Dolphin.
void op_stwcx_rc(Gekko& cpu, u32 w) {
    const u32 ea = ea_x(cpu, w);
    if ((ea & 3) != 0) GekkoOps::raise_alignment(cpu, ea, w);
    GekkoState& s = cpu.state();
    const bool stored = s.reservation && s.reservation_address == ea;
    if (stored) {
        GekkoOps::write32(cpu, ea, reg(cpu, rd(w)));
        s.reservation = false;
    }
    set_cr_field(cpu, 0, (stored ? 2u : 0u) | (s.xer >> 31));
}

// ---- Caches and synchronization ------------------------------------------------------------------

// dcbz: the 32-byte block holding the address becomes zero (a store for translation).
void zero_block(Gekko& cpu, u32 ea) {
    const u32 block = ea & ~31u;
    for (u32 i = 0; i < 32; i += 8) GekkoOps::write64(cpu, block + i, 0);
}
// With the data cache disabled (HID0[DCE] = 0) the 750 cannot allocate the line and raises
// alignment instead (750 manual, alignment exception causes; Dolphin's dcbz).
void op_dcbz(Gekko& cpu, u32 w) {
    const u32 ea = ea_x(cpu, w);
    if ((cpu.state().hid0 & Gekko::kHid0Dce) == 0) GekkoOps::raise_alignment(cpu, ea, w);
    zero_block(cpu, ea);
}
// dcbz_l: allocates a zeroed line in the locked cache, which the memory map places at the
// address (0xE0000000 under the boot BATs). Illegal unless HID2[LCE]; then alignment while
// HID0[DCE] = 0, like dcbz (Dolphin's Interpreter_LoadStore.cpp dcbz_l, same order).
void op_dcbz_l(Gekko& cpu, u32 w) {
    if ((cpu.state().hid2 & Gekko::kHid2Lce) == 0) GekkoOps::raise(cpu, Gekko::Exception::Program, Gekko::kProgramIllegal);
    const u32 ea = ea_x(cpu, w);
    if ((cpu.state().hid0 & Gekko::kHid0Dce) == 0) GekkoOps::raise_alignment(cpu, ea, w);
    zero_block(cpu, ea);
}
// No caches, store buffers or TLB are modeled: memory is always coherent, so these change
// nothing. (icbi will matter to a JIT: it is where compiled code gets invalidated.)
void op_cache_no_effect(Gekko&, u32) {}
void op_supervisor_no_effect(Gekko& cpu, u32) { GekkoOps::require_supervisor(cpu); }  // dcbi, tlbie, tlbsync

// ---- System --------------------------------------------------------------------------------------

void op_sc(Gekko& cpu, u32) { GekkoOps::raise(cpu, Gekko::Exception::Syscall); }
void op_rfi(Gekko& cpu, u32) {
    GekkoOps::require_supervisor(cpu);
    // MSR bits 0, 5-9 and 16-31 come back from SRR1, then POW (bit 13) is cleared: Dolphin's
    // Interpreter_Branch.cpp rfi (mask 0x87C0FFFF), matching what exception entry saves.
    GekkoState& s = cpu.state();
    constexpr u32 kMask = 0x87C0FFFF;
    s.msr = ((s.msr & ~kMask) | (s.srr1 & kMask)) & ~Gekko::kMsrPow;
    s.npc = s.srr0 & ~3u;
}
void op_mfmsr(Gekko& cpu, u32 w) {
    GekkoOps::require_supervisor(cpu);
    reg(cpu, rd(w)) = cpu.state().msr;
}
// Every MSR bit is stored. EE, PR, FP, ME, IP, IR, DR and RI act. Accepted, no effect: POW
// (power management: the CPU never dozes here), ILE/LE (little-endian mode: the GameCube
// runs big-endian only), SE/BE (trace exceptions are not modeled) and FE0/FE1 (enabled FP
// exceptions are not modeled: games run with them off).
void op_mtmsr(Gekko& cpu, u32 w) {
    GekkoOps::require_supervisor(cpu);
    cpu.state().msr = reg(cpu, rd(w));  // an interrupt it enables is taken before the next instruction
}

// SPRs with bit 4 of their number set (0x10) are supervisor-only (PEM 8, mfspr/mtspr).
void check_spr_privilege(Gekko& cpu, int number) {
    if ((number & 0x10) != 0) GekkoOps::require_supervisor(cpu);
}
void op_mfspr(Gekko& cpu, u32 w) {
    const int number = spr_number(w);
    check_spr_privilege(cpu, number);
    u32 value = 0;
    if (!cpu.try_read_spr(number, value))
        throw GekkoError("mfspr of unknown SPR " + std::to_string(number), cpu.pc(), w);
    reg(cpu, rd(w)) = value;
}
void op_mtspr(Gekko& cpu, u32 w) {
    const int number = spr_number(w);
    check_spr_privilege(cpu, number);
    if (!cpu.try_write_spr(number, reg(cpu, rd(w))))
        throw GekkoError("mtspr to unknown or read-only SPR " + std::to_string(number), cpu.pc(), w);
}
void op_mftb(Gekko& cpu, u32 w) {
    const int number = spr_number(w);
    const std::uint64_t tb = cpu.state().tb;
    if (number == spr::kTbl) reg(cpu, rd(w)) = static_cast<u32>(tb);
    else if (number == spr::kTbu) reg(cpu, rd(w)) = static_cast<u32>(tb >> 32);
    else throw GekkoError("mftb of unknown TBR " + std::to_string(number), cpu.pc(), w);
}
void op_mfsr(Gekko& cpu, u32 w) {
    GekkoOps::require_supervisor(cpu);
    reg(cpu, rd(w)) = cpu.state().sr[static_cast<std::size_t>(ra(w) & 15)];
}
void op_mtsr(Gekko& cpu, u32 w) {
    GekkoOps::require_supervisor(cpu);
    cpu.state().sr[static_cast<std::size_t>(ra(w) & 15)] = reg(cpu, rd(w));
}
void op_mfsrin(Gekko& cpu, u32 w) {
    GekkoOps::require_supervisor(cpu);
    reg(cpu, rd(w)) = cpu.state().sr[reg(cpu, rb(w)) >> 28];
}
void op_mtsrin(Gekko& cpu, u32 w) {
    GekkoOps::require_supervisor(cpu);
    cpu.state().sr[reg(cpu, rb(w)) >> 28] = reg(cpu, rd(w));
}

}  // namespace

void install_integer_ops(GekkoDecodeTables& t) {
    auto& p = t.primary;
    p[3] = op_twi;
    p[7] = op_mulli;
    p[8] = op_subfic;
    p[10] = op_cmpli;
    p[11] = op_cmpi;
    p[12] = op_addic;
    p[13] = op_addic_rc;
    p[14] = op_addi;
    p[15] = op_addis;
    p[16] = op_bc;
    p[17] = op_sc;
    p[18] = op_b;
    p[20] = op_rlwimi;
    p[21] = op_rlwinm;
    p[23] = op_rlwnm;
    p[24] = op_ori;
    p[25] = op_oris;
    p[26] = op_xori;
    p[27] = op_xoris;
    p[28] = op_andi_rc;
    p[29] = op_andis_rc;
    p[32] = load_op<ea_d, load_word, false>;   // lwz
    p[33] = load_op<ea_du, load_word, true>;   // lwzu
    p[34] = load_op<ea_d, load_byte, false>;   // lbz
    p[35] = load_op<ea_du, load_byte, true>;   // lbzu
    p[36] = store_op<ea_d, store_word, false>;  // stw
    p[37] = store_op<ea_du, store_word, true>;  // stwu
    p[38] = store_op<ea_d, store_byte, false>;  // stb
    p[39] = store_op<ea_du, store_byte, true>;  // stbu
    p[40] = load_op<ea_d, load_half, false>;   // lhz
    p[41] = load_op<ea_du, load_half, true>;   // lhzu
    p[42] = load_op<ea_d, load_half_algebraic, false>;  // lha
    p[43] = load_op<ea_du, load_half_algebraic, true>;  // lhau
    p[44] = store_op<ea_d, store_half, false>;  // sth
    p[45] = store_op<ea_du, store_half, true>;  // sthu
    p[46] = op_lmw;
    p[47] = op_stmw;

    t.op4[1014] = op_dcbz_l;

    auto& x19 = t.op19;
    x19[0] = op_mcrf;
    x19[16] = op_bclr;
    x19[33] = op_crnor;
    x19[50] = op_rfi;
    x19[129] = op_crandc;
    x19[150] = op_cache_no_effect;  // isync
    x19[193] = op_crxor;
    x19[225] = op_crnand;
    x19[257] = op_crand;
    x19[289] = op_creqv;
    x19[417] = op_crorc;
    x19[449] = op_cror;
    x19[528] = op_bcctr;

    auto& x31 = t.op31;
    // XO-form arithmetic: the same handler with OE = 0 and OE = 1.
    const auto xo = [&x31](int code, GekkoOp op) {
        x31[static_cast<std::size_t>(code)] = op;
        x31[static_cast<std::size_t>(code | 0x200)] = op;
    };
    xo(8, op_subfc);
    xo(10, op_addc);
    xo(40, op_subf);
    xo(104, op_neg);
    xo(136, op_subfe);
    xo(138, op_adde);
    xo(200, op_subfze);
    xo(202, op_addze);
    xo(232, op_subfme);
    xo(234, op_addme);
    xo(235, op_mullw);
    xo(266, op_add);
    xo(459, op_divwu);
    xo(491, op_divw);
    x31[11] = op_mulhwu;  // no OE form
    x31[75] = op_mulhw;

    x31[0] = op_cmp;
    x31[4] = op_tw;
    x31[19] = op_mfcr;
    x31[20] = op_lwarx;
    x31[23] = load_op<ea_x, load_word, false>;  // lwzx
    x31[24] = op_slw;
    x31[26] = op_cntlzw;
    x31[28] = op_and;
    x31[32] = op_cmpl;
    x31[54] = op_cache_no_effect;  // dcbst
    x31[55] = load_op<ea_xu, load_word, true>;  // lwzux
    x31[60] = op_andc;
    x31[83] = op_mfmsr;
    x31[86] = op_cache_no_effect;  // dcbf
    x31[87] = load_op<ea_x, load_byte, false>;  // lbzx
    x31[119] = load_op<ea_xu, load_byte, true>;  // lbzux
    x31[124] = op_nor;
    x31[144] = op_mtcrf;
    x31[146] = op_mtmsr;
    x31[150] = op_stwcx_rc;
    x31[151] = store_op<ea_x, store_word, false>;  // stwx
    x31[183] = store_op<ea_xu, store_word, true>;  // stwux
    x31[210] = op_mtsr;
    x31[215] = store_op<ea_x, store_byte, false>;  // stbx
    x31[242] = op_mtsrin;
    x31[246] = op_cache_no_effect;  // dcbtst
    x31[247] = store_op<ea_xu, store_byte, true>;  // stbux
    x31[278] = op_cache_no_effect;  // dcbt
    x31[279] = load_op<ea_x, load_half, false>;  // lhzx
    x31[284] = op_eqv;
    x31[306] = op_supervisor_no_effect;  // tlbie
    x31[311] = load_op<ea_xu, load_half, true>;  // lhzux
    x31[316] = op_xor;
    x31[339] = op_mfspr;
    x31[343] = load_op<ea_x, load_half_algebraic, false>;  // lhax
    x31[371] = op_mftb;
    x31[375] = load_op<ea_xu, load_half_algebraic, true>;  // lhaux
    x31[407] = store_op<ea_x, store_half, false>;  // sthx
    x31[412] = op_orc;
    x31[439] = store_op<ea_xu, store_half, true>;  // sthux
    x31[444] = op_or;
    x31[467] = op_mtspr;
    x31[470] = op_supervisor_no_effect;  // dcbi
    x31[476] = op_nand;
    x31[512] = op_mcrxr;
    x31[533] = op_lswx;
    x31[534] = load_op<ea_x, load_word_reversed, false>;  // lwbrx
    x31[536] = op_srw;
    x31[566] = op_supervisor_no_effect;  // tlbsync
    x31[595] = op_mfsr;
    x31[597] = op_lswi;
    x31[598] = op_cache_no_effect;  // sync
    x31[659] = op_mfsrin;
    x31[661] = op_stswx;
    x31[662] = store_op<ea_x, store_word_reversed, false>;  // stwbrx
    x31[725] = op_stswi;
    x31[790] = load_op<ea_x, load_half_reversed, false>;  // lhbrx
    x31[792] = op_sraw;
    x31[824] = op_srawi;
    x31[854] = op_cache_no_effect;  // eieio
    x31[918] = store_op<ea_x, store_half_reversed, false>;  // sthbrx
    x31[922] = op_extsh;
    x31[954] = op_extsb;
    x31[982] = op_cache_no_effect;  // icbi
    x31[1014] = op_dcbz;
}

}  // namespace ezy::gc
