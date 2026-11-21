#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

#include "cpu_jit_twin.hpp"
#include "n64/n64.hpp"
#include "n64_test_helpers.hpp"

using namespace cpu_jit_test;

namespace {

// Random integer programs: 32/64-bit ALU, LUI, MULT/DIV and family, forward branches
// (all kinds, with their delay slots) and aligned loads/stores to a kseg0 data zone,
// wrapped in a 3-iteration loop so cached blocks run again; it ends at `end`.
class Generator {
public:
    Generator(unsigned seed, bool overflow_ops) : rng_(seed), overflow_ops_(overflow_ops) {}

    std::uint32_t pick(std::uint32_t n) { return static_cast<std::uint32_t>(rng_() % n); }
    int reg() { return static_cast<int>(1 + pick(22)); }       // r1..r22
    int dst() { return pick(16) == 0 ? 0 : reg(); }              // sometimes r0

    std::uint32_t alu() {
        const int d = dst(), s = reg(), t = reg();
        const int sa = static_cast<int>(pick(32));
        const std::int32_t imm = static_cast<std::int32_t>(pick(0x10000)) - 0x8000;
        switch (pick(overflow_ops_ ? 46 : 40)) {
            case 0: return special(s, t, d, 0, 0x21);    // ADDU
            case 1: return special(s, t, d, 0, 0x23);    // SUBU
            case 2: return special(s, t, d, 0, 0x24);    // AND
            case 3: return special(s, t, d, 0, 0x25);    // OR
            case 4: return special(s, t, d, 0, 0x26);    // XOR
            case 5: return special(s, t, d, 0, 0x27);    // NOR
            case 6: return special(s, t, d, 0, 0x2A);    // SLT
            case 7: return special(s, t, d, 0, 0x2B);    // SLTU
            case 8: return special(0, t, d, sa, 0x00);   // SLL
            case 9: return special(0, t, d, sa, 0x02);   // SRL
            case 10: return special(0, t, d, sa, 0x03);  // SRA
            case 11: return special(s, t, d, 0, 0x04);   // SLLV
            case 12: return special(s, t, d, 0, 0x06);   // SRLV
            case 13: return special(s, t, d, 0, 0x07);   // SRAV
            case 14: return special(s, t, d, 0, 0x2D);   // DADDU
            case 15: return special(s, t, d, 0, 0x2F);   // DSUBU
            case 16: return special(0, t, d, sa, 0x38);  // DSLL
            case 17: return special(0, t, d, sa, 0x3A);  // DSRL
            case 18: return special(0, t, d, sa, 0x3B);  // DSRA
            case 19: return special(0, t, d, sa, 0x3C);  // DSLL32
            case 20: return special(0, t, d, sa, 0x3E);  // DSRL32
            case 21: return special(0, t, d, sa, 0x3F);  // DSRA32
            case 22: return special(s, t, d, 0, 0x14);   // DSLLV
            case 23: return special(s, t, d, 0, 0x16);   // DSRLV
            case 24: return special(s, t, d, 0, 0x17);   // DSRAV
            case 25: return i(0x09, s, d, imm);          // ADDIU
            case 26: return i(0x0A, s, d, imm);          // SLTI
            case 27: return i(0x0B, s, d, imm);          // SLTIU
            case 28: return i(0x0C, s, d, imm);          // ANDI
            case 29: return i(0x0D, s, d, imm);          // ORI
            case 30: return i(0x0E, s, d, imm);          // XORI
            case 31: return i(0x0F, 0, d, imm);          // LUI
            case 32: return i(0x19, s, d, imm);          // DADDIU
            case 33: return special(0, 0, d, 0, pick(2) ? 0x10 : 0x12);  // MFHI, MFLO
            case 34: return special(s, 0, 0, 0, pick(2) ? 0x11 : 0x13);  // MTHI, MTLO
            case 35: return special(s, t, 0, 0, 0x18 + static_cast<int>(pick(8)));  // MULT..DDIVU
            case 36: return special(s, t, 0, 0, 0x18 + static_cast<int>(pick(2)));  // MULT, MULTU
            case 37: return special(0, 0, 0, 0, 0x0F);   // SYNC
            case 38: return i(0x2F, 28, static_cast<int>(pick(32)), static_cast<std::int32_t>(pick(kDataBytes)));  // CACHE
            case 40: return special(s, t, d, 0, 0x20);   // ADD
            case 41: return special(s, t, d, 0, 0x22);   // SUB
            case 42: return special(s, t, d, 0, 0x2C);   // DADD
            case 43: return special(s, t, d, 0, 0x2E);   // DSUB
            case 44: return i(0x08, s, d, imm);          // ADDI
            case 45: return i(0x18, s, d, imm);          // DADDI
            default: return special(s, t, d, 0, 0x25);  // OR
        }
    }

