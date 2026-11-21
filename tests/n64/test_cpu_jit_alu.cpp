// CPU JIT: the integer ALU runs as native code, with the interpreter's exact results,
// cycles and exceptions.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "cpu_jit_twin.hpp"

using namespace cpu_jit_test;
using ezy::n64::Kind;
using ezy::n64::classify;

namespace {

constexpr std::uint64_t kBase = kKseg0 + kCode;
constexpr std::uint32_t kTlbwi = 0x42000002;

constexpr std::uint32_t sub(int rd, int rs, int rt) { return special(rs, rt, rd, 0, 0x22); }
constexpr std::uint32_t dsub(int rd, int rs, int rt) { return special(rs, rt, rd, 0, 0x2E); }
constexpr std::uint32_t daddi(int rt, int rs, std::int32_t imm) { return i(0x18, rs, rt, imm); }
constexpr std::uint32_t dsll(int rd, int rt, int sa) { return special(0, rt, rd, sa, 0x38); }
constexpr std::uint32_t multu(int rs, int rt) { return special(rs, rt, 0, 0, 0x19); }
constexpr std::uint32_t dmult(int rs, int rt) { return special(rs, rt, 0, 0, 0x1C); }
constexpr std::uint32_t dmultu(int rs, int rt) { return special(rs, rt, 0, 0, 0x1D); }

std::uint32_t exc_code(const Vr4300& cpu) { return static_cast<std::uint32_t>(cpu.cop0().cause() >> 2) & 31; }

}  // namespace

TEST_CASE("CPU JIT runs integer ALU natively and still matches") {
    std::uint64_t native = 0;
    for (unsigned seed = 0; seed < 200; ++seed) {
        CAPTURE(seed);
        Twin t(seed + 1000, Twin::Generated{.edge_values = true, .overflow_ops = true});
        t.lockstep = seed < 20;
        t.run_until_pc(t.end);
        CHECK(t.same());
        native += t.jit.stats().native;
    }
    if (Twin{}.jit.available()) CHECK(native > 0);
}

TEST_CASE("CPU JIT 64-bit ops in 32-bit user mode still raise RI through the interpreter") {
    CHECK(classify(dsll(9, 8, 4), true) == Kind::Native);
    CHECK(classify(dsll(9, 8, 4), false) == Kind::InPlace);
    CHECK(classify(dmult(8, 9), false) == Kind::InPlace);
    CHECK(classify(addu(9, 8, 8), false) == Kind::Native);
    Twin t;
    t.lockstep = true;
    // User page 0x10000 -> physical 0x20000, written through TLBWI by step().
    t.poke32(0x7000, kTlbwi);
    for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) {
        cpu->cop0().write(Cop0::kStatus, 0);
        cpu->cop0().write(Cop0::kIndex, 0);
        cpu->cop0().write(Cop0::kPageMask, 0);
        cpu->cop0().write(Cop0::kEntryHi, 0x10000);
        cpu->cop0().write(Cop0::kEntryLo0, ((0x20000 >> 12) << 6) | (3U << 3) | 6U);
        cpu->cop0().write(Cop0::kEntryLo1, ((0x21000 >> 12) << 6) | (3U << 3) | 6U);
        cpu->set_pc(kKseg0 + 0x7000);
        cpu->step();
    }
    t.program(0x20000, {addiu(8, 0, 5), addu(10, 8, 8), dsll(9, 8, 4), addiu(11, 0, 1)});
    // The handler runs in kernel mode (EXL), where the same 64-bit ops are native.
    t.program(0x180, {addiu(12, 0, 3), dsll32(13, 12, 3), dmult(12, 13), mflo(14), kStop});
    t.start(0x10000, 0x10);  // user mode, UX = 0
    t.run_until_pc(kVector + 16);
    CHECK(t.same());
    CHECK(exc_code(t.jit_cpu) == 10);  // RI
    CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == 0x10008);
    CHECK(t.jit_cpu.gpr(9) == 0);
    CHECK(t.jit_cpu.gpr(10) == 10);
    CHECK(t.jit_cpu.gpr(14) == 9ULL << 35);
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().native == 4);  // the handler; the user code ran in the interpreter
}

