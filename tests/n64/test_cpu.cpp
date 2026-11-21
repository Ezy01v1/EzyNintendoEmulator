#include <doctest/doctest.h>

#include <cstdint>

#include "mips_asm.hpp"

namespace {
struct GuardBus final : ezy::n64::CpuBus {
    FlatBus inner;
    mutable unsigned reads = 0, writes = 0, idle_reads = 0;
    bool change_idle_slot = false;
    GuardBus() { set_direct_ram(inner.mem.data(), FlatBus::kSize); set_memory_guard(true); }
    void prepare_memory(std::uint32_t address, std::size_t bytes, bool write) const override {
        if (write) ++writes; else ++reads;
        if (bytes == 8) {
            ++idle_reads;
            if (change_idle_slot) direct_ram()[address + 7] = 1;
        }
    }
    std::uint8_t read8(std::uint32_t a) override { return inner.read8(a); }
    std::uint16_t read16(std::uint32_t a) override { return inner.read16(a); }
    std::uint32_t read32(std::uint32_t a) override { return inner.read32(a); }
    std::uint64_t read64(std::uint32_t a) override { return inner.read64(a); }
    void write8(std::uint32_t a, std::uint8_t v) override { inner.write8(a, v); }
    void write16(std::uint32_t a, std::uint16_t v) override { inner.write16(a, v); }
    void write32(std::uint32_t a, std::uint32_t v) override { inner.write32(a, v); }
    void write64(std::uint32_t a, std::uint64_t v) override { inner.write64(a, v); }
    std::uint32_t uncached_cycles(std::uint32_t) const override { return 0; }
    bool interrupt_line() const override { return false; }
};
}

using namespace mips;

// Register numbers used below.
constexpr int t0 = 8, t1 = 9, t2 = 10, t3 = 11, ra = 31;

TEST_CASE("CPU direct fetch load store and idle detection obey memory guards") {
    GuardBus bus;
    ezy::n64::Vr4300 cpu(bus);
    cpu.cop0().write(ezy::n64::Cop0::kStatus, 0);
    cpu.set_pc(0xFFFFFFFF80001000ULL);
    bus.inner.write32(0x1000, lui(t0, 0x8000));
    bus.inner.write32(0x1004, addiu(t1, 0, 37));
    bus.inner.write32(0x1008, sw(t1, t0, 0x100));
    bus.inner.write32(0x100C, lw(t2, t0, 0x100));
    for (unsigned i = 0; i < 4; ++i) cpu.step();
    CHECK(cpu.gpr(t2) == 37);
    CHECK(bus.reads == 5);
    CHECK(bus.writes == 1);
    bus.inner.write32(0x2000, beq(0, 0, -1));
    bus.inner.write32(0x2004, nop());
    cpu.set_pc(0xFFFFFFFF80002000ULL);
    cpu.step(); cpu.step();
    CHECK(cpu.idle_loop_cycles() == 2);
    bus.change_idle_slot = true;
    CHECK(cpu.idle_loop_cycles() == 0);
    CHECK(bus.idle_reads == 2);
}

TEST_CASE("ADDIU and LUI build a 32-bit constant") {
    CpuFixture f;
    f.load({lui(t0, 0x1234), ori(t0, t0, 0x5678), addiu(t1, 0, -1)});
    f.run(3);
    CHECK(f.cpu.gpr(t0) == 0x12345678);
    CHECK(f.cpu.gpr(t1) == 0xFFFFFFFFFFFFFFFFULL);
}

TEST_CASE("32-bit ops sign-extend") {
    CpuFixture f;
    f.cpu.set_gpr(t0, 0x7FFFFFFF);
    f.cpu.set_gpr(t1, 1);
    f.load({addu(t2, t0, t1), lui(t3, 0x8000), sll(t1, t0, 1)});
    f.run(3);
    CHECK(f.cpu.gpr(t2) == 0xFFFFFFFF80000000ULL);
    CHECK(f.cpu.gpr(t3) == 0xFFFFFFFF80000000ULL);
    CHECK(f.cpu.gpr(t1) == 0xFFFFFFFFFFFFFFFEULL);
}