    std::uint32_t any() { return pick(4) == 0 ? memory() : alu(); }

    std::uint32_t memory() {
        struct Op { int op; std::uint32_t bytes; bool any_offset; };
        static constexpr Op kOps[] = {
            {0x20, 1, false}, {0x24, 1, false}, {0x21, 2, false}, {0x25, 2, false}, {0x23, 4, false},
            {0x27, 4, false}, {0x37, 8, false}, {0x30, 4, false}, {0x34, 8, false}, {0x28, 1, false},
            {0x29, 2, false}, {0x2B, 4, false}, {0x3F, 8, false}, {0x38, 4, false}, {0x3C, 8, false},
            {0x22, 4, true},  {0x26, 4, true},  {0x1A, 8, true},  {0x1B, 8, true},  {0x2A, 4, true},
            {0x2E, 4, true},  {0x2C, 8, true},  {0x2D, 8, true},
        };
        const Op& o = kOps[pick(sizeof(kOps) / sizeof(kOps[0]))];
        std::uint32_t offset = pick(kDataBytes - 8);
        if (!o.any_offset) offset &= ~(o.bytes - 1);
        return i(o.op, 28, dst(), static_cast<std::int32_t>(offset));
    }

    // Body instructions; index n is the loop tail. Branch offsets are filled in at the end,
    // once no target can land inside a register-jump sequence (it would jump through a stale
    // r27, possibly backwards).
    std::vector<std::uint32_t> body(std::uint32_t base, std::uint32_t n) {
        struct Fixup {
            std::uint32_t at, target;
            int kind;  // 0: 16-bit offset, 1: J/JAL, 2: LUI+ORI of r27
        };
        std::vector<std::uint32_t> w;
        std::vector<Fixup> fixups;
        std::vector<bool> interior(n + 1, false);
        while (w.size() < n) {
            const auto at = static_cast<std::uint32_t>(w.size());
            const std::uint32_t room = n - at;
            if (room >= 4 && pick(10) == 0) {  // branch + slot, or a register jump
                const std::uint32_t target = at + 2 + pick(std::min<std::uint32_t>(16, room - 1));  // <= n
                const int s = reg(), t = reg();
                switch (pick(9)) {
                    case 0: w.push_back(i(0x04 + static_cast<int>(pick(4)), s, t, 0)); break;  // BEQ BNE BLEZ BGTZ
                    case 1: w.push_back(i(0x14 + static_cast<int>(pick(4)), s, t, 0)); break;  // likely forms
                    case 2: w.push_back(i(0x01, s, static_cast<int>(pick(4)), 0)); break;        // BLTZ BGEZ (L)
                    case 3: w.push_back(i(0x01, s, 0x10 + static_cast<int>(pick(4)), 0)); break; // ...AL forms
                    case 4:                                                                       // J, JAL
                        w.push_back(j(0x02 + static_cast<int>(pick(2)), 0));
                        fixups.push_back({at, target, 1});
                        break;
                    case 5: w.push_back(i(0x04, 0, 0, 0)); break;  // B
                    default:
                        if (target < at + 4) {
                            w.push_back(i(0x05, s, t, 0));
                            break;
                        }
                        // JR / JALR through r27, which the generator never writes otherwise.
                        w.push_back(lui(27, 0));
                        w.push_back(ori(27, 27, 0));
                        w.push_back(pick(2) ? jr(27) : special(27, 0, dst(), 0, 0x09));
                        fixups.push_back({at, target, 2});
                        interior[at + 1] = interior[at + 2] = true;
                        break;
                }
                if (fixups.empty() || fixups.back().at != at) fixups.push_back({at, target, 0});
                w.push_back(any());  // the delay slot
                continue;
            }
            w.push_back(any());
        }
        for (Fixup f : fixups) {
            while (interior[f.target]) ++f.target;
            const std::uint32_t target_addr = base + 4 * f.target;
            switch (f.kind) {
                case 0: w[f.at] |= static_cast<std::uint32_t>(f.target - (f.at + 1)) & 0xFFFF; break;
                case 1: w[f.at] |= (target_addr >> 2) & 0x3FFFFFF; break;
                default:
                    w[f.at] |= static_cast<std::uint32_t>((kKseg0 + target_addr) >> 16) & 0xFFFF;
                    w[f.at + 1] |= target_addr & 0xFFFF;
                    break;
            }
        }
        return w;
    }

