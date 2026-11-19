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
//  - native instructions do not write pc_/next_pc_/branch_taken_/in_delay_slot_; the
//    same places store what step() would have left before the next instruction, which
//    depends on where the block is (PcState):
//      Sequential  pc_ = its address, next_pc_ = pc_ + 4, both flags false
//      InSlot      after a native branch, before its delay slot: pc_ = the slot,
//                  next_pc_ = N (the target if taken, else the slot + 4),
//                  branch_taken_ = true, in_delay_slot_ = false
//      AfterSlot   after a native slot: pc_ = N, next_pc_ = N + 4, branch_taken_ =
//                  false, in_delay_slot_ = true (the slot was one)
// After a helper call nothing is pending (Clean): the helper ran the interpreter on the
// real state.
class BlockBuilder {
public:
    // `vaddr`: the kseg0 address of the block's first instruction; `ram_size`: Context::ram_size.
    BlockBuilder(jit::IrBuilder& ir, const GuestOffsets& offsets, std::uint64_t vaddr, bool allow_64bit,
                 std::uint32_t ram_size)
        : ir_(ir), offsets_(offsets), vaddr_(vaddr), allow_64bit_(allow_64bit), ram_size_(ram_size) {
        cached_.fill(kNone);
    }

    // For the emitters, while they emit the current instruction.
    [[nodiscard]] jit::IrBuilder& ir() { return ir_; }
    [[nodiscard]] bool allow_64bit() const { return allow_64bit_; }
    [[nodiscard]] std::uint32_t ram_size() const { return ram_size_; }
    jit::Value gpr(int index);                  // I64; $0 reads as 0
    void set_gpr(int index, jit::Value value);  // I64; $0 is never written
    jit::Value hi() { return reg(kHi); }
    jit::Value lo() { return reg(kLo); }
    void set_hi(jit::Value value) { set_reg(kHi, value); }
    void set_lo(jit::Value value) { set_reg(kLo, value); }
    jit::Value i64(std::uint64_t value) { return ir_.constant(jit::Type::I64, value); }
    jit::Value i32(std::uint32_t value) { return ir_.constant(jit::Type::I32, value); }
    // The current instruction's (virtual, kseg0) address.
    [[nodiscard]] std::uint64_t address() const { return vaddr_; }
    // Vr4300::idle_candidate_ (I64), stored right away: it is not part of the lazy state.
    jit::Value idle_candidate();
    void set_idle_candidate(jit::Value value);
    // When cond (I32) is nonzero, leaves the block with the CPU exactly before the current
    // instruction, which is not retired: the interpreter's next step() runs it (and raises
    // its exception). Call it before the instruction writes anything.
    void exit_before_if(jit::Value cond);
    // The exit exit_before_if() takes, for several jumps (exit_if, a barrier's failure).
    jit::ExitId exit_before();
    // When cond (I32) is nonzero, leaves the block right after the current instruction, a
    // native one taking `cycles` that retires (kExitAfter: execute() stops there): the pc
    // state of the next instruction. Call it after the instruction's writes.
    void exit_after_if(jit::Value cond, std::uint32_t cycles);
    // For a branch-likely: when not_taken (I32) is nonzero, leaves the block right after
    // the current instruction, its delay slot nullified (a normal end: the next block
    // starts at the slot + 4). Call it after the branch's writes (the link).
    void skip_slot_if(jit::Value not_taken);
    // Ends the current instruction, a native branch (1 static cycle); next (I64) is the
    // next_pc_ it leaves, N above. The block's next instruction is its delay slot.
    void end_branch(jit::Value next);
    void end_branch(std::uint64_t next);

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

private:
    static constexpr std::uint32_t kNone = ~0U;
    static constexpr int kHi = 32, kLo = 33, kRegs = 34;
    enum class PcState : std::uint8_t { Clean, Sequential, InSlot, AfterSlot };

    jit::Value reg(int index);
    void set_reg(int index, jit::Value value);
    [[nodiscard]] std::uint32_t reg_offset(int index) const;
    // Stores the pending registers and the pending pc state (into the current region: the
    // body, or an exit's).
    void write_back();
    void store_registers();  // the pending registers only
    // The pc state `state` for the instruction at `at` (Sequential, InSlot: its pc_;
    // AfterSlot only uses N). Clean stores nothing.
    void store_pc_state(PcState state, std::uint64_t at);
    jit::Value next_value();  // N as an IR value
    jit::ExitId add_exit(std::uint32_t instructions, std::uint32_t cycles, std::uint32_t native, std::uint32_t kind);

    jit::IrBuilder& ir_;
    const GuestOffsets& offsets_;
    std::uint64_t vaddr_;  // the current instruction's address
    bool allow_64bit_;
    std::uint32_t ram_size_;
    std::array<std::uint32_t, kRegs> cached_{};  // IR value id per register, kNone if not loaded
    std::uint64_t dirty_ = 0;                     // registers written since their last store
    PcState pc_state_ = PcState::Clean;           // Clean: the CPU holds the pc state
    // N (see PcState): an IR value, or a constant when next_is_imm_.
    jit::Value next_{};
    std::uint64_t next_imm_ = 0;
    bool next_is_imm_ = false;
    std::uint32_t done_ = 0, cycles_ = 0, native_ = 0;  // before the current instruction
    std::vector<jit::BlockExit> exits_;
};

// Native integer ALU (vr4300_jit_alu.cpp): emits `instr` into the builder, or returns
// false (emitting nothing) when it is not one under the builder's mode (alu_cycles == 0).
bool emit_alu(BlockBuilder& builder, std::uint32_t instr);
// Native branches (vr4300_jit_branch.cpp): J JAL JR JALR, B* and B*L, the REGIMM
// branches and their -AL forms (not BC1*). Emits `instr` and ends it (end_branch), or
// returns false (emitting nothing) when it is not one.
bool emit_branch(BlockBuilder& builder, std::uint32_t instr);
// Native loads and stores (vr4300_jit_memory.cpp): LB LBU LH LHU LW SB SH SW, and LWU LD SD
// under allow_64bit (is_native_memory). Emits `instr` (the caller ends it, 1 static cycle),
// or returns false (emitting nothing) when it is not one.
bool emit_memory(BlockBuilder& builder, std::uint32_t instr);

}  // namespace ezy::n64