TEST_CASE("DADD and DSLL32 work on 64 bits") {
    CpuFixture f;
    f.cpu.set_gpr(t0, 0x00000001'FFFFFFFFULL);
    f.cpu.set_gpr(t1, 1);
    f.load({dadd(t2, t0, t1), dsll32(t3, t1, 4)});
    f.run(2);
    CHECK(f.cpu.gpr(t2) == 0x00000002'00000000ULL);
    CHECK(f.cpu.gpr(t3) == 0x00000010'00000000ULL);
}

TEST_CASE("branch delay slot runs before the jump") {
    CpuFixture f;
    // 0: beq r0,r0,+2 (to 12)   4: addiu t0,1 (slot)   8: addiu t1,1 (skipped)   12: addiu t2,1
    f.load({beq(0, 0, 2), addiu(t0, 0, 1), addiu(t1, 0, 1), addiu(t2, 0, 1)});
    f.run(3);
    CHECK(f.cpu.gpr(t0) == 1);
    CHECK(f.cpu.gpr(t1) == 0);
    CHECK(f.cpu.gpr(t2) == 1);
}

TEST_CASE("branch-likely not taken skips the delay slot") {
    CpuFixture f;
    f.cpu.set_gpr(t3, 5);
    f.load({beql(0, t3, 5), addiu(t0, 0, 1), addiu(t1, 0, 1)});
    f.run(2);
    CHECK(f.cpu.gpr(t0) == 0);
    CHECK(f.cpu.gpr(t1) == 1);
}

TEST_CASE("JAL links PC + 8") {
    CpuFixture f;
    f.load({jal(0x2000), nop()});
    f.run(2);
    CHECK(f.cpu.gpr(ra) == CpuFixture::kEntry + 8);
    CHECK(f.cpu.pc() == 0xFFFFFFFF80002000ULL);
}

TEST_CASE("LWL and LWR assemble an unaligned word") {
    CpuFixture f;
    for (std::uint32_t k = 0; k < 8; ++k) f.bus.write8(0x3000 + k, static_cast<std::uint8_t>(0x11 * (k + 1)));
    f.cpu.set_gpr(t0, 0xFFFFFFFF80003000ULL);
    // Load the word at 0x3001 (22 33 44 55) the usual way: LWL at +1, LWR at +4.
    f.load({lwl(t1, t0, 1), lwr(t1, t0, 4)});
    f.run(2);
    CHECK(f.cpu.gpr(t1) == 0x22334455);
}

TEST_CASE("SDL and SDR store an unaligned doubleword") {
    CpuFixture f;
    f.cpu.set_gpr(t0, 0xFFFFFFFF80003000ULL);
    f.cpu.set_gpr(t1, 0x0102030405060708ULL);
    f.load({sdl(t1, t0, 3), sdr(t1, t0, 10)});  // the doubleword at 0x3003
    f.run(2);
    for (std::uint32_t k = 0; k < 8; ++k) CHECK(f.bus.read8(0x3003 + k) == k + 1);
    CHECK(f.bus.read8(0x3002) == 0);
    CHECK(f.bus.read8(0x300B) == 0);
}

TEST_CASE("DIV by zero gives the hardware results") {
    CpuFixture f;
    f.cpu.set_gpr(t0, 7);
    f.cpu.set_gpr(t1, 0xFFFFFFFFFFFFFFF9ULL);  // -7
    f.load({mips::div(t0, 0), mflo(t2), mfhi(t3), mips::div(t1, 0), mflo(t0), divu(t1, 0), mflo(t1)});
    f.run(7);
    CHECK(f.cpu.gpr(t2) == 0xFFFFFFFFFFFFFFFFULL);  // rs >= 0: LO = -1
    CHECK(f.cpu.gpr(t3) == 7);                       // HI = rs
    CHECK(f.cpu.gpr(t0) == 1);                       // rs < 0: LO = 1
    CHECK(f.cpu.gpr(t1) == 0xFFFFFFFFFFFFFFFFULL);  // DIVU: LO = all ones
}

TEST_CASE("MULT costs 5 extra cycles") {
    CpuFixture f;
    f.cpu.set_gpr(t0, 0xFFFFFFFFFFFFFFFEULL);  // -2
    f.cpu.set_gpr(t1, 3);
    f.load({mult(t0, t1), nop()});
    CHECK(f.cpu.step() == 1 + 5);
    CHECK(f.cpu.lo() == 0xFFFFFFFFFFFFFFFAULL);
    CHECK(f.cpu.hi() == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(f.cpu.step() == 1);
}

TEST_CASE("r0 stays zero") {
    CpuFixture f;
    f.load({addiu(0, 0, 5), lui(0, 0x1234)});
    f.run(2);
    CHECK(f.cpu.gpr(0) == 0);
}

TEST_CASE("uncached fetch pays the bus penalty") {
    CpuFixture f;
    f.bus.uncached_penalty = 10;
    f.load({nop()});
    f.cpu.set_pc(0xFFFFFFFFA0000000ULL + CpuFixture::kProgram);  // kseg1
    CHECK(f.cpu.step() == 1 + 10);
}