    std::mt19937 rng_;
    bool overflow_ops_;
};

// Words that set register r to an edge value.
std::vector<std::uint32_t> edge_value(int r, std::uint32_t which) {
    switch (which) {
        case 0: return {addiu(r, 0, 0)};
        case 1: return {addiu(r, 0, 1)};
        case 2: return {addiu(r, 0, -1)};
        case 3: return {lui(r, 0x8000)};                                   // INT32_MIN
        case 4: return {lui(r, 0x7FFF), ori(r, r, 0xFFFF)};                // INT32_MAX
        case 5: return {addiu(r, 0, 1), special(0, r, r, 31, 0x3C)};       // INT64_MIN (DSLL32 31)
        default: return {addiu(r, 0, -1), special(0, r, r, 1, 0x3A)};      // INT64_MAX (DSRL 1)
    }
}

}  // namespace

cpu_jit_test::Twin::Twin(unsigned seed, Generated options) : Twin(TwinOptions{.code_budget = options.code_budget}) {
    Generator g(seed, options.overflow_ops);
    budget = 1 + g.pick(300);
    for (std::uint32_t a = 0; a < kDataBytes; ++a) poke8(kData + a, static_cast<std::uint8_t>(g.pick(256)));
    std::vector<std::uint32_t> words;
    words.push_back(lui(28, static_cast<std::int32_t>((kKseg0 + kData) >> 16)));  // data base
    words.push_back(addiu(23, 0, 3));                                          // loop counter
    for (int r = 1; r <= 22; ++r) {
        words.push_back(lui(r, static_cast<std::int32_t>(g.pick(0x10000))));
        words.push_back(ori(r, r, static_cast<std::int32_t>(g.pick(0x10000))));
        if (g.pick(3) == 0) words.push_back(dsll32(r, r, static_cast<int>(g.pick(32))));
        if (options.edge_values && g.pick(3) == 0) {
            const auto edge = edge_value(r, g.pick(7));
            words.insert(words.end(), edge.begin(), edge.end());
        }
    }
    if (options.overflow_ops) {
        // Ov handler: EPC += 4, ERET (k0 = r26 is not used by the programs).
        program(0x180, {mfc0(26, 14), addiu(26, 26, 4), mtc0(26, 14), eret()});
    }
    const auto body_start = static_cast<std::uint32_t>(words.size());
    const std::uint32_t n = 120 + g.pick(130);
    const auto body = g.body(kCode + 4 * body_start, n);
    words.insert(words.end(), body.begin(), body.end());
    words.push_back(addiu(23, 23, -1));
    const auto tail = static_cast<std::int32_t>(words.size());
    words.push_back(bne(23, 0, static_cast<std::int32_t>(body_start) - (tail + 1)));
    words.push_back(nop());
    end = kKseg0 + kCode + 4 * words.size();
    words.push_back(kStop);
    program(kCode, words);
    start(kKseg0 + kCode);
}

TEST_CASE("CPU JIT matches the interpreter on generated integer programs") {
    std::uint64_t jit_instructions = 0;
    for (unsigned seed = 0; seed < 200; ++seed) {
        CAPTURE(seed);
        Twin t(seed);
        t.run_until_pc(t.end);
        CHECK(t.same());
        jit_instructions += t.jit_instructions;
    }
    if (Twin{}.jit.available()) CHECK(jit_instructions > 0);
}

