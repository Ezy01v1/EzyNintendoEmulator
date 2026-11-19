#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

#include "common/state.hpp"
#include "mips_asm.hpp"
#include "n64/cpu/vr4300.hpp"
#include "n64/cpu/vr4300_jit.hpp"

using ezy::n64::Cop0;
using ezy::n64::Vr4300;
using ezy::n64::Vr4300Jit;
using namespace mips;

namespace {

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

std::vector<std::uint8_t> state_of(Vr4300& cpu) {
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
    explicit Twin(unsigned seed, std::size_t code_budget = kDefaultCode);

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

// Random integer programs: 32/64-bit ALU, LUI, MULT/DIV and family, forward branches
// (all kinds, with their delay slots) and aligned loads/stores to a kseg0 data zone,
// wrapped in a 3-iteration loop so cached blocks run again; it ends at `end`.
class Generator {
public:
    explicit Generator(unsigned seed) : rng_(seed) {}

    std::uint32_t pick(std::uint32_t n) { return static_cast<std::uint32_t>(rng_() % n); }
    int reg() { return static_cast<int>(1 + pick(22)); }       // r1..r22
    int dst() { return pick(16) == 0 ? 0 : reg(); }              // sometimes r0

    std::uint32_t alu() {
        const int d = dst(), s = reg(), t = reg();
        const int sa = static_cast<int>(pick(32));
        const std::int32_t imm = static_cast<std::int32_t>(pick(0x10000)) - 0x8000;
        switch (pick(40)) {
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
};

Twin::Twin(unsigned seed, std::size_t code_budget) : Twin(TwinOptions{.code_budget = code_budget}) {
    Generator g(seed);
    budget = 1 + g.pick(300);
    for (std::uint32_t a = 0; a < kDataBytes; ++a) poke8(kData + a, static_cast<std::uint8_t>(g.pick(256)));
    std::vector<std::uint32_t> words;
    words.push_back(lui(28, static_cast<std::int32_t>((kKseg0 + kData) >> 16)));  // data base
    words.push_back(addiu(23, 0, 3));                                          // loop counter
    for (int r = 1; r <= 22; ++r) {
        words.push_back(lui(r, static_cast<std::int32_t>(g.pick(0x10000))));
        words.push_back(ori(r, r, static_cast<std::int32_t>(g.pick(0x10000))));
        if (g.pick(3) == 0) words.push_back(dsll32(r, r, static_cast<int>(g.pick(32))));
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

#define REQUIRE_NATIVE(jit)                              \
    if (!(jit).available()) {                            \
        MESSAGE("CPU JIT not available in this build");  \
        return;                                          \
    }

}  // namespace

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
    const auto rest = jit.execute(1000);
    CHECK(rest.instructions == 8);
    CHECK(rest.cycles == 8);
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
    CHECK(t.jit.stats().in_place > 0);
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
    Twin t(7, /*code_budget=*/64 * 1024);
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

TEST_CASE("CPU JIT rethrows a host error raised inside a block") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    t.jit_bus.throw_at = kData + 0x10;
    t.program(kCode, {addiu(8, 0, 1), lui(9, 0x8004), lw(10, 9, 0x10), addiu(11, 0, 1), kStop});
    t.start(kKseg0 + kCode);
    CHECK_THROWS_AS(t.jit.execute(1000), std::runtime_error);
    CHECK(t.jit_cpu.gpr(11) == 0);
    t.jit_bus.throw_at = ~0U;
    t.jit.flush();
    CHECK(t.jit.execute(1000).instructions > 0);
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
