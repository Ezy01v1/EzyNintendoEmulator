#pragma once

// The CPU JIT tests' harness: a flat bus and two identical machines, one running the
// interpreter and one the JIT, compared instruction for instruction.
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "common/state.hpp"
#include "mips_asm.hpp"
#include "n64/cpu/vr4300.hpp"
#include "n64/cpu/vr4300_jit.hpp"

namespace cpu_jit_test {

using ezy::n64::Cop0;
using ezy::n64::Vr4300;
using ezy::n64::Vr4300Jit;
using namespace mips;

constexpr std::uint64_t kKseg0 = 0xFFFFFFFF80000000ULL;
constexpr std::uint32_t kCode = 0x1000;           // physical start of every program
constexpr std::uint32_t kData = 0x40000;          // data zone for loads and stores
constexpr std::uint32_t kDataBytes = 0x1000;
constexpr std::uint64_t kVector = kKseg0 + 0x180;  // general exception vector (BEV = 0)
constexpr std::size_t kDefaultCode = 32 * 1024 * 1024;
// Out of block: no block runs past it, so a run can stop exactly there.
constexpr std::uint32_t kStop = mfc0(0, 9);

// Plain memory, the first `direct` bytes exposed as direct RAM; the rest (and anything
// above, wrapping) goes through the virtual accessors, counted as MMIO.
struct TwinBus final : ezy::n64::CpuBus {
    std::vector<std::uint8_t> mem;
    mutable unsigned mmio_reads = 0, mmio_writes = 0;
    std::uint32_t penalty = 0;
    std::uint32_t throw_at = ~0U;  // prepare_memory throws for an access covering it

    TwinBus(std::uint32_t size, std::uint32_t direct) : mem(size, 0) {
        set_direct_ram(mem.data(), direct);
        set_memory_guard(true);
    }
    std::uint8_t& at(std::uint32_t paddr) { return mem[paddr % mem.size()]; }
    void prepare_memory(std::uint32_t address, std::size_t bytes, bool) const override {
        if (throw_at >= address && throw_at - address < bytes) throw std::runtime_error("host failure");
    }
    std::uint64_t read(std::uint32_t paddr, unsigned bytes) {
        ++mmio_reads;
        std::uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | at(paddr + i);
        return v;
    }
    void write(std::uint32_t paddr, std::uint64_t v, unsigned bytes) {
        ++mmio_writes;
        for (unsigned i = 0; i < bytes; ++i) at(paddr + i) = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
    }
    std::uint8_t read8(std::uint32_t a) override { return static_cast<std::uint8_t>(read(a, 1)); }
    std::uint16_t read16(std::uint32_t a) override { return static_cast<std::uint16_t>(read(a, 2)); }
    std::uint32_t read32(std::uint32_t a) override { return static_cast<std::uint32_t>(read(a, 4)); }
    std::uint64_t read64(std::uint32_t a) override { return read(a, 8); }
    void write8(std::uint32_t a, std::uint8_t v) override { write(a, v, 1); }
    void write16(std::uint32_t a, std::uint16_t v) override { write(a, v, 2); }
    void write32(std::uint32_t a, std::uint32_t v) override { write(a, v, 4); }
    void write64(std::uint32_t a, std::uint64_t v) override { write(a, v, 8); }
    [[nodiscard]] std::uint32_t uncached_cycles(std::uint32_t) const override { return penalty; }
    [[nodiscard]] bool interrupt_line() const override { return false; }
};

struct TwinOptions {
    std::uint32_t mem = 0x100000, direct = 0x100000;
    std::size_t code_budget = kDefaultCode;
    std::uint32_t penalty = 0;
};

inline std::vector<std::uint8_t> state_of(Vr4300& cpu) {
    ezy::StateWriter w;
    cpu.serialize(w);
    return w.take();
}

// Two identical machines: `ref` runs the interpreter, `jit` runs Vr4300Jit (or step() when
// it runs nothing), always the same number of instructions.
struct Twin {
    TwinBus ref_bus, jit_bus;
    Vr4300 ref_cpu{ref_bus}, jit_cpu{jit_bus};
    Vr4300Jit jit;
    std::uint64_t ref_cycles = 0, jit_cycles = 0, jit_instructions = 0;
    std::uint64_t budget = 1000;
    bool lockstep = false;  // compare the whole state after every advance()
    bool diverged = false;
    bool cycles_differ = false;
    std::uint64_t end = 0;