TEST_CASE("CPU JIT matches on branches, likely branches not taken and delay slots") {
    Twin t;
    t.lockstep = true;
    constexpr std::uint64_t kBase = kKseg0 + kCode;
    std::vector<std::uint32_t> w = {
        addiu(8, 0, 1),                 // 0x00
        addiu(9, 0, 2),                 // 0x04
        beql(8, 9, 3),                  // 0x08 not taken: skips 0x0C
        addiu(10, 0, 99),               // 0x0C nullified
        bne(8, 9, 2),                   // 0x10 taken to 0x1C
        addiu(11, 0, 5),                // 0x14 slot runs
        addiu(12, 0, 77),               // 0x18 skipped
        beq(8, 9, 2),                   // 0x1C not taken: slot runs, falls to 0x24
        addiu(13, 0, 6),                // 0x20 slot
        beql(8, 8, 2),                  // 0x24 likely taken to 0x30
        addiu(14, 0, 7),                // 0x28 slot runs
        addiu(15, 0, 88),               // 0x2C skipped
        i(0x01, 0, 0x11, 2),            // 0x30 BGEZAL r0 taken to 0x3C, links r31
        addiu(16, 31, 0),               // 0x34 slot sees the link
        addiu(17, 0, 66),               // 0x38 skipped
        i(0x01, 8, 0x02, 3),            // 0x3C BLTZL r8 not taken: skips 0x40
        addiu(18, 0, 55),               // 0x40 nullified
        jal(kCode + 0x50),              // 0x44 -> 0x50, links 0x4C
        addiu(19, 0, 4),                // 0x48 slot
        j(0x02, kCode + 0x70),          // 0x4C (after JR) -> 0x70; its slot is 0x50 again
        lui(20, static_cast<std::int32_t>((kBase + 0x64) >> 16)),        // 0x50
        ori(20, 20, static_cast<std::int32_t>((kBase + 0x64) & 0xFFFF)),  // 0x54
        special(20, 0, 21, 0, 0x09),    // 0x58 JALR r21, r20 -> 0x64
        addiu(22, 21, 1),               // 0x5C slot sees the link
        nop(),                          // 0x60
        jr(31),                         // 0x64 back to 0x4C
        nop(),                          // 0x68
        nop(),                          // 0x6C
    };
    // 0x70: a block that reaches 64 instructions with a branch in the last position.
    for (int k = 0; k < 63; ++k) w.push_back(addiu(8, 8, 1));
    w.push_back(beq(0, 0, 2));
    w.push_back(addiu(9, 9, 1));
    w.push_back(nop());
    w.push_back(kStop);
    t.program(kCode, w);
    const std::uint64_t end = kBase + 0x70 + 4 * 66;
    t.start(kBase);
    t.run_until_pc(end);
    CHECK(t.same());
    CHECK(t.jit_cpu.gpr(10) == 0);
    CHECK(t.jit_cpu.gpr(11) == 5);
    CHECK(t.jit_cpu.gpr(12) == 0);
    CHECK(t.jit_cpu.gpr(16) == kBase + 0x38);
    CHECK(t.jit_cpu.gpr(18) == 0);
    CHECK(t.jit_cpu.gpr(22) == kBase + 0x61);
    if (t.jit.available()) CHECK(t.jit_instructions > 0);
}

TEST_CASE("CPU JIT matches when an instruction in a block raises an exception") {
    constexpr std::uint64_t kBase = kKseg0 + kCode;
    struct Case {
        const char* name;
        std::vector<std::uint32_t> words;
        std::uint64_t epc;
        bool delay_slot;
    };
    const Case cases[] = {
        {"ADD overflow", {lui(8, 0x7FFF), ori(8, 8, 0xFFFF), addiu(9, 0, 1), mips::div(8, 9), add(10, 8, 9), addiu(11, 0, 1)},
         kBase + 0x10, false},
        {"unaligned LW", {lui(8, 0x8004), addiu(9, 0, 3), lw(10, 8, 2), addiu(11, 0, 1)}, kBase + 0x8, false},
        {"TEQ", {addiu(8, 0, 7), addiu(9, 0, 7), teq(8, 9), addiu(11, 0, 1)}, kBase + 0x8, false},
        {"SYSCALL in a delay slot", {addiu(8, 0, 7), beq(0, 0, 4), syscall(), addiu(11, 0, 1)}, kBase + 0x4, true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Twin t;
        t.lockstep = true;
        t.program(kCode, c.words);
        t.poke32(0x180, kStop);
        t.start(kBase);
        t.run_until_pc(kVector);
        CHECK(t.same());
        CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == c.epc);
        CHECK(((t.jit_cpu.cop0().cause() & Cop0::kBD) != 0) == c.delay_slot);
        CHECK(t.jit_cpu.gpr(11) == 0);
        if (t.jit.available()) CHECK(t.jit_instructions > 0);
    }
}

