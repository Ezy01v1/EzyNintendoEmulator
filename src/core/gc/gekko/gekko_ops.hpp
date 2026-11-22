#pragma once

// Internal to the Gekko interpreter: the decode tables, instruction field extraction and the
// CPU services only instruction handlers may use, shared by gekko.cpp and the files that
// implement instructions.
//
// Invalid instruction forms (the PEM's "invalid form": update loads with rA = 0 or rA = rD,
// lmw/lswi loading rA, stwcx. with Rc = 0, bcctr that decrements CTR...): where the result
// is well known (documented by the 750 manual or matched by Dolphin against hardware), the
// instruction runs that way, with a comment at the handler; where the result is undefined
// and nothing observed relies on it, it stops emulation with GekkoError (Gekko::
// unimplemented), like an unknown opcode. Today the only stopping case is bcctr with
// BO[2] = 0.

#include <array>
#include <cstddef>
#include <cstdint>

#include "gc/gekko/gekko.hpp"

namespace ezy::gc {

// Handlers by primary opcode (instruction bits 0-5), and by extended opcode (bits 21-30) for
// the primary opcodes that have one. Forms with a shorter extended opcode fill every slot
// it matches: XO-form arithmetic (9 bits + OE) both OE values; A-form (5 bits in 26-30) all
// 32 values of bits 21-25; the indexed quantized loads/stores (6 bits in 25-30) all 16
// values of W and I. Every unfilled slot stops emulation (Gekko::unimplemented).
struct GekkoDecodeTables {
    std::array<GekkoOp, 64> primary{};
    std::array<GekkoOp, 1024> op4{}, op19{}, op31{}, op59{}, op63{};
};

// An A-form instruction (5-bit extended opcode in bits 26-30) in a 1024-entry table: every
// value of bits 21-25 (frC, or frB for the ones without frC) decodes to it.
inline void fill_a_form(std::array<GekkoOp, 1024>& table, int xo, GekkoOp op) {
    for (int high = 0; high < 32; ++high) table[static_cast<std::size_t>((high << 5) | xo)] = op;
}

// Built once: the dispatchers, then each instruction file's install function.
const GekkoDecodeTables& gekko_decode_tables();
// gekko_integer.cpp: integer, branch, load/store, CR, SPR, system and cache instructions.
void install_integer_ops(GekkoDecodeTables& tables);
// gekko_fpu.cpp: floating-point loads/stores, arithmetic, conversions, compares and FPSCR.
void install_fpu_ops(GekkoDecodeTables& tables);
// gekko_paired.cpp: paired singles and the quantized loads/stores.
void install_paired_ops(GekkoDecodeTables& tables);

// The Gekko services that only make sense while step() executes an instruction (they can
// fault, and a fault ends the instruction with an exception only step() catches). Private
// in Gekko; instruction handlers reach them here.
struct GekkoOps {
    static std::uint8_t read8(Gekko& cpu, std::uint32_t ea) { return cpu.read8(ea); }
    static std::uint16_t read16(Gekko& cpu, std::uint32_t ea) { return cpu.read16(ea); }
    static std::uint32_t read32(Gekko& cpu, std::uint32_t ea) { return cpu.read32(ea); }
    static std::uint64_t read64(Gekko& cpu, std::uint32_t ea) { return cpu.read64(ea); }
    static void write8(Gekko& cpu, std::uint32_t ea, std::uint8_t value) { cpu.write8(ea, value); }
    static void write16(Gekko& cpu, std::uint32_t ea, std::uint16_t value) { cpu.write16(ea, value); }
    static void write32(Gekko& cpu, std::uint32_t ea, std::uint32_t value) { cpu.write32(ea, value); }
    static void write64(Gekko& cpu, std::uint32_t ea, std::uint64_t value) { cpu.write64(ea, value); }
    [[noreturn]] static void raise(Gekko& cpu, Gekko::Exception kind, std::uint32_t srr1_cause = 0) {
        cpu.raise(kind, srr1_cause);
    }
    [[noreturn]] static void raise_alignment(Gekko& cpu, std::uint32_t ea, std::uint32_t word) {
        cpu.raise_alignment(ea, word);
    }
    static void require_supervisor(Gekko& cpu) { cpu.require_supervisor(); }
};

// Instruction fields (PEM numbering: bit 0 is the MSB).
namespace ppc_fields {
constexpr int rd(std::uint32_t w) { return static_cast<int>((w >> 21) & 31); }  // also rS, frD, frS, BO, TO, crbD
constexpr int ra(std::uint32_t w) { return static_cast<int>((w >> 16) & 31); }  // also frA, BI, crbA
constexpr int rb(std::uint32_t w) { return static_cast<int>((w >> 11) & 31); }  // also frB, SH, NB, crbB
constexpr int rc_field(std::uint32_t w) { return static_cast<int>((w >> 6) & 31); }  // frC (A-form)
constexpr int crfd(std::uint32_t w) { return static_cast<int>((w >> 23) & 7); }
constexpr int crfs(std::uint32_t w) { return static_cast<int>((w >> 18) & 7); }
constexpr int mb(std::uint32_t w) { return static_cast<int>((w >> 6) & 31); }
constexpr int me(std::uint32_t w) { return static_cast<int>((w >> 1) & 31); }
constexpr std::uint32_t uimm(std::uint32_t w) { return w & 0xFFFF; }
constexpr std::uint32_t simm(std::uint32_t w) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFF)));
}
constexpr bool rc(std::uint32_t w) { return (w & 1) != 0; }  // also LK
constexpr bool oe(std::uint32_t w) { return (w & 0x400) != 0; }
constexpr bool aa(std::uint32_t w) { return (w & 2) != 0; }
constexpr int xo10(std::uint32_t w) { return static_cast<int>((w >> 1) & 0x3FF); }
// mfspr/mtspr/mftb: the SPR number, its two 5-bit halves swapped back.
constexpr int spr_number(std::uint32_t w) { return static_cast<int>(((w >> 16) & 31) | (((w >> 11) & 31) << 5)); }
}  // namespace ppc_fields

// Effective addresses of loads and stores: (rA|0) + d and (rA|0) + rB, where rA = 0 reads as
// zero; the update forms use rA itself. rA = 0 and (for loads) rA = rD are invalid update
// forms that run as Dolphin's interpreter runs them: r0 is used as a register, and rA
// (written last) ends with the EA.
inline std::uint32_t gpr_or_zero(const Gekko& cpu, int index) {
    return index == 0 ? 0 : cpu.state().gpr[static_cast<std::size_t>(index)];
}
inline std::uint32_t ea_d(Gekko& cpu, std::uint32_t w) { return gpr_or_zero(cpu, ppc_fields::ra(w)) + ppc_fields::simm(w); }
inline std::uint32_t ea_x(Gekko& cpu, std::uint32_t w) {
    return gpr_or_zero(cpu, ppc_fields::ra(w)) + cpu.state().gpr[static_cast<std::size_t>(ppc_fields::rb(w))];
}
inline std::uint32_t ea_du(Gekko& cpu, std::uint32_t w) {
    return cpu.state().gpr[static_cast<std::size_t>(ppc_fields::ra(w))] + ppc_fields::simm(w);
}
inline std::uint32_t ea_xu(Gekko& cpu, std::uint32_t w) {
    return cpu.state().gpr[static_cast<std::size_t>(ppc_fields::ra(w))] +
           cpu.state().gpr[static_cast<std::size_t>(ppc_fields::rb(w))];
}

}  // namespace ezy::gc
