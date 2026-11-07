#pragma once

#include <array>
#include <cstdint>

#include "n64/cpu/cop0.hpp"
#include "n64/cpu/cop1.hpp"
#include "n64/cpu/cpu_bus.hpp"
#include "n64/cpu/tlb.hpp"

namespace ezy::n64 {

// The NEC VR4300 (MIPS III): the N64's main CPU, interpreted one instruction per step().
//
// Branches: the instruction after a branch (its delay slot) always runs before the jump,
// except for a branch-likely that is not taken, which skips it. pc_ is the instruction
// to run next and next_pc_ the one after it, so a branch only has to set next_pc_.
//
// Cycles: step() returns what the instruction took: 1, plus the extra cycles of
// multiplies and divides (VR4300 manual, "Instruction Execution Cycles"), plus the bus
// penalty of each uncached access. Cached accesses cost nothing extra: the cache
// contents aren't emulated.
class Vr4300 {
public:
    explicit Vr4300(CpuBus& bus) : bus_(bus) {}

    int step();

    [[nodiscard]] std::uint64_t gpr(int index) const { return gpr_[static_cast<std::size_t>(index)]; }
    void set_gpr(int index, std::uint64_t value) {
        if (index != 0) gpr_[static_cast<std::size_t>(index)] = value;
    }
    [[nodiscard]] std::uint64_t pc() const { return pc_; }
    void set_pc(std::uint64_t pc) {
        pc_ = pc;
        next_pc_ = pc + 4;
        in_delay_slot_ = false;
        branch_taken_ = false;
    }
    [[nodiscard]] std::uint64_t hi() const { return hi_; }
    [[nodiscard]] std::uint64_t lo() const { return lo_; }
    [[nodiscard]] Cop0& cop0() { return cop0_; }
    [[nodiscard]] const Cop0& cop0() const { return cop0_; }
    [[nodiscard]] Cop1& cop1() { return cop1_; }
    [[nodiscard]] const Cop1& cop1() const { return cop1_; }

    // Takes an exception for the instruction being executed (or, for Int, about to be).
    // coprocessor goes to Cause.CE for CpU.
    void raise(ExceptionCode code, int coprocessor = 0);
    // 64-bit operations are reserved in 32-bit user or supervisor mode.
    [[nodiscard]] bool allow_64bit_ops() const;

    template <class Ar>
    void serialize(Ar& ar) {
        ar(gpr_), ar(hi_), ar(lo_), ar(pc_), ar(next_pc_), ar(in_delay_slot_), ar(branch_taken_), ar(llbit_);
        cop0_.serialize(ar);
        tlb_.serialize(ar);
        cop1_.serialize(ar);
    }

private:
    // Physical address of a virtual one. False if it can't be translated (yet).
    struct Translation {
        std::uint32_t paddr;
        bool cached;
    };
    [[nodiscard]] bool translate(std::uint64_t vaddr, bool write, Translation& out);

    template <typename T>
    bool load(std::uint64_t vaddr, T& value);
    template <typename T>
    bool store(std::uint64_t vaddr, T value);

    void execute(std::uint32_t instr);
    void execute_special(std::uint32_t instr);
    void execute_regimm(std::uint32_t instr);
    void branch(bool taken, std::uint64_t target, bool likely);
    void execute_load_store(std::uint32_t instr);
    void execute_cop0(std::uint32_t instr);
    void execute_cop1(std::uint32_t instr);
    void execute_tlb(std::uint32_t instr);  // TLBR, TLBWI, TLBWR, TLBP
    // Addresses outside kseg0/kseg1: through the TLB. Raises the exception on failure.
    [[nodiscard]] bool translate_mapped(std::uint64_t vaddr, bool write, Translation& out);
    // Raises Reserved Instruction (and returns false) for a 64-bit op in 32-bit user or
    // supervisor mode.
    bool require_64bit();
    // TLB miss/invalid/modified: BadVAddr, Context, XContext, EntryHi, then the vector.
    void raise_tlb(ExceptionCode code, std::uint64_t vaddr, bool refill, bool wide);
    [[nodiscard]] bool wide_addressing() const;  // 64-bit addressing in the current mode
    void eret();
    void raise_address_error(std::uint64_t vaddr, bool write);
    // The exception vector: base (BEV) + offset; refill uses the TLB refill offsets.
    void enter_exception(ExceptionCode code, int coprocessor, std::uint32_t offset);
    enum class Mode : std::uint8_t { Kernel, Supervisor, User };
    [[nodiscard]] Mode mode() const;

    CpuBus& bus_;
    Cop0 cop0_;
    Tlb tlb_;
    Cop1 cop1_;
    std::array<std::uint64_t, 32> gpr_{};
    std::uint64_t hi_ = 0, lo_ = 0;
    std::uint64_t pc_ = 0, next_pc_ = 4;
    std::uint64_t current_pc_ = 0;  // address of the instruction being executed
    bool in_delay_slot_ = false;    // the instruction being executed is in a delay slot
    bool branch_taken_ = false;     // the last instruction was a branch (its slot runs next)
    bool llbit_ = false;
    int cycles_ = 0;                // cycles of the instruction being executed
    bool exception_ = false;        // the instruction being executed raised an exception
};

}  // namespace ezy::n64