TEST_CASE("CPU JIT stops before a block that does not fit the budget") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    std::vector<std::uint32_t> w;
    for (int k = 0; k < 10; ++k) w.push_back(addiu(8, 8, 1));
    w.push_back(kStop);
    w.push_back(addiu(9, 0, 1));
    w.push_back(mips::div(8, 9));  // 38 cycles
    for (int k = 0; k < 8; ++k) w.push_back(addiu(10, 10, 1));
    w.push_back(kStop);
    t.program(kCode, w);
    t.start(kKseg0 + kCode);
    auto& jit = t.jit;
    CHECK(jit.execute(3).instructions == 0);
    CHECK(jit.execute(10).instructions == 0);  // strict: 10 cycles need a budget of 11
    const auto all = jit.execute(11);
    CHECK(all.instructions == 10);
    CHECK(all.cycles == 10);
    for (int k = 0; k < 11; ++k) t.ref_cpu.step();
    t.jit_cpu.step();  // the stop
    // A 10-instruction block whose DIV makes it overshoot a budget of 20: it exits right after the DIV.
    const auto part = jit.execute(20);
    CHECK(part.instructions == 2);
    CHECK(part.cycles == 39);
    // The exit left the pc inside the block (not at a jump target): the interpreter finishes it.
    const auto rest = jit.execute(1000);
    CHECK(rest.instructions == 0);
    CHECK(rest.cycles == 0);
    CHECK(jit.stats().resume_skips == 1);
    for (int k = 0; k < 8; ++k) t.jit_cpu.step();
    for (int k = 0; k < 10; ++k) t.ref_cpu.step();
    t.ref_cycles = t.jit_cycles = 0;
    CHECK(t.same());
}

TEST_CASE("CPU JIT stops before Count reaches Compare") {
    // Short blocks (a branch every 4 instructions) and a DIV, so that Compare falls inside
    // a block, on a DIV or on a one-cycle instruction depending on its value.
    std::vector<std::uint32_t> w;
    for (int g = 0; g < 16; ++g) {
        w.push_back(addiu(8, 8, 1));
        w.push_back(g == 6 ? mips::div(8, 9) : addiu(9, 9, 2));
        w.push_back(beq(0, 0, 1));
        w.push_back(nop());
    }
    w.push_back(kStop);
    for (std::uint32_t compare = 3; compare < 48; ++compare) {
        CAPTURE(compare);
        Twin t;
        t.lockstep = true;
        t.program(kCode, w);
        t.poke32(0x180, kStop);
        t.start(kKseg0 + kCode, Cop0::kIE | (1U << 15));  // IM7
        for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) {
            cpu->cop0().write(Cop0::kCount, 0);
            cpu->cop0().write(Cop0::kCompare, compare);
        }
        t.run_until_pc(kVector);
        const bool cause_ip7_set_at_same_instruction = t.same();
        CHECK(cause_ip7_set_at_same_instruction);
        CHECK((t.jit_cpu.cop0().cause() & Cop0::kIP7) != 0);
        if (t.jit.available() && compare > 20) CHECK(t.jit_instructions > 0);
    }
}

TEST_CASE("CPU JIT leaves COP0 and MMIO to the interpreter") {
    Twin t(TwinOptions{.mem = 0x200000, .direct = 0x100000, .penalty = 7});
    t.lockstep = true;
    const std::vector<std::uint32_t> w = {
        lui(8, 0xA410),  // kseg1, beyond direct RAM: MMIO
        addiu(9, 0, 1),
        lw(9, 8, 0),                                // exits before; the interpreter reads it
        addiu(10, 9, 1),
        lui(11, 0xA004), // kseg1 RAM: in place, uncached cycles
        sw(10, 11, 0),
        lw(12, 11, 4),
        mfc0(13, 9),                                // Count: out of block
        addiu(14, 13, 3),
        sw(12, 8, 8),                               // MMIO write
        lui(15, 0x8010), // kseg0 just past direct RAM
        lw(16, 15, 0),
        addiu(17, 0, 1),
        kStop,
    };
    t.program(kCode, w);
    t.start(kKseg0 + kCode);
    t.reset_mmio_counts();
    t.run_until_pc(kKseg0 + kCode + 4 * (w.size() - 1));
    CHECK(t.same());
    CHECK(t.jit_bus.mmio_reads == 2);
    CHECK(t.jit_bus.mmio_writes == 1);
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().out_of_block > 0);
    CHECK(t.jit.stats().exits_before > 0);
    CHECK(t.jit.stats().in_place == 0);  // the loads and stores are native, the uncached ones exit before
}

