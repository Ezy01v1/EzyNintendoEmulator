// CPU JIT: LB LBU LH LHU LW LWU LD SB SH SW SD run as native code when they reach kseg0
// inside direct RAM, aligned; anything else (kseg1, MMIO, TLB, an unaligned address)
// leaves the block before the instruction, for the interpreter. The threaded RDP's inline
// barrier leaves too, exactly when the interpreter would have to wait.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "cpu_jit_twin.hpp"

using namespace cpu_jit_test;
using ezy::n64::Kind;
using ezy::n64::classify;

namespace {

constexpr std::uint64_t kBase = kKseg0 + kCode;

constexpr std::uint32_t lb(int rt, int base, std::int32_t off) { return i(0x20, base, rt, off); }
constexpr std::uint32_t lh(int rt, int base, std::int32_t off) { return i(0x21, base, rt, off); }
constexpr std::uint32_t lbu(int rt, int base, std::int32_t off) { return i(0x24, base, rt, off); }
constexpr std::uint32_t lhu(int rt, int base, std::int32_t off) { return i(0x25, base, rt, off); }
constexpr std::uint32_t lwu(int rt, int base, std::int32_t off) { return i(0x27, base, rt, off); }
constexpr std::uint32_t ld(int rt, int base, std::int32_t off) { return i(0x37, base, rt, off); }
constexpr std::uint32_t sb(int rt, int base, std::int32_t off) { return i(0x28, base, rt, off); }
constexpr std::uint32_t sh(int rt, int base, std::int32_t off) { return i(0x29, base, rt, off); }
constexpr std::uint32_t sd(int rt, int base, std::int32_t off) { return i(0x3F, base, rt, off); }
constexpr std::uint32_t ll(int rt, int base, std::int32_t off) { return i(0x30, base, rt, off); }
constexpr std::uint32_t sc(int rt, int base, std::int32_t off) { return i(0x38, base, rt, off); }
constexpr std::uint32_t lwc1(int ft, int base, std::int32_t off) { return i(0x31, base, ft, off); }

// r28 = the data zone (kseg0), 0x80040000.
std::vector<std::uint32_t> with_data_base(std::vector<std::uint32_t> body) {
    body.insert(body.begin(), lui(28, static_cast<std::int32_t>((kKseg0 + kData) >> 16)));
    body.push_back(kStop);
    return body;
}

void fill_data(Twin& t) {
    // 00 11 22 ... with the sign bit set in every other byte: 0x80 0x01 0x82 0x03 ...
    for (std::uint32_t a = 0; a < 0x40; ++a) t.poke8(kData + a, static_cast<std::uint8_t>((a & 1) ? a : 0x80 | a));
}

void run_to_stop(Twin& t, const std::vector<std::uint32_t>& w, std::uint32_t status = 0) {
    t.program(kCode, w);
    t.poke32(0x000, kStop);  // TLB refill vector
    t.poke32(0x180, kStop);
    t.start(kBase, status);
    t.reset_mmio_counts();
    const auto at_end = [&] {
        const std::uint64_t pc = t.jit_cpu.pc();
        return pc == kBase + 4 * (w.size() - 1) || pc == kVector || pc == kKseg0;
    };
    for (unsigned k = 0; k < 10000 && !at_end(); ++k) t.advance();
    REQUIRE(at_end());
}

}  // namespace

TEST_CASE("CPU JIT classifies the aligned loads and stores as native, the rest as memory") {
    for (const std::uint32_t w : {lb(8, 28, 0), lbu(8, 28, 0), lh(8, 28, 0), lhu(8, 28, 0), lw(8, 28, 0), sb(8, 28, 0),
                                  sh(8, 28, 0), sw(8, 28, 0)}) {
        CAPTURE(w);
        CHECK(classify(w, true) == Kind::Native);
        CHECK(classify(w, false) == Kind::Native);
    }
    for (const std::uint32_t w : {lwu(8, 28, 0), ld(8, 28, 0), sd(8, 28, 0)}) {  // 64-bit: RI without allow_64bit
        CAPTURE(w);
        CHECK(classify(w, true) == Kind::Native);
        CHECK(classify(w, false) == Kind::Memory);
    }
    for (const std::uint32_t w : {lwl(8, 28, 0), lwr(8, 28, 0), sdl(8, 28, 0), sdr(8, 28, 0), ll(8, 28, 0),
                                  sc(8, 28, 0), lwc1(0, 28, 0)}) {
        CAPTURE(w);
        CHECK(classify(w, true) == Kind::Memory);
    }
}

