#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>

#include "jit/block_cache.hpp"
#include "jit/context.hpp"
#include "n64/cpu/cpu_bus.hpp"
#include "n64/cpu/vr4300.hpp"

namespace ezy::n64 {

// How a block treats an instruction (vr4300_jit_decode.cpp):
//   Native      translated to IR (none yet: every instruction still runs in place)
//   InPlace     Vr4300::execute_in_place, called from the block
//   Memory      a load or store: Vr4300::execute_memory_in_place, which exits before it
//               unless it is a kernel-mode kseg0/kseg1 access inside direct RAM
//   OutOfBlock  COP0: the block ends before it and Vr4300::step() runs it
enum class Kind : std::uint8_t { Native, InPlace, Memory, OutOfBlock };
[[nodiscard]] Kind classify(std::uint32_t instr);
// J JAL JR JALR, B*, B*L, the REGIMM branches (and their -AL forms), BC1*.
[[nodiscard]] bool is_branch(std::uint32_t instr);
// The memory range a Memory instruction touches: `bytes` at its address, rounded down to
// `bytes` for LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR. bytes == 0: not a Memory instruction.
struct MemoryAccess {
    std::uint8_t bytes = 0;
    bool aligned_down = false;
    bool store = false;
};
[[nodiscard]] MemoryAccess memory_access(std::uint32_t instr);

inline constexpr unsigned kMaxBlockInstructions = 64;

// The VR4300 CPU JIT: compiles blocks of kernel-mode kseg0 code from direct RAM and runs
// them in place of Vr4300::step(), with the same result instruction for instruction.
// A block ends after a branch and its delay slot, before an OutOfBlock instruction, at
// kMaxBlockInstructions, or at the end of its 4 KiB page or of direct RAM.
class Vr4300Jit {
public:
    Vr4300Jit(Vr4300& cpu, CpuBus& bus, std::size_t code_budget = 32 * 1024 * 1024);
    Vr4300Jit(const Vr4300Jit&) = delete;
    Vr4300Jit& operator=(const Vr4300Jit&) = delete;

    // False when this build has no native code: execute() then never runs anything.
    [[nodiscard]] bool available() const { return available_; }

    struct Result {
        std::uint64_t cycles = 0, instructions = 0;
    };
    // Runs blocks from cpu.pc() while each fits strictly below min(event_budget,
    // cycles_until_compare()). Retires Count/Random (retire_many) itself.
    // instructions == 0: the caller runs cpu.step().
    Result execute(std::uint64_t event_budget);
    void invalidate_ram(std::uint32_t begin, std::uint32_t end);  // from any RDRAM writer
    void flush();
    void set_check(bool enabled) { check_ = enabled; }  // Task 5

    struct Stats {
        std::uint64_t compiled = 0, in_place = 0, native = 0, out_of_block = 0, exits_before = 0, compile_ns = 0,
                      invalidations = 0, flushes = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    // What a helper tells its block (the value Helper returns).
    enum Status : std::uint64_t { kOk, kAfter, kBefore, kSkip };
    // BlockExit::kind.
    enum ExitKind : std::uint32_t { kNormal, kExitBefore, kExitAfter };
    // Helper flags (second argument).
    static constexpr std::uint64_t kFlagMemory = 1, kFlagBranch = 2;

    // The Helper every block calls: runs one instruction in place; block_cycles is the
    // block's static cycle count, for the budget check.
    static std::uint64_t run_instruction(jit::Context* ctx, std::uint64_t instr, std::uint64_t flags,
                                         std::uint64_t block_cycles) noexcept;
    std::uint64_t run(std::uint32_t instr, std::uint64_t flags, std::uint64_t block_cycles);
    [[nodiscard]] std::uint32_t mode_key() const;
    [[nodiscard]] bool kernel_mode() const;
    const jit::Block* compile(std::uint32_t paddr, std::uint32_t key);
    void retire(std::uint64_t cycles, std::uint64_t instructions, std::uint64_t until_compare);

    // Byte offsets of Vr4300 members from Context::guest (= &cpu), for native code.
    struct Offsets {
        std::uint32_t gpr = 0, hi = 0, lo = 0, pc = 0, next_pc = 0, branch_taken = 0, idle_candidate = 0;
    };

    Vr4300& cpu_;
    CpuBus& bus_;
    jit::BlockCache cache_;
    jit::Context ctx_{};
    Offsets offsets_;
    bool available_ = false;
    bool check_ = false;
    std::exception_ptr error_;  // a C++ exception a helper caught, rethrown by execute()
    Stats stats_;
};

}  // namespace ezy::n64
