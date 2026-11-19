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
//   Native      translated to IR: the integer ALU (vr4300_jit_alu.cpp), the 64-bit ops
//               only when allow_64bit (else InPlace: the interpreter raises RI); every
//               branch but BC1* (vr4300_jit_branch.cpp); LB LBU LH LHU LW SB SH SW, and
//               LWU LD SD only when allow_64bit (vr4300_jit_memory.cpp), which leave the
//               block before themselves unless they are aligned kseg0 accesses inside
//               direct RAM that the threaded RDP's barrier lets through
//   InPlace     Vr4300::execute_in_place, called from the block (BC1* included)
//   Memory      any other load or store (LWL..SDR, LL/SC, COP1, the 64-bit ones without
//               allow_64bit): Vr4300::execute_memory_in_place, which exits before it
//               unless it is a kernel-mode kseg0/kseg1 access inside direct RAM
//   OutOfBlock  COP0: the block ends before it and Vr4300::step() runs it
enum class Kind : std::uint8_t { Native, InPlace, Memory, OutOfBlock };
[[nodiscard]] Kind classify(std::uint32_t instr, bool allow_64bit);
// J JAL JR JALR, B*, B*L, the REGIMM branches (and their -AL forms), BC1*.
[[nodiscard]] bool is_branch(std::uint32_t instr);
// The static cycles of an integer ALU instruction under this mode (1; MULT/MULTU 6;
// DMULT/DMULTU 9), 0 if it is not one (vr4300_jit_alu.cpp). Native branches take 1.
[[nodiscard]] std::uint32_t alu_cycles(std::uint32_t instr, bool allow_64bit);
// The memory range a Memory instruction touches: `bytes` at its address, rounded down to
// `bytes` for LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR. bytes == 0: not a Memory instruction.
struct MemoryAccess {
    std::uint8_t bytes = 0;
    bool aligned_down = false;
    bool store = false;
};
[[nodiscard]] MemoryAccess memory_access(std::uint32_t instr);
// Whether a load or store runs natively under this mode (vr4300_jit_memory.cpp).
[[nodiscard]] bool is_native_memory(std::uint32_t instr, bool allow_64bit);

inline constexpr unsigned kMaxBlockInstructions = 64;

// Byte offsets of Vr4300 members from Context::guest (= &cpu), for native code.
struct GuestOffsets {
    std::uint32_t gpr = 0, hi = 0, lo = 0, pc = 0, next_pc = 0, in_delay_slot = 0, branch_taken = 0,
                  idle_candidate = 0;
};

// The VR4300 CPU JIT: compiles blocks of kernel-mode kseg0 code from direct RAM and runs
// them in place of Vr4300::step(), with the same result instruction for instruction.
// A block ends after a branch and its delay slot, before an OutOfBlock instruction, at
// kMaxBlockInstructions, or at the end of its 4 KiB page or of direct RAM. A delay slot
// is native or in place like any other instruction, except after BC1* (in place itself),
// whose slot stays in place too.
class Vr4300Jit {
public:
    // Registers itself as the bus's code watch (CpuBus::set_code_watch) until destroyed.
    // code_budget caps the executable memory, allocated as blocks need it; reaching it
    // flushes every block. With native loads and stores, MK64's first 1900 fields compile
    // about 68 MB of code: under the former 32 MiB, flushes and recompiles halved the speed.
    Vr4300Jit(Vr4300& cpu, CpuBus& bus, std::size_t code_budget = std::size_t{128} * 1024 * 1024);
    ~Vr4300Jit();
    Vr4300Jit(const Vr4300Jit&) = delete;
    Vr4300Jit& operator=(const Vr4300Jit&) = delete;

    // False when this build has no native code: execute() then never runs anything.
    [[nodiscard]] bool available() const { return available_; }

    struct Result {
        std::uint64_t cycles = 0, instructions = 0;
    };
    // Runs blocks from cpu.pc() while each fits strictly below min(event_budget,
    // cycles_until_compare()). Retires Count/Random (retire_many) itself.
    // instructions == 0: the caller runs cpu.step(). Throws std::runtime_error in check
    // mode when a block no longer matches RAM, before running it.
    Result execute(std::uint64_t event_budget);
    // Drops the blocks over [begin, end). The bus's code watch calls it for every write
    // over compiled code (CpuBus::note_ram_write), stores run in place included: a block
    // whose own store drops code leaves right after it (CodeChanged). Native stores call
    // native_code_write instead, through the same table, with the same effect.
    void invalidate_ram(std::uint32_t begin, std::uint32_t end);
    void flush();
    // Debugging: before entering each block, compare the words it was compiled from with
    // RAM; a difference means some writer did not report its write.
    void set_check(bool enabled) { check_ = enabled; }

    // in_place and native: instructions retired by blocks, through a helper or as native
    // code. exits_before: blocks left before an instruction the interpreter must run.
    struct Stats {
        std::uint64_t compiled = 0, in_place = 0, native = 0, out_of_block = 0, exits_before = 0, compile_ns = 0,
                      invalidations = 0, flushes = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    // What a helper tells its block (the value Helper returns).
    enum Status : std::uint64_t { kOk, kAfter, kBefore, kSkip };
    // Helper flags (second argument).
    static constexpr std::uint64_t kFlagMemory = 1, kFlagBranch = 2;

    // The Helper every block calls: runs one instruction in place; block_cycles is the
    // block's static cycle count, for the budget check.
    static std::uint64_t run_instruction(jit::Context* ctx, std::uint64_t instr, std::uint64_t flags,
                                         std::uint64_t block_cycles) noexcept;
    std::uint64_t run(std::uint32_t instr, std::uint64_t flags, std::uint64_t block_cycles);
    static void on_code_write(void* self, std::uint32_t begin, std::uint32_t end);
    // Context::code_write: a native store over a marked page; 1 if it dropped a block, or if it
    // caught an error (the store is retired; execute() rethrows the error at the boundary).
    static std::uint32_t native_code_write(jit::Context* ctx, std::uint32_t paddr, std::uint32_t bytes) noexcept;
    void verify(const jit::Block& block) const;
    [[nodiscard]] std::uint32_t mode_key() const;
    [[nodiscard]] bool kernel_mode() const;
    const jit::Block* compile(std::uint32_t paddr, std::uint32_t key);
    void retire(std::uint64_t cycles, std::uint64_t instructions, std::uint64_t until_compare);

    Vr4300& cpu_;
    CpuBus& bus_;
    jit::BlockCache cache_;
    jit::Context ctx_{};
    jit::PageBarrierView barrier_view_{};  // ctx_.barrier while the bus has one (execute())
    GuestOffsets offsets_;
    bool available_ = false;
    bool check_ = false;
    bool code_changed_ = false;  // invalidate_ram() dropped a block since run() cleared it
    std::exception_ptr error_;  // a C++ exception a helper caught, rethrown by execute()
    Stats stats_;
};

}  // namespace ezy::n64
