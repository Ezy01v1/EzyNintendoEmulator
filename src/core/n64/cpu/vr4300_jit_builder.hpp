#pragma once

// Internal to the VR4300 JIT: how a block's IR is built, instruction by instruction, by
// Vr4300Jit::compile() and the native emitters (vr4300_jit_alu.cpp, ...).
#include <array>
#include <cstdint>
#include <vector>

#include "jit/block_cache.hpp"
#include "jit/ir.hpp"
#include "n64/cpu/vr4300_jit.hpp"

namespace ezy::n64 {

// BlockExit::kind.
enum ExitKind : std::uint32_t { kExitNormal, kExitBefore, kExitAfter };

// The state native instructions leave is lazy:
//  - registers (GPRs, HI, LO) stay in IR values; a write is stored back only before a
//    helper call and on the exits taken while it is still pending;
//  - native instructions do not write pc_/next_pc_/branch_taken_/in_delay_slot_; when one
//    ran since the last write, the same places store what step() would have left before
//    the next instruction: pc_ = its address, next_pc_ = pc_ + 4, both flags false.
// After a helper call nothing is pending: the helper ran the interpreter on the real state.
class BlockBuilder {
public:
    // `vaddr`: the kseg0 address of the block's first instruction.
    BlockBuilder(jit::IrBuilder& ir, const GuestOffsets& offsets, std::uint64_t vaddr, bool allow_64bit)
        : ir_(ir), offsets_(offsets), vaddr_(vaddr), allow_64bit_(allow_64bit) {
        cached_.fill(kNone);
    }

    // For the emitters, while they emit the current instruction.
    [[nodiscard]] jit::IrBuilder& ir() { return ir_; }
    [[nodiscard]] bool allow_64bit() const { return allow_64bit_; }
    jit::Value gpr(int index);                  // I64; $0 reads as 0
    void set_gpr(int index, jit::Value value);  // I64; $0 is never written
    jit::Value hi() { return reg(kHi); }
    jit::Value lo() { return reg(kLo); }
    void set_hi(jit::Value value) { set_reg(kHi, value); }
    void set_lo(jit::Value value) { set_reg(kLo, value); }
    jit::Value i64(std::uint64_t value) { return ir_.constant(jit::Type::I64, value); }
    jit::Value i32(std::uint32_t value) { return ir_.constant(jit::Type::I32, value); }
    // When cond (I32) is nonzero, leaves the block with the CPU exactly before the current
    // instruction, which is not retired: the interpreter's next step() runs it (and raises
    // its exception). Call it before the instruction writes anything.
    void exit_before_if(jit::Value cond);

    // For compile().
    // Ends the current instruction, emitted natively, taking `cycles` static cycles.
    void end_native(std::uint32_t cycles);
    // Before a helper call for the current instruction: writes the pending state back and
    // forgets the cached registers (the helper may read and write any of them).
    void sync();
    // Ends the current instruction, run in place by the helper (1 static cycle).
    void end_in_place();
    // An exit after a helper call for the current instruction: `after` counts it as retired.
    jit::ExitId helper_exit(std::uint32_t kind, bool after);
    // The block's last exit, after its last instruction; writes the pending state back.
    void finish();

    [[nodiscard]] const std::vector<jit::BlockExit>& exits() const { return exits_; }
    [[nodiscard]] std::uint32_t native_count() const { return native_; }

private:
    static constexpr std::uint32_t kNone = ~0U;
    static constexpr int kHi = 32, kLo = 33, kRegs = 34;

    jit::Value reg(int index);
    void set_reg(int index, jit::Value value);
    [[nodiscard]] std::uint32_t reg_offset(int index) const;
    // Stores the pending registers and, if stale, the pc state for the current instruction
    // (into the current region: the body, or an exit's).
    void write_back();
    jit::ExitId add_exit(std::uint32_t instructions, std::uint32_t cycles, std::uint32_t native, std::uint32_t kind);

    jit::IrBuilder& ir_;
    const GuestOffsets& offsets_;
    std::uint64_t vaddr_;  // the current instruction's address
    bool allow_64bit_;
    std::array<std::uint32_t, kRegs> cached_{};  // IR value id per register, kNone if not loaded
    std::uint64_t dirty_ = 0;                     // registers written since their last store
    bool pc_stale_ = false;                       // a native instruction ran since the last pc write
    std::uint32_t done_ = 0, cycles_ = 0, native_ = 0;  // before the current instruction
    std::vector<jit::BlockExit> exits_;
};

// Native integer ALU (vr4300_jit_alu.cpp). alu_cycles: the static cycles of `instr` if it
// is one under this mode (1; MULT/MULTU 6; DMULT/DMULTU 9), else 0. emit_alu emits it into
// the builder, or returns false (emitting nothing) when it is not one.
[[nodiscard]] std::uint32_t alu_cycles(std::uint32_t instr, bool allow_64bit);
bool emit_alu(BlockBuilder& builder, std::uint32_t instr);

}  // namespace ezy::n64
