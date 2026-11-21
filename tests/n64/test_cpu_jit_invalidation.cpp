// CPU JIT: compiled code stays in step with RAM. Every RDRAM writer reports what it writes
// through CpuBus::note_ram_write; the check mode catches one that does not.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "cpu_jit_twin.hpp"
#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/scheduler.hpp"

using namespace cpu_jit_test;

namespace {

constexpr int t0 = 8, t1 = 9, t2 = 10, t3 = 11, ra = 31;

void put32(TwinBus& bus, std::uint32_t paddr, std::uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bus.at(paddr + i) = static_cast<std::uint8_t>(word >> (24 - 8 * i));
}

}  // namespace

TEST_CASE("CPU JIT runs the new instruction after a store rewrites the next one in the same block") {
    // One block: nop; sw t1 -> 0x1008 (writes "addiu t2, 0, 99"); 0x1008 was "addiu t2, 0, 1".
    Twin t;
    t.lockstep = true;
    t.program(kCode, {nop(), sw(t1, t0, 8), addiu(t2, 0, 1), kStop});
    t.start(kKseg0 + kCode);
    for (Vr4300* cpu : {&t.ref_cpu, &t.jit_cpu}) {
        cpu->set_gpr(t0, kKseg0 + kCode);
        cpu->set_gpr(t1, addiu(t2, 0, 99));
    }
    t.run_until_pc(kKseg0 + kCode + 0xC);
    CHECK(t.jit_cpu.gpr(t2) == 99);
    CHECK(t.same());
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().invalidations >= 1);
}

TEST_CASE("CPU JIT recompiles code overwritten by another block's store") {
    // The caller (page 1) calls a routine in page 2 twice and rewrites its first word in
    // between, so the second call must run the new word: t2 = 99, not 1. The MFC0 after the
    // first call ends a block, so a run can stop there.
    constexpr std::uint32_t kRoutine = kCode + 0x1100;
    Twin t;
    t.lockstep = true;
    const std::uint32_t replacement = addiu(t2, 0, 99);
    t.program(kCode, {lui(t0, 0x8000), lui(t1, static_cast<std::int32_t>(replacement >> 16)),
                      ori(t1, t1, static_cast<std::int32_t>(replacement & 0xFFFF)), jal(kRoutine), nop(),
                      mfc0(t3, 12), sw(t1, t0, static_cast<std::int32_t>(kRoutine)), jal(kRoutine), nop(), kStop});
    t.program(kRoutine, {addiu(t2, 0, 1), jr(ra), nop()});
    t.start(kKseg0 + kCode);
    t.run_until_pc(kKseg0 + kCode + 0x14);  // the first call is done
    CHECK(t.jit_cpu.gpr(t2) == 1);

    SUBCASE("a store inside a block") {}
    SUBCASE("a store the interpreter runs") {
        t.advance();  // the MFC0, with step()
        REQUIRE(t.jit_cpu.pc() == kKseg0 + kCode + 0x18);
        t.budget = 1;  // no block fits: Twin::advance runs the store with step()
        t.advance();
        REQUIRE(t.jit_cpu.pc() == kKseg0 + kCode + 0x1C);
        t.budget = 1000;
    }
    t.run_until_pc(kKseg0 + kCode + 0x24);
    CHECK(t.jit_cpu.gpr(t2) == 99);
    CHECK(t.same());
    REQUIRE_NATIVE(t.jit);
    CHECK(t.jit.stats().invalidations >= 1);
}

TEST_CASE("CPU JIT check mode reports code changed behind its back") {
    Twin t;
    REQUIRE_NATIVE(t.jit);
    t.program(kCode, {addiu(t2, 0, 1), addiu(t3, 0, 2), kStop});
    t.start(kKseg0 + kCode);
    t.jit.set_check(true);
    CHECK(t.jit.execute(1000).instructions == 2);  // code that matches RAM passes
    CHECK(t.jit_cpu.gpr(t3) == 2);

    // Writes straight into the bus memory, without note_ram_write: what a writer that
    // forgot to report would leave behind.
    t.jit_cpu.set_pc(kKseg0 + kCode);
    put32(t.jit_bus, kCode + 4, addiu(t3, 0, 3));
    CHECK_THROWS_WITH_AS(t.jit.execute(1000), doctest::Contains("0x00001000"), std::runtime_error);
    CHECK(t.jit_cpu.gpr(t3) == 2);  // the stale block did not run
    CHECK(t.jit_cpu.pc() == kKseg0 + kCode);

    // Reported, the same write recompiles the block and the check passes again.
    t.jit_bus.note_ram_write(kCode + 4, 4);
    CHECK(t.jit.execute(1000).instructions == 2);
    CHECK(t.jit_cpu.gpr(t3) == 3);
}

TEST_CASE("CPU JIT on the N64 bus drops the blocks a DMA overwrites") {
    std::vector<std::uint8_t> rom(0x100000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    const auto cart = *ezy::n64::Cartridge::load(rom);
    ezy::n64::Scheduler scheduler;
    ezy::n64::Bus bus(scheduler, cart);
    Vr4300 cpu(bus);
    auto jit = std::make_unique<Vr4300Jit>(cpu, bus);
    CHECK(bus.code_watch_user() == jit.get());
    REQUIRE_NATIVE(*jit);

    std::uint32_t at = kCode;
    for (const std::uint32_t w : {addiu(t2, 0, 1), addiu(t3, 0, 2), kStop}) {
        bus.write32(at, w);
        at += 4;
    }
    cpu.cop0().write(Cop0::kStatus, 0);
    cpu.set_pc(kKseg0 + kCode);
    CHECK(jit->execute(1000).instructions == 2);
    CHECK(jit->stats().invalidations == 0);

    bus.write32(0x04600000, kCode);      // PI_DRAM_ADDR
    bus.write32(0x04600004, 0x10001000); // PI_CART_ADDR: zeros
    bus.write32(0x0460000C, 7);          // PI_WR_LEN: 8 bytes, over the block
    CHECK(jit->stats().invalidations == 1);

    jit.reset();
    CHECK(bus.code_watch_user() == nullptr);
    bus.write32(kCode, 0);  // nothing watches any more
}