TEST_CASE("CPU JIT native kseg0 loads and stores match, including sign and zero extension") {
    Twin t;
    t.lockstep = true;
    fill_data(t);
    const std::vector<std::uint32_t> w = with_data_base({
        lb(1, 28, 0), lb(2, 28, 1), lbu(3, 28, 0), lbu(4, 28, 1),
        lh(5, 28, 0), lh(6, 28, 0x10), lhu(7, 28, 0), lhu(8, 28, 2),
        lw(9, 28, 0), lw(10, 28, 4), lwu(11, 28, 0), lwu(12, 28, 8),
        ld(13, 28, 0), ld(14, 28, 0x18),
        addiu(20, 28, 0x40), lw(15, 20, -0x3C),  // a negative offset
        addiu(23, 28, 0), lw(23, 23, 0x10),      // rt == base: the base is read first
        lw(0, 28, 0),                            // into $0: the access happens, $0 stays 0
        sb(1, 28, 0x20), sh(5, 28, 0x22), sw(9, 28, 0x24), sd(13, 28, 0x28),
        sb(0, 28, 0x30), sd(2, 28, 0x38),
        ld(17, 28, 0x20), ld(18, 28, 0x28), ld(19, 28, 0x38), lbu(21, 28, 0x30),
    });
    run_to_stop(t, w);
    CHECK(t.same());
    CHECK(t.jit_cpu.gpr(1) == 0xFFFFFFFFFFFFFF80ULL);  // LB 0x80
    CHECK(t.jit_cpu.gpr(2) == 0x01);
    CHECK(t.jit_cpu.gpr(3) == 0x80);                   // LBU
    CHECK(t.jit_cpu.gpr(5) == 0xFFFFFFFFFFFF8001ULL);  // LH 0x8001
    CHECK(t.jit_cpu.gpr(7) == 0x8001);                 // LHU
    CHECK(t.jit_cpu.gpr(9) == 0xFFFFFFFF80018203ULL);  // LW
    CHECK(t.jit_cpu.gpr(11) == 0x80018203ULL);         // LWU
    CHECK(t.jit_cpu.gpr(13) == 0x8001820384058607ULL); // LD
    CHECK(t.jit_cpu.gpr(15) == 0xFFFFFFFF84058607ULL); // 0x40 - 0x3C = 4
    CHECK(t.jit_cpu.gpr(23) == 0xFFFFFFFF90119213ULL); // rt == base
    CHECK(t.jit_cpu.gpr(0) == 0);
    CHECK(t.jit_cpu.gpr(17) == 0x8021800180018203ULL);  // SB SH SW stored back
    CHECK(t.jit_cpu.gpr(18) == 0x8001820384058607ULL);  // SD
    CHECK(t.jit_cpu.gpr(19) == 0x0000000000000001ULL);  // SD of r2
    CHECK(t.jit_cpu.gpr(21) == 0);                      // SB of $0
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().in_place == 0);
    CHECK(t.jit.stats().exits_before == 0);
    CHECK(t.jit.stats().native == t.jit_instructions);
}

TEST_CASE("CPU JIT matches on generated programs with native loads and stores in their delay slots") {
    for (unsigned seed = 100; seed < 116; ++seed) {
        CAPTURE(seed);
        Twin t(seed, Twin::Generated{.edge_values = true});
        t.lockstep = true;
        t.run_until_pc(t.end);
        CHECK(t.same());
    }
}