TEST_CASE("CPU JIT counts native instructions and their static multiply cycles") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    t.program(kCode, {addiu(8, 0, -7), addiu(9, 0, 3), mult(8, 9), multu(8, 9), dmult(8, 9), dmultu(8, 9), mflo(10),
                      mfhi(11), kStop});
    t.start(kBase);
    constexpr std::uint64_t kCycles = 1 + 1 + 6 + 6 + 9 + 9 + 1 + 1;
    CHECK(t.jit.execute(kCycles).instructions == 0);  // strict: the whole block must fit below the budget
    const auto r = t.jit.execute(kCycles + 1);
    CHECK(r.instructions == 8);
    CHECK(r.cycles == kCycles);
    CHECK(t.jit.stats().native == 8);
    CHECK(t.jit.stats().in_place == 0);
    std::uint64_t ref = 0;
    for (int k = 0; k < 8; ++k) ref += static_cast<std::uint64_t>(t.ref_cpu.step());
    CHECK(ref == kCycles);
    CHECK(t.same());
    CHECK(t.jit_cpu.pc() == kBase + 32);
}

TEST_CASE("CPU JIT leaves an overflowing instruction to the interpreter, before it") {
    struct Case {
        const char* name;
        std::uint32_t word;
    };
    // r8 = INT32_MAX, r9 = 1, r12 = INT64_MIN, r13 = -1, r14 = INT32_MIN.
    const Case cases[] = {
        {"ADD", add(10, 8, 9)},        {"ADD to r0", add(0, 8, 9)},   {"ADDI", addi(10, 8, 1)},
        {"SUB", sub(10, 14, 9)},       {"DADD", dadd(10, 12, 13)},    {"DSUB", dsub(10, 12, 9)},
        {"DADDI", daddi(10, 12, -1)},  {"DADDI to r0", daddi(0, 12, -1)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Twin t;
        t.lockstep = true;
        t.program(kCode, {lui(8, 0x7FFF), ori(8, 8, 0xFFFF), addiu(9, 0, 1), addiu(12, 0, 1), dsll32(12, 12, 31),
                          addiu(13, 0, -1), lui(14, 0x8000), c.word, addiu(11, 0, 1)});
        t.poke32(0x180, kStop);
        t.start(kBase);
        t.run_until_pc(kVector);
        CHECK(t.same());
        CHECK(exc_code(t.jit_cpu) == 12);  // Ov
        CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == kBase + 28);
        CHECK(t.jit_cpu.gpr(10) == 0);
        CHECK(t.jit_cpu.gpr(11) == 0);
        if (!t.jit.available()) continue;
        CHECK(t.jit.stats().native == 7);  // not the overflowing one
        CHECK(t.jit.stats().exits_before == 1);  // the interpreter then resumes mid-block: no new block
    }
    SUBCASE("no overflow: the result is written") {
        Twin t;
        t.lockstep = true;
        t.program(kCode, {lui(8, 0x3FFF), addi(9, 8, -1), add(10, 9, 9), dadd(11, 8, 8), daddi(12, 11, -2),
                          addiu(15, 0, -1), add(13, 15, 15), sub(14, 15, 8), kStop});
        t.start(kBase);
        t.run_until_pc(kBase + 32);
        CHECK(t.same());
        CHECK(t.jit_cpu.gpr(10) == 0x7FFDFFFEULL);
        CHECK(t.jit_cpu.gpr(11) == 0x7FFE0000ULL);
        CHECK(t.jit_cpu.gpr(12) == 0x7FFDFFFEULL);
        CHECK(t.jit_cpu.gpr(13) == 0xFFFFFFFFFFFFFFFEULL);
        CHECK(t.jit_cpu.gpr(14) == 0xFFFFFFFFC000FFFFULL);
        if (t.jit.available()) CHECK(t.jit.stats().native == 8);
    }
}

