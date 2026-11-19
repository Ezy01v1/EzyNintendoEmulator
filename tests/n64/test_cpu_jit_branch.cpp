// CPU JIT: branches run as native code, their delay slots natively or in place, with the
// interpreter's exact links, exit states (pc_, next_pc_, branch_taken_, in_delay_slot_),
// cycles and idle-loop arming. Every program runs in lockstep with the interpreter.
#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "cpu_jit_twin.hpp"

using namespace cpu_jit_test;
using ezy::n64::Kind;
using ezy::n64::classify;

namespace {

constexpr std::uint64_t kBase = kKseg0 + kCode;

constexpr std::uint32_t jalr(int rd, int rs) { return special(rs, 0, rd, 0, 0x09); }
constexpr std::uint32_t regimm(int rs, int rt, std::int32_t words) { return i(0x01, rs, rt, words); }
constexpr std::uint32_t bc1(int nd_tf, std::int32_t words) { return i(0x11, 0x08, nd_tf, words); }  // BC1F/T(L)

struct Branch {
    const char* name;
    // At 0x14, to 0x20 (offset 2 from its slot at 0x18), comparing r8 (and r9).
    std::uint32_t word;
    bool likely = false;
    bool can_take = true, can_fall = true;  // with the test's register values
};

// Every native branch kind. r31 starts at 0x5A5A, so BLTZAL r31 is never taken unless the
// link were written before the compare.
std::vector<Branch> branch_kinds() {
    return {
        {"J", j(0x02, kCode + 0x20), false, true, false},
        {"JAL", jal(kCode + 0x20), false, true, false},
        {"JR", jr(20), false, true, false},
        {"JALR", jalr(21, 20), false, true, false},
        {"JALR rd = rs", jalr(20, 20), false, true, false},
        {"BEQ", beq(8, 9, 2)},
        {"BNE", bne(8, 9, 2)},
        {"BLEZ", i(0x06, 8, 0, 2)},
        {"BGTZ", i(0x07, 8, 0, 2)},
        {"BEQL", beql(8, 9, 2), true},
        {"BNEL", i(0x15, 8, 9, 2), true},
        {"BLEZL", i(0x16, 8, 0, 2), true},
        {"BGTZL", i(0x17, 8, 0, 2), true},
        {"BLTZ", regimm(8, 0x00, 2)},
        {"BGEZ", regimm(8, 0x01, 2)},
        {"BLTZL", regimm(8, 0x02, 2), true},
        {"BGEZL", regimm(8, 0x03, 2), true},
        {"BLTZAL", regimm(8, 0x10, 2)},
        {"BGEZAL", regimm(8, 0x11, 2)},
        {"BLTZALL", regimm(8, 0x12, 2), true},
        {"BGEZALL", regimm(8, 0x13, 2), true},
        {"BLTZAL r31", regimm(31, 0x10, 2), false, false, true},
        {"BGEZALL r31", regimm(31, 0x13, 2), true, true, false},
        {"B (BEQ r0, r0)", beq(0, 0, 2), false, true, false},
        {"BAL (BGEZAL r0)", regimm(0, 0x11, 2), false, true, false},
        {"BEQL r0, r0", beql(0, 0, 2), true, true, false},
        {"BNE r8, r8 (never)", bne(8, 8, 2), false, false, true},
        {"BNEL r0, r0 (never)", i(0x15, 0, 0, 2), true, false, true},
        {"BGTZ r0 (never)", i(0x07, 0, 0, 2), false, false, true},
        {"BLTZL r0 (never)", regimm(0, 0x02, 2), true, false, true},
    };
}

// 0x00..0x10 set r8, r9, r20 (= the target) and r28 (the data zone); 0x14
// the branch, 0x18 its slot, 0x1C the fall-through, 0x20 the target; the stop at 0x2C.
std::vector<std::uint32_t> branch_program(std::uint32_t branch, std::uint32_t slot, std::int32_t v8, std::int32_t v9) {
    return {
        addiu(8, 0, v8),                                                   // 0x00
        addiu(9, 0, v9),                                                   // 0x04
        lui(20, static_cast<std::int32_t>((kBase + 0x20) >> 16)),          // 0x08
        ori(20, 20, static_cast<std::int32_t>((kBase + 0x20) & 0xFFFF)),  // 0x0C
        lui(28, static_cast<std::int32_t>((kKseg0 + kData) >> 16)),        // 0x10
        branch,                                                            // 0x14
        slot,                                                              // 0x18
        addiu(11, 0, 1),                                                   // 0x1C fall-through only
        addiu(12, 31, 0),                                                  // 0x20 target: copy the link
        addiu(13, 21, 0),                                                  // 0x24
        addiu(15, 20, 0),                                                  // 0x28
        kStop,                                                             // 0x2C
    };
}

void start_program(Twin& t, const std::vector<std::uint32_t>& w) {
    t.program(kCode, w);
    for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) cpu->set_gpr(31, 0x5A5A);  // a stale link
    t.start(kBase);
}