    explicit Twin(TwinOptions o = {})
        : ref_bus(o.mem, o.direct), jit_bus(o.mem, o.direct), jit(jit_cpu, jit_bus, o.code_budget) {
        ref_bus.penalty = jit_bus.penalty = o.penalty;
    }
    // Random program from the generator (defined in test_cpu_jit.cpp).
    struct Generated {
        std::size_t code_budget = kDefaultCode;
        // Some registers start at 0, 1, -1, INT32_MIN, INT32_MAX, INT64_MIN or INT64_MAX.
        bool edge_values = false;
        // ADD ADDI SUB DADD DADDI DSUB too; the Ov handler at the vector skips the instruction.
        bool overflow_ops = false;
    };
    Twin(unsigned seed, Generated options);
    explicit Twin(unsigned seed, std::size_t code_budget = kDefaultCode)
        : Twin(seed, Generated{.code_budget = code_budget}) {}

    void poke32(std::uint32_t paddr, std::uint32_t word) {
        for (TwinBus* bus : {&ref_bus, &jit_bus})
            for (unsigned i = 0; i < 4; ++i) bus->at(paddr + i) = static_cast<std::uint8_t>(word >> (24 - 8 * i));
    }
    void poke8(std::uint32_t paddr, std::uint8_t v) { ref_bus.at(paddr) = jit_bus.at(paddr) = v; }
    void program(std::uint32_t paddr, const std::vector<std::uint32_t>& words) {
        for (const std::uint32_t w : words) {
            poke32(paddr, w);
            paddr += 4;
        }
    }
    void start(std::uint64_t pc, std::uint32_t status = 0) {
        for (Vr4300* cpu : {&ref_cpu, &jit_cpu}) {
            cpu->cop0().write(Cop0::kStatus, status);
            cpu->set_pc(pc);
        }
    }
    void reset_mmio_counts() { ref_bus.mmio_reads = ref_bus.mmio_writes = jit_bus.mmio_reads = jit_bus.mmio_writes = 0; }

    void advance() {
        const auto r = jit.execute(budget);
        std::uint64_t n = r.instructions;
        std::uint64_t ran = 0;
        if (n == 0) {
            jit_cycles += static_cast<std::uint64_t>(jit_cpu.step());
            n = 1;
        } else {
            jit_cycles += r.cycles;
            jit_instructions += n;
        }
        for (std::uint64_t i = 0; i < n; ++i) ran += static_cast<std::uint64_t>(ref_cpu.step());
        ref_cycles += ran;
        if (r.instructions != 0 && ran != r.cycles) cycles_differ = true;
        if (lockstep && !same()) diverged = true;
    }
    void run_until_pc(std::uint64_t pc, unsigned max_advances = 200000) {
        for (unsigned i = 0; i < max_advances && jit_cpu.pc() != pc; ++i) advance();
        REQUIRE(jit_cpu.pc() == pc);
    }
    [[nodiscard]] bool same() {
        return state_of(ref_cpu) == state_of(jit_cpu) && ref_bus.mem == jit_bus.mem && ref_cycles == jit_cycles &&
               !cycles_differ && !diverged && ref_bus.mmio_reads == jit_bus.mmio_reads &&
               ref_bus.mmio_writes == jit_bus.mmio_writes;
    }
};

#define REQUIRE_NATIVE(jit)                              \
    if (!(jit).available()) {                            \
        MESSAGE("CPU JIT not available in this build");  \
        return;                                          \
    }

}  // namespace cpu_jit_test