TEST_CASE("CPU JIT native loads leave kseg1, MMIO, TLB and unaligned addresses to the interpreter") {
    struct Case {
        const char* name;
        std::vector<std::uint32_t> words;  // r28 = the kseg0 data zone, r16 = the test address
        std::uint64_t address;
        bool raises;
    };
    const std::uint64_t kseg1_ram = 0xFFFFFFFFA0000000ULL + kData;
    const std::uint64_t kseg1_mmio = 0xFFFFFFFFA4400000ULL;
    const std::uint64_t kseg0_past_ram = kKseg0 + 0x100000;  // beyond direct RAM (MMIO)
    const std::uint64_t kuseg = 0x100;                        // TLB refill
    const std::uint64_t unextended = 0x0000000080040000ULL;  // not a sign-extended address: AdEL
    const Case cases[] = {
        {"LW kseg1 RAM", {lw(10, 16, 4)}, kseg1_ram, false},
        {"SW kseg1 RAM", {sw(28, 16, 4)}, kseg1_ram, false},
        {"LBU kseg1 MMIO", {lbu(10, 16, 0x13)}, kseg1_mmio, false},
        {"SH kseg1 MMIO", {sh(28, 16, 0x10)}, kseg1_mmio, false},
        {"LD kseg0 past direct RAM", {ld(10, 16, 8)}, kseg0_past_ram, false},
        {"SB kseg0 past direct RAM", {sb(28, 16, 1)}, kseg0_past_ram, false},
        {"LH kuseg (TLB)", {lh(10, 16, 0)}, kuseg, true},
        {"SD kuseg (TLB)", {sd(28, 16, 0)}, kuseg, true},
        {"LW unextended", {lw(10, 16, 0)}, unextended, true},
        {"LW unaligned", {lw(10, 28, 2)}, 0, true},
        {"LH unaligned", {lh(10, 28, 1)}, 0, true},
        {"LHU unaligned", {lhu(10, 28, 3)}, 0, true},
        {"LWU unaligned", {lwu(10, 28, 6)}, 0, true},
        {"LD unaligned", {ld(10, 28, 4)}, 0, true},
        {"SH unaligned", {sh(10, 28, 1)}, 0, true},
        {"SW unaligned", {sw(10, 28, 2)}, 0, true},
        {"SD unaligned", {sd(10, 28, 12)}, 0, true},
        {"LW last word of direct RAM", {lw(10, 16, -4)}, kseg0_past_ram, false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Twin t(TwinOptions{.mem = 0x200000, .direct = 0x100000, .penalty = 5});
        t.lockstep = true;
        fill_data(t);
        for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) cpu->set_gpr(16, c.address);
        std::vector<std::uint32_t> w = with_data_base({addiu(9, 0, 1)});
        w.insert(w.end() - 1, c.words.begin(), c.words.end());
        w.insert(w.end() - 1, addiu(11, 0, 1));
        run_to_stop(t, w);
        CHECK(t.same());
        CHECK((t.jit_cpu.pc() != kBase + 4 * (w.size() - 1)) == c.raises);
        CHECK(t.jit_cpu.gpr(11) == (c.raises ? 0 : 1));
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.stats().in_place == 0);
        const bool last_word = c.words[0] == lw(10, 16, -4);
        CHECK((t.jit.stats().exits_before > 0) == !last_word);
    }
}