TEST_CASE("CPU JIT does not compile past a page end or past RAM") {
    SUBCASE("a branch whose slot is on the next page") {
        Twin t;
        t.lockstep = true;
        std::vector<std::uint32_t> w;
        for (int k = 0; k < 7; ++k) w.push_back(addiu(8, 8, 1));  // 0x1FE0..0x1FF8
        w.push_back(beq(0, 0, 2));                                 // 0x1FFC -> 0x2008
        w.push_back(addiu(9, 0, 1));                               // 0x2000 slot
        w.push_back(addiu(10, 0, 1));                              // 0x2004 skipped
        w.push_back(bne(8, 0, 1));                                 // 0x2008 -> 0x2010
        w.push_back(addiu(11, 0, 1));
        w.push_back(kStop);                                        // 0x2010
        t.program(0x1FE0, w);
        t.start(kKseg0 + 0x1FE0);
        t.run_until_pc(kKseg0 + 0x2010);
        CHECK(t.same());
        CHECK(t.jit_cpu.gpr(9) == 1);
        CHECK(t.jit_cpu.gpr(10) == 0);
    }
    SUBCASE("code running up to the end of direct RAM") {
        Twin t(TwinOptions{.mem = 0x20000, .direct = 0xFF80});
        t.lockstep = true;
        std::vector<std::uint32_t> w;
        for (std::uint32_t a = 0xFF00; a < 0x10040; a += 4) w.push_back(addiu(8, 8, 1));
        w.push_back(kStop);
        t.program(0xFF00, w);
        t.start(kKseg0 + 0xFF00);
        t.reset_mmio_counts();
        t.run_until_pc(kKseg0 + 0x10040);
        CHECK(t.same());
        CHECK(t.jit_cpu.gpr(8) == (0x10040 - 0xFF00) / 4);
        CHECK(t.jit_bus.mmio_reads == (0x10040 - 0xFF80) / 4);  // fetches past RAM, by step()
        if (t.jit.available()) CHECK(t.jit_instructions == (0xFF80 - 0xFF00) / 4);
    }
}

TEST_CASE("CPU JIT matches with a tiny code buffer that flushes mid-run") {
    Twin t(7, /*code_budget=*/4 * 1024);
    t.run_until_pc(t.end);
    CHECK(t.same());
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().flushes > 0);
}

TEST_CASE("CPU JIT keeps idle loop detection working") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    t.program(kCode, {addiu(8, 0, 1), addiu(9, 0, 2), beq(0, 0, -1), nop()});
    t.start(kKseg0 + kCode);
    const auto first = t.jit.execute(5);  // the first block only
    CHECK(first.instructions == 4);
    CHECK(t.jit_cpu.pc() == kKseg0 + kCode + 8);
    CHECK(t.jit_cpu.idle_loop_cycles() == 2);
    const auto loop = t.jit.execute(1000);
    CHECK(loop.instructions >= 2);
    CHECK(t.jit_cpu.idle_loop_cycles() == 2);
}

TEST_CASE("CPU JIT leaves a block at an instruction boundary when the RDP worker failed") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    // The renderer failed: the native LW's inline barrier sends it to the slow path, so the
    // block leaves before the LW, the instructions before it retired. The interpreter's
    // step() then makes the access through guard_memory, which rethrows the failure.
    t.jit_bus.failed = true;
    t.jit_bus.throw_at = kData + 0x10;
    t.program(kCode, {addiu(8, 0, 1), lui(9, 0x8004), lw(10, 9, 0x10), addiu(11, 0, 1), kStop});
    t.start(kKseg0 + kCode);
    const auto ran = t.jit.execute(1000);
    CHECK(ran.instructions == 2);  // a block ran: ADDIU, LUI
    CHECK(t.jit.stats().native == 2);
    CHECK(t.jit.stats().exits_before == 1);
    CHECK(t.jit_cpu.pc() == kKseg0 + kCode + 8);  // at the LW, which did not run
    for (int k = 0; k < 2; ++k) t.ref_cpu.step();
    CHECK(state_of(t.ref_cpu) == state_of(t.jit_cpu));  // exactly the boundary step() leaves
    CHECK_THROWS_AS(t.jit_cpu.step(), std::runtime_error);  // the failure reaches the caller
    CHECK(t.jit_cpu.gpr(10) == 0);
    CHECK(t.jit_cpu.gpr(11) == 0);
    t.jit_bus.failed = false;
    t.jit_bus.throw_at = ~0U;
    t.jit.flush();
    CHECK(t.jit.execute(1000).instructions > 0);
}

