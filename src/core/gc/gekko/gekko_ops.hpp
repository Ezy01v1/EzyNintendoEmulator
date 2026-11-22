#pragma once

// Internal to the Gekko interpreter: the decode tables and instruction field extraction,
// shared by gekko.cpp and the files that implement instructions.

#include <array>
#include <cstdint>

#include "gc/gekko/gekko.hpp"

namespace ezy::gc {

// Handlers by primary opcode (instruction bits 0-5), and by extended opcode (bits 21-30) for
// the primary opcodes that have one. Forms with a shorter extended opcode fill every slot
// it matches: XO-form arithmetic (9 bits + OE) both OE values; A-form (5 bits in 26-30) all
// 32 values of bits 21-25. Every unfilled slot stops emulation (Gekko::unimplemented).
struct GekkoDecodeTables {
    std::array<GekkoOp, 64> primary{};
    std::array<GekkoOp, 1024> op4{}, op19{}, op31{}, op59{}, op63{};
};

// Built once: the dispatchers, then each instruction file's install function.
const GekkoDecodeTables& gekko_decode_tables();
// gekko_integer.cpp: integer, branch, load/store, CR, SPR, system and cache instructions.
void install_integer_ops(GekkoDecodeTables& tables);

// Instruction fields (PEM numbering: bit 0 is the MSB).
namespace ppc_fields {
constexpr int rd(std::uint32_t w) { return static_cast<int>((w >> 21) & 31); }  // also rS, BO, TO, crbD
constexpr int ra(std::uint32_t w) { return static_cast<int>((w >> 16) & 31); }  // also BI, crbA
constexpr int rb(std::uint32_t w) { return static_cast<int>((w >> 11) & 31); }  // also SH, NB, crbB
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

}  // namespace ezy::gc