TEST_CASE("CPU JIT native accesses call the RDP barrier exactly when the interpreter would wait") {
    // Page 0x40 has a writer packet (sequence 1), page 0x41 a reader packet (sequence 2),
    // page 0x42 nothing: a read waits on 0x40 only, a write on both. A wait completes the
    // packet, so later accesses to its page no longer wait.
    constexpr std::uint32_t kGuardEnd = kData + 0x3000;
    // Every access follows a taken branch (beq 0,0 skipping nothing but its slot), so it is a
    // jump target: after the interpreter ran a waiting access, the next one compiles and runs
    // natively again (a mid-block resume would be left to the interpreter).
    std::vector<std::uint32_t> body;
    for (const std::uint32_t access : {
             lw(8, 28, 0x1000),    // 0x41 read: no wait
             lw(9, 28, 0),         // 0x40 read: waits for 1
             sw(9, 28, 4),         // 0x40 write: 1 is done
             sh(8, 28, 0x1002),    // 0x41 write: waits for 2
             sb(8, 28, 0x2000),    // 0x42: no wait
             ld(10, 28, 8),        // 0x40: no wait
             lbu(11, 28, 0x1003),  // 0x41: no wait
         }) {
        body.insert(body.end(), {beq(0, 0, 1), nop(), access});
    }
    const std::vector<std::uint32_t> w = with_data_base(body);
    constexpr unsigned kAccesses = 7, kWaits = 2;
    const auto prepare = [](Twin& t) {
        for (TwinBus* bus : {&t.ref_bus, &t.jit_bus}) {
            bus->pend(kData, false, 1, 1);
            bus->pend(kData + 0x1000, true, 2, 2);
            bus->guard_begin = kData;
            bus->guard_end = kGuardEnd;
        }
    };
    SUBCASE("threaded: the inline check decides") {
        Twin t;
        t.lockstep = true;
        prepare(t);
        run_to_stop(t, w);
        CHECK(t.same());
        CHECK(t.ref_bus.waits == kWaits);
        CHECK(t.jit_bus.waits == kWaits);
        CHECK(t.ref_bus.guards == kAccesses);  // the interpreter asks on every access
        REQUIRE_NATIVE(t.jit);
        // Native code never calls it: its slow path leaves the block before the access, and
        // the interpreter's step() asks (and waits) for those accesses only.
        CHECK(t.jit_bus.guards == kWaits);
        CHECK(t.jit.stats().native == t.jit_instructions);
        // Each wait: the block leaves before the access, then the block entered at it leaves
        // again with no instruction run, and the harness steps the interpreter.
        CHECK(t.jit.stats().exits_before == 2 * kWaits);
    }
    SUBCASE("a failed renderer sends every access to the bus") {
        Twin t;
        t.lockstep = true;
        prepare(t);
        t.ref_bus.failed = t.jit_bus.failed = true;
        run_to_stop(t, w);
        CHECK(t.same());
        CHECK(t.ref_bus.guards == kAccesses);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit_bus.guards == kAccesses);  // the code reads wait too: only the data zone is counted
        // Each access is left to the interpreter: the first block leaves before the first one,
        // then every block entered at an access leaves with no instruction run.
        CHECK(t.jit.stats().exits_before == kAccesses + 1);
    }
    SUBCASE("without a view (synchronous RDP) native code never calls it") {
        Twin t;
        prepare(t);
        t.jit_bus.threaded = false;
        t.program(kCode, w);
        t.start(kBase);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.execute(1000).instructions == w.size() - 1);
        CHECK(t.jit_bus.guards == 0);
        CHECK(t.jit_bus.waits == 0);
    }
}

TEST_CASE("CPU JIT native store over compiled code leaves the block right after it") {
    constexpr std::uint32_t kNew = 0x256B0064;  // addiu r11, r11, 100
    SUBCASE("the store rewrites the next instruction of its own block") {
        Twin t;
        t.lockstep = true;
        const std::vector<std::uint32_t> w = {lui(8, 0x8000), lui(9, kNew >> 16), ori(9, 9, kNew & 0xFFFF),
                                              sw(9, 8, kCode + 0x10), addiu(11, 11, 1), addiu(12, 0, 1), kStop};
        run_to_stop(t, w);
        CHECK(t.same());
        CHECK(t.jit_cpu.gpr(11) == 100);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.stats().invalidations > 0);
        CHECK(t.jit.stats().in_place == 0);
        CHECK(t.jit.stats().exits_before == 0);
    }
    SUBCASE("a store in a branch delay slot drops the block it jumps to") {
        // 0x00-0x0C set up; 0x10: J 0x40 with SW over 0x40 in its slot; 0x40: the counted
        // loop back to 0x10. The second pass's store drops the block at 0x40 (compiled by
        // the first pass) and exits in the AfterSlot state, at 0x40.
        Twin t;
        t.lockstep = true;
        t.program(kCode, {addiu(20, 0, 3), lui(8, 0x8000), lui(9, kNew >> 16), ori(9, 9, kNew & 0xFFFF),
                          j(0x02, kCode + 0x40), sw(9, 8, kCode + 0x40)});
        t.program(kCode + 0x40, {addiu(11, 11, 1), addiu(20, 20, -1), bne(20, 0, -((0x48 + 4 - 0x10) / 4)), nop(),
                                 kStop});
        t.start(kBase);
        t.run_until_pc(kBase + 0x50);
        CHECK(t.same());
        CHECK(t.jit_cpu.gpr(11) == 300);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.stats().invalidations > 0);
        CHECK(t.jit.stats().in_place == 0);
    }
    SUBCASE("a store to a page without compiled code keeps running") {
        // (Blocks are dropped by page: any store to a page holding code leaves, as in place.)
        Twin t;
        const std::vector<std::uint32_t> w = {lui(8, 0x8004), sw(8, 8, 0x10), addiu(11, 0, 1), addiu(12, 0, 1), kStop};
        t.program(kCode, w);
        t.start(kBase);
        REQUIRE_NATIVE(t.jit);
        CHECK(t.jit.execute(1000).instructions == 4);
        CHECK(t.jit.stats().invalidations == 0);
    }
}