TEST_CASE("CPU JIT rethrows an RDP worker failure raised inside a block") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    // LWL runs in place: its memory guard throws inside the block. The helper catches it, the
    // block leaves before the LWL and execute() rethrows after retiring what ran, leaving
    // the state the interpreter's step() leaves when it throws at the same instruction.
    t.jit_bus.throw_at = t.ref_bus.throw_at = kData + 0x11;
    t.program(kCode, {addiu(8, 0, 1), lui(9, 0x8004), lwl(10, 9, 0x11), addiu(11, 0, 1), kStop});
    t.start(kKseg0 + kCode);
    CHECK_THROWS_WITH_AS(t.jit.execute(10'000), "rdp failed", std::runtime_error);
    for (int k = 0; k < 2; ++k) t.ref_cpu.step();
    CHECK_THROWS_WITH_AS(t.ref_cpu.step(), "rdp failed", std::runtime_error);
    CHECK(t.jit_cpu.pc() == t.ref_cpu.pc());  // the LWL faulted; nothing after it ran
    CHECK(state_of(t.ref_cpu) == state_of(t.jit_cpu));
    CHECK(t.jit_cpu.gpr(10) == 0);
    CHECK(t.jit_cpu.gpr(11) == 0);
    // The failure is gone: both machines continue identically.
    t.jit_bus.throw_at = t.ref_bus.throw_at = ~0U;
    t.run_until_pc(kKseg0 + kCode + 0x10);
    CHECK(t.same());
}

TEST_CASE("CPU JIT rethrows an error from an in-place store's code watch after retiring the store") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    // An in-place store (SWL) over a page holding compiled code reports it to the code watch;
    // here the watch throws. The interpreter lets it out of step() with the store done.
    const auto throwing = [](void*, std::uint32_t, std::uint32_t) {
        throw std::runtime_error("watch failed");
    };
    t.program(kCode, {addiu(8, 0, 1), lui(9, 0x8000), swl(8, 9, kCode + 0x21), addiu(11, 0, 1), kStop});
    t.start(kKseg0 + kCode);
    static std::uint8_t pages[0x100000 >> 12];
    pages[kCode >> 12] = 1;
    t.jit_bus.set_code_watch(pages, throwing, nullptr);
    t.ref_bus.set_code_watch(pages, throwing, nullptr);
    CHECK_THROWS_WITH_AS(t.jit.execute(10'000), "watch failed", std::runtime_error);
    for (int k = 0; k < 2; ++k) t.ref_cpu.step();
    CHECK_THROWS_WITH_AS(t.ref_cpu.step(), "watch failed", std::runtime_error);
    CHECK(state_of(t.ref_cpu) == state_of(t.jit_cpu));
    CHECK(t.ref_bus.mem == t.jit_bus.mem);
}

TEST_CASE("CPU JIT sees a store that rewrites the block it is running") {
    Twin t;
    t.lockstep = true;
    constexpr std::uint32_t kNew = 0x240A002A;  // addiu r10, r0, 42
    t.program(kCode, {lui(8, 0x8000), lui(9, kNew >> 16), ori(9, 9, kNew & 0xFFFF), sw(9, 8, kCode + 0x14), nop(),
                      addiu(10, 0, 1), kStop});
    t.start(kKseg0 + kCode);
    t.run_until_pc(kKseg0 + kCode + 0x18);
    CHECK(t.same());
    CHECK(t.jit_cpu.gpr(10) == 42);
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().invalidations > 0);
}

TEST_CASE("CPU JIT leaves a jump to an unaligned pc to the interpreter") {
    // JR to kBase + 0x41 after a block at kBase + 0x40 was compiled: the interpreter's next
    // fetch raises AdEL; no block may run (or be built) at the unaligned address.
    constexpr std::uint64_t kBase = kKseg0 + kCode;
    Twin t;
    t.lockstep = true;
    t.program(kCode, {j(0x02, kCode + 0x40), nop()});
    t.program(kCode + 0x40, {addiu(10, 10, 1), lui(8, 0x8000), ori(8, 8, kCode + 0x41), jr(8), addiu(9, 0, 1)});
    t.poke32(0x180, kStop);
    t.start(kBase);
    t.run_until_pc(kVector);
    CHECK(t.same());
    CHECK(((t.jit_cpu.cop0().cause() >> 2) & 31) == 4);  // AdEL
    CHECK(t.jit_cpu.cop0().read(Cop0::kBadVAddr) == kBase + 0x41);
    CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == kBase + 0x41);
    CHECK(t.jit_cpu.gpr(9) == 1);
    if (t.jit.available()) CHECK(t.jit_instructions > 0);
}