void run_until_any(Twin& t, std::initializer_list<std::uint64_t> pcs) {
    const auto at_end = [&] {
        for (const std::uint64_t pc : pcs)
            if (t.jit_cpu.pc() == pc) return true;
        return false;
    };
    for (unsigned k = 0; k < 10000 && !at_end(); ++k) t.advance();
    REQUIRE(at_end());
}

}  // namespace

TEST_CASE("CPU JIT classifies branches as native, except BC1T/BC1F") {
    for (const Branch& b : branch_kinds()) {
        CAPTURE(b.name);
        CHECK(classify(b.word, true) == Kind::Native);
        CHECK(classify(b.word, false) == Kind::Native);
    }
    for (int nd_tf = 0; nd_tf < 4; ++nd_tf) CHECK(classify(bc1(nd_tf, 2), true) == Kind::InPlace);
}

TEST_CASE("CPU JIT native branches match on every branch kind, taken and not") {
    struct Slot {
        const char* name;
        std::uint32_t word;
        bool in_place;
    };
    const Slot slots[] = {
        {"native slot", addiu(10, 10, 1), false},
        {"native slot reading the link", addu(10, 31, 21), false},
        {"native slot writing the compared register", addiu(8, 8, 5), false},
        {"in-place slot (DIV)", mips::div(8, 14), true},
        {"memory slot in RAM (LW)", lw(10, 28, 8), true},
    };
    const std::int32_t pairs[][2] = {{-1, -1}, {0, 0}, {1, 1}, {-1, 1}, {1, -1}, {0, 1}};
    for (const Branch& b : branch_kinds()) {
        bool seen_taken = false, seen_not_taken = false;
        for (const Slot& s : slots) {
            for (const auto& p : pairs) {
                CAPTURE(b.name);
                CAPTURE(s.name);
                CAPTURE(p[0]);
                CAPTURE(p[1]);
                Twin t;
                t.lockstep = true;
                for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) cpu->set_gpr(14, 3);  // the DIV's divisor
                start_program(t, branch_program(b.word, s.word, p[0], p[1]));
                t.run_until_pc(kBase + 0x2C);
                CHECK(t.same());
                const bool taken = t.jit_cpu.gpr(11) == 0;
                (taken ? seen_taken : seen_not_taken) = true;
                REQUIRE_NATIVE(t.jit);
                const bool slot_ran = !b.likely || taken;
                CHECK(t.jit.stats().in_place == (s.in_place && slot_ran ? 1 : 0));
                CHECK(t.jit.stats().native + t.jit.stats().in_place == t.jit_instructions);
                // Everything but the stop: 0x00-0x10, the branch, the slot unless nullified,
                // 0x1C when not taken, 0x20-0x28.
                CHECK(t.jit_instructions == 5 + 1 + (slot_ran ? 1 : 0) + (taken ? 0 : 1) + 3);
                CHECK(t.jit.stats().exits_before == 0);
            }
        }
        CAPTURE(b.name);
        CHECK(seen_taken == b.can_take);
        CHECK(seen_not_taken == b.can_fall);
    }
}

TEST_CASE("CPU JIT native branch with a delay slot that exits before keeps the delay-slot state") {
    // slot = LW from MMIO (kseg1 0xA4400010): interpreter runs it with in_delay_slot
    struct Slot {
        const char* name;
        std::uint32_t word;
        bool raises;  // with Cause.BD and EPC at the branch
    };
    const Slot slots[] = {
        {"MMIO LW", lw(10, 16, 0x10), false},
        {"ADD overflow (native, exits before)", add(10, 17, 17), true},
        {"kuseg LW (TLB)", lw(10, 0, 0x100), true},
    };
    const std::int32_t pairs[][2] = {{-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
    for (const Branch& b : branch_kinds()) {
        for (const Slot& s : slots) {
            for (const auto& p : pairs) {
                CAPTURE(b.name);
                CAPTURE(s.name);
                CAPTURE(p[0]);
                CAPTURE(p[1]);
                Twin t(TwinOptions{.mem = 0x200000, .direct = 0x100000});
                t.lockstep = true;
                std::vector<std::uint32_t> w = branch_program(b.word, s.word, p[0], p[1]);
                t.poke32(0x000, kStop);  // TLB refill vector
                t.poke32(0x180, kStop);
                for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) {
                    cpu->set_gpr(16, 0xFFFFFFFFA4400000ULL);  // kseg1 MMIO, beyond direct RAM
                    cpu->set_gpr(17, 0x7FFFFFFF);
                }
                start_program(t, w);
                t.reset_mmio_counts();
                run_until_any(t, {kBase + 0x2C, kKseg0, kVector});
                CHECK(t.same());
                const bool at_vector = t.jit_cpu.pc() != kBase + 0x2C;
                const bool slot_ran = s.raises ? at_vector : t.jit_bus.mmio_reads == 1;
                CHECK(at_vector == (s.raises && slot_ran));
                if (at_vector) {
                    CHECK((t.jit_cpu.cop0().cause() & Cop0::kBD) != 0);
                    CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == kBase + 0x14);
                }
                if (!b.likely) CHECK(slot_ran);
                REQUIRE_NATIVE(t.jit);
                CHECK(t.jit.stats().in_place == 0);
                CHECK(t.jit.stats().native == t.jit_instructions);
                CHECK((t.jit.stats().exits_before > 0) == slot_ran);
            }
        }
    }
}

