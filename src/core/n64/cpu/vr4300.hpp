#pragma once

#include <array>
#include <cstdint>

#include "n64/cpu/cpu_bus.hpp"

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

    template <class Ar>
    void serialize(Ar& ar) {
        ar(gpr_), ar(hi_), ar(lo_), ar(pc_), ar(next_pc_), ar(in_delay_slot_), ar(branch_taken_), ar(llbit_);
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

    CpuBus& bus_;
    std::array<std::uint64_t, 32> gpr_{};
    std::uint64_t hi_ = 0, lo_ = 0;
    std::uint64_t pc_ = 0, next_pc_ = 4;
    std::uint64_t current_pc_ = 0;  // address of the instruction being executed
    bool in_delay_slot_ = false;    // the instruction being executed is in a delay slot
    bool branch_taken_ = false;     // the last instruction was a branch (its slot runs next)
    bool llbit_ = false;
    int cycles_ = 0;                // cycles of the instruction being executed
};

}  // namespace ezy::n64