TEST_CASE("N64::set_cpu_mode refuses the JIT when it was not built") {
    using ezy::n64::CpuMode;
    const auto n64 = ezy::n64::N64::create(n64_test::counting_rom("CPU MODE TEST"));
    REQUIRE(n64);
    CHECK(n64->set_cpu_mode(CpuMode::Interpreter));
    CHECK(n64->cpu_mode() == CpuMode::Interpreter);
    CHECK(n64->cpu_jit() == nullptr);
    CHECK(n64->set_cpu_mode(CpuMode::Jit) == (EZY_JIT_AVAILABLE == 1));
    CHECK(n64->cpu_mode() == (EZY_JIT_AVAILABLE == 1 ? CpuMode::Jit : CpuMode::Interpreter));
    CHECK((n64->cpu_jit() != nullptr) == (EZY_JIT_AVAILABLE == 1));
    CHECK(n64->set_cpu_mode(CpuMode::Interpreter));
    CHECK(n64->cpu_jit() == nullptr);
}

TEST_CASE("an arbitrary resume inside a compiled block does not compile") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    t.program(kCode, {addiu(8, 8, 1), addiu(8, 8, 1), addiu(8, 8, 1), addiu(8, 8, 1), addiu(8, 8, 1), kStop});
    t.start(kKseg0 + kCode);
    CHECK(t.jit.execute(1000).instructions == 5);
    const auto before = t.jit.stats();
    CHECK(before.compiled == 1);
    t.jit_cpu.set_pc(kKseg0 + kCode + 8);  // the third instruction, not reached by a jump
    const auto r = t.jit.execute(1000);
    CHECK(r.cycles == 0);
    CHECK(r.instructions == 0);
    CHECK(t.jit.stats().compiled == before.compiled);
    CHECK(t.jit.stats().resume_skips == before.resume_skips + 1);
}

TEST_CASE("a branch target inside a block compiles") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    // The first block falls into the loop whose head is its third word.
    t.program(kCode, {addiu(8, 0, 0), addiu(9, 0, 0), addiu(8, 8, 1), addiu(9, 9, 2), addiu(10, 8, -5),
                      bne(10, 0, -4), nop(), kStop});
    t.start(kKseg0 + kCode);
    t.jit.execute(100000);
    CHECK(t.jit_cpu.pc() == kKseg0 + kCode + 28);
    CHECK(t.jit_cpu.gpr(8) == 5);
    CHECK(t.jit_cpu.gpr(9) == 10);
    const auto s = t.jit.stats();
    CHECK(s.compiled == 2);  // the entry block and the loop head
    CHECK(s.resume_skips == 0);
    CHECK(s.native > 20);
}

TEST_CASE("the resume rule keeps the interpreter's result") {
    std::uint64_t skips = 0;
    for (unsigned seed = 0; seed < 60; ++seed) {
        CAPTURE(seed);
        Twin t(seed);
        t.budget = 1 + seed % 17;  // small budgets stop blocks and force mid-block resumes
        t.run_until_pc(t.end);
        CHECK(t.same());
        skips += t.jit.stats().resume_skips;
    }
    if (Twin{}.jit.available()) CHECK(skips > 0);
}