TEST_CASE("CPU JIT native self-branch arms idle loop skipping") {
    SUBCASE("BEQ r0, r0, -1") {
        Twin t;
        REQUIRE_NATIVE(t.jit);
        t.program(kCode, {addiu(8, 0, 1), beq(0, 0, -1), nop()});
        t.start(kBase);
        CHECK(t.jit.execute(4).instructions == 3);
        CHECK(t.jit_cpu.pc() == kBase + 4);
        CHECK(t.jit_cpu.idle_loop_cycles() == 2);
        CHECK(t.jit.stats().native == 3);
        CHECK(t.jit.stats().in_place == 0);
        for (int k = 0; k < 3; ++k) t.ref_cpu.step();
        t.ref_cycles = t.jit_cycles = 0;
        CHECK(t.same());
    }
    SUBCASE("J to itself") {
        Twin t;
        REQUIRE_NATIVE(t.jit);
        t.program(kCode, {addiu(8, 0, 1), j(0x02, kCode + 4), nop()});
        t.start(kBase);
        CHECK(t.jit.execute(4).instructions == 3);
        CHECK(t.jit_cpu.idle_loop_cycles() == 2);
        CHECK(t.jit.stats().native == 3);
    }
    SUBCASE("a not-taken self-branch and a JR elsewhere keep the armed candidate") {
        // 0x00: B . (arms 0x00); then, from 0x40: BNE r8 (= 0), r0, . not taken, JR r9
        // (not itself) back to 0x00, whose arrival must still find the candidate armed.
        Twin t;
        REQUIRE_NATIVE(t.jit);
        t.program(kCode, {beq(0, 0, -1), nop()});
        t.program(kCode + 0x40, {addiu(8, 0, 0), bne(8, 0, -1), nop(), lui(9, static_cast<std::int32_t>(kBase >> 16)),
                                 ori(9, 9, static_cast<std::int32_t>(kBase & 0xFFFF)), jr(9), nop()});
        t.start(kBase);
        CHECK(t.jit.execute(3).instructions == 2);  // one iteration of B . arms it
        for (int k = 0; k < 2; ++k) t.ref_cpu.step();
        CHECK(t.ref_cpu.idle_loop_cycles() == 2);
        CHECK(t.jit_cpu.idle_loop_cycles() == 2);
        for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) cpu->set_pc(kBase + 0x40);
        CHECK(t.jit.execute(8).instructions == 7);  // blocks of 3 and 4, stopping on arrival at 0x00
        for (int k = 0; k < 7; ++k) t.ref_cpu.step();
        CHECK(t.jit_cpu.pc() == kBase);
        CHECK(t.ref_cpu.idle_loop_cycles() == 2);
        CHECK(t.jit_cpu.idle_loop_cycles() == 2);
        CHECK(t.same());
        CHECK(t.jit.stats().in_place == 0);
    }
    SUBCASE("JR to itself arms the candidate as the interpreter does") {
        // idle_loop_cycles() only accepts B . and J .: once JR . armed the candidate,
        // rewriting it into B . must find the loop armed on both machines.
        Twin t;
        t.lockstep = true;
        t.program(kCode, {lui(9, static_cast<std::int32_t>((kBase + 8) >> 16)),
                          ori(9, 9, static_cast<std::int32_t>((kBase + 8) & 0xFFFF)), jr(9), nop()});
        t.start(kBase);
        for (int k = 0; k < 4; ++k) t.advance();
        CHECK(t.same());
        CHECK(t.jit_cpu.pc() == kBase + 8);
        t.poke32(kCode + 8, beq(0, 0, -1));
        t.jit.invalidate_ram(kCode + 8, kCode + 12);
        CHECK(t.ref_cpu.idle_loop_cycles() == 2);
        CHECK(t.jit_cpu.idle_loop_cycles() == 2);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.stats().in_place == 0);
    }
    // The register-dependent forms arm it through a select (BNE) or after the likely
    // branch's skip exit (BEQL). As above, the loop is rewritten into B . afterwards.
    struct SelfBranch {
        const char* name;
        std::vector<std::uint32_t> words;  // the self-branch is the third word, its slot a NOP
    };
    const SelfBranch self_branches[] = {
        {"BNE r8, r0, . taken once", {addiu(8, 0, 1), addiu(9, 0, 0), bne(8, 0, -1), nop()}},
        {"BEQL r8, r9, . likely taken", {addiu(8, 0, 1), addiu(9, 0, 1), beql(8, 9, -1), nop()}},
    };
    for (const SelfBranch& s : self_branches) {
        SUBCASE(s.name) {
            Twin t;
            REQUIRE_NATIVE(t.jit);
            t.program(kCode, s.words);
            t.start(kBase);
            CHECK(t.jit.execute(5).instructions == 4);  // the first block only: taken once
            for (int k = 0; k < 4; ++k) t.ref_cpu.step();
            CHECK(t.jit_cpu.pc() == kBase + 8);
            t.poke32(kCode + 8, beq(0, 0, -1));
            t.jit.invalidate_ram(kCode + 8, kCode + 12);
            CHECK(t.ref_cpu.idle_loop_cycles() == 2);
            CHECK(t.jit_cpu.idle_loop_cycles() == 2);
            CHECK(t.jit.stats().native == 4);
            CHECK(t.jit.stats().in_place == 0);
            t.ref_cycles = t.jit_cycles = 0;
            CHECK(t.same());
        }
    }
}