TEST_CASE("CPU JIT writes the pc state before helpers and on exits") {
    SUBCASE("native instructions after a delay slot, then the end of the block") {
        // The second block starts right after a delay slot (in_delay_slot_ was true) and
        // runs only native instructions: its exit must leave what step() would.
        Twin t;
        t.lockstep = true;
        t.program(kCode, {beq(0, 0, 1), nop(), addiu(8, 0, 1), addiu(9, 8, 1), kStop});
        t.start(kBase);
        t.run_until_pc(kBase + 16);
        CHECK(t.same());
        if (t.jit.available()) CHECK(t.jit.stats().native == 4);  // the branch and its slot too
    }
    SUBCASE("native instructions, then an in-place DIV, then an MMIO load that exits before") {
        Twin t(TwinOptions{.mem = 0x200000, .direct = 0x100000});
        t.lockstep = true;
        t.program(kCode, {addiu(8, 0, 40), addiu(9, 0, 3), mips::div(8, 9), mflo(10), lui(11, 0xA010), addiu(12, 10, 1),
                          lw(13, 11, 0), addiu(14, 13, 1), kStop});
        t.start(kBase);
        t.run_until_pc(kBase + 32);
        CHECK(t.same());
        CHECK(t.jit_cpu.gpr(12) == 14);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.stats().exits_before > 0);
        CHECK(t.jit.stats().native == 5);  // the code after the LW resumes mid-block: the interpreter runs it
        CHECK(t.jit.stats().in_place == 1);  // the DIV; the LW exits before and step() runs it
    }
}

TEST_CASE("CPU JIT runs a branch and its delay slot natively") {
    Twin t;
    t.lockstep = true;
    t.program(kCode, {addiu(8, 0, 1), bne(8, 0, 2), addiu(9, 0, 2), addiu(10, 0, 3), addu(11, 8, 9), kStop});
    t.start(kBase);
    t.run_until_pc(kBase + 20);
    CHECK(t.same());
    CHECK(t.jit_cpu.gpr(10) == 0);
    CHECK(t.jit_cpu.gpr(11) == 3);
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().native == 4);  // 0x00, the branch, its slot and 0x10
    CHECK(t.jit.stats().in_place == 0);
}

TEST_CASE("CPU JIT keeps its blocks when only CU1 changes") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    t.program(kCode, {addiu(8, 8, 1), addiu(9, 9, 1), beq(0, 0, -3), nop()});
    t.start(kBase);
    CHECK(t.jit.execute(5).instructions == 4);
    t.jit_cpu.cop0().write(Cop0::kStatus, Cop0::kCU1);
    CHECK(t.jit.execute(5).instructions == 4);
    t.jit_cpu.cop0().write(Cop0::kStatus, 0);
    CHECK(t.jit.execute(5).instructions == 4);
    CHECK(t.jit.stats().compiled == 1);
}

TEST_CASE("CPU JIT stops before Count reaches Compare with static multiply cycles") {
    // Blocks of 4 with MULT (6 cycles) and DMULTU (9): Compare falls on a multiply, on a
    // one-cycle instruction or between blocks, depending on its value.
    std::vector<std::uint32_t> w;
    for (int g = 0; g < 8; ++g) {
        w.push_back(addiu(8, 8, 1));
        w.push_back(g % 3 == 0 ? mult(8, 9) : g % 3 == 1 ? dmultu(8, 8) : addiu(9, 9, 2));
        w.push_back(beq(0, 0, 1));
        w.push_back(nop());
    }
    w.push_back(kStop);
    for (std::uint32_t compare = 3; compare < 70; ++compare) {
        CAPTURE(compare);
        Twin t;
        t.lockstep = true;
        t.program(kCode, w);
        t.poke32(0x180, kStop);
        t.start(kBase, Cop0::kIE | (1U << 15));  // IM7
        for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) {
            cpu->cop0().write(Cop0::kCount, 0);
            cpu->cop0().write(Cop0::kCompare, compare);
        }
        t.run_until_pc(kVector);
        CHECK(t.same());
        CHECK((t.jit_cpu.cop0().cause() & Cop0::kIP7) != 0);
    }
}