TEST_CASE("CPU JIT native JR and JALR to an unaligned target leave the fetch to the interpreter") {
    for (const bool link : {false, true}) {
        CAPTURE(link);
        Twin t;
        t.lockstep = true;
        t.program(kCode, {lui(8, 0x8000), ori(8, 8, kCode + 0x42), link ? jalr(31, 8) : jr(8), addiu(9, 0, 1), kStop});
        t.poke32(0x180, kStop);
        t.start(kBase);
        t.run_until_pc(kVector);
        CHECK(t.same());
        CHECK(((t.jit_cpu.cop0().cause() >> 2) & 31) == 4);  // AdEL
        CHECK(t.jit_cpu.cop0().read(Cop0::kBadVAddr) == kBase + 0x42);
        CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == kBase + 0x42);
        CHECK((t.jit_cpu.cop0().cause() & Cop0::kBD) == 0);
        CHECK(t.jit_cpu.gpr(9) == 1);
        CHECK(t.jit_cpu.gpr(31) == (link ? kBase + 0x10 : 0));
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.stats().native == 4);
        CHECK(t.jit.stats().in_place == 0);
    }
}

TEST_CASE("CPU JIT keeps BC1T/BC1F and their delay slots in place") {
    for (int nd_tf = 0; nd_tf < 4; ++nd_tf) {
        CAPTURE(nd_tf);
        Twin t;
        t.lockstep = true;
        t.program(kCode, {addiu(8, 0, 1), bc1(nd_tf, 2), addiu(10, 0, 1), addiu(11, 0, 1), addiu(12, 0, 1), kStop});
        t.start(kBase, Cop0::kCU1);
        t.run_until_pc(kBase + 0x14);
        CHECK(t.same());
        REQUIRE_NATIVE(t.jit);
        const bool nullified = nd_tf == 3;  // BC1TL with the condition clear
        CHECK(t.jit.stats().in_place == (nullified ? 1 : 2));
    }
}

TEST_CASE("CPU JIT native branch slots take their extra cycles against Compare") {
    // A DIV (38 cycles) in place in the slot of a native branch, a MULT (6) natively.
    for (const std::uint32_t slot : {mips::div(8, 9), mult(8, 9)}) {
        for (std::uint32_t compare = 2; compare < 60; ++compare) {
            CAPTURE(slot);
            CAPTURE(compare);
            Twin t;
            t.lockstep = true;
            t.program(kCode, {addiu(8, 0, 7), addiu(9, 0, 3), bne(8, 0, 2), slot, addiu(10, 0, 1), addiu(11, 8, 1),
                              beq(0, 0, 2), slot, nop(), addiu(12, 0, 1), kStop});
            t.poke32(0x180, kStop);
            t.start(kBase, Cop0::kIE | (1U << 15));  // IM7
            for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) {
                cpu->cop0().write(Cop0::kCount, 0);
                cpu->cop0().write(Cop0::kCompare, compare);
            }
            run_until_any(t, {kVector, kBase + 0x28});
            CHECK(t.same());
        }
    }
}
