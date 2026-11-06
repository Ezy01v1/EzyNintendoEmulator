#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>

#include "flat_bus.hpp"
#include "gb/cpu.hpp"

using namespace ezy::gb;
using ezy::gb::test::FlatBus;

namespace {

// A CPU at PC = 0x0100 with `program` loaded there.
struct Fixture {
    FlatBus bus;
    Cpu cpu{bus};

    explicit Fixture(std::initializer_list<std::uint8_t> program) {
        cpu.regs().pc = 0x0100;
        cpu.regs().sp = 0xFFFE;
        bus.load(0x0100, program);
    }

    // Runs one instruction and returns how many M-cycles it took.
    int step() {
        const int before = bus.m_cycles;
        cpu.step();
        return bus.m_cycles - before;
    }
};

}  // namespace

TEST_CASE("post-boot register state") {
    FlatBus bus;
    Cpu cpu(bus);
    cpu.reset_post_boot();
    CHECK(cpu.regs().af() == 0x01B0);
    CHECK(cpu.regs().bc() == 0x0013);
    CHECK(cpu.regs().de() == 0x00D8);
    CHECK(cpu.regs().hl() == 0x014D);
    CHECK(cpu.regs().sp == 0xFFFE);
    CHECK(cpu.regs().pc == 0x0100);
}

TEST_CASE("LD r, n and LD r, r'") {
    Fixture f({0x06, 0x42, 0x48});  // LD B, 42 ; LD C, B
    CHECK(f.step() == 2);
    CHECK(f.step() == 1);
    CHECK(f.cpu.regs().c == 0x42);
}

TEST_CASE("LD (HL+), A writes and increments HL") {
    Fixture f({0x21, 0x00, 0xC0, 0x3E, 0x99, 0x22});  // LD HL, C000 ; LD A, 99 ; LD (HL+), A
    f.step();
    f.step();
    CHECK(f.step() == 2);
    CHECK(f.bus.memory[0xC000] == 0x99);
    CHECK(f.cpu.regs().hl() == 0xC001);
}

TEST_CASE("ADD sets Z, H and C") {
    Fixture f({0x3E, 0x8F, 0xC6, 0x71});  // LD A, 8F ; ADD A, 71 -> 0x100
    f.step();
    f.step();
    CHECK(f.cpu.regs().a == 0x00);
    CHECK(f.cpu.regs().f == (flag::kZ | flag::kH | flag::kC));
}

TEST_CASE("SUB / CP set N and borrow flags") {
    Fixture f({0x3E, 0x10, 0xFE, 0x01});  // LD A, 10 ; CP 01
    f.step();
    f.step();
    CHECK(f.cpu.regs().a == 0x10);  // CP does not store the result
    CHECK(f.cpu.regs().f == (flag::kN | flag::kH));
}

TEST_CASE("INC r preserves carry") {
    Fixture f({0x37, 0x3E, 0x0F, 0x3C});  // SCF ; LD A, 0F ; INC A
    f.step();
    f.step();
    f.step();
    CHECK(f.cpu.regs().a == 0x10);
    CHECK(f.cpu.regs().f == (flag::kH | flag::kC));
}

TEST_CASE("DAA corrects a BCD addition") {
    Fixture f({0x3E, 0x45, 0xC6, 0x38, 0x27});  // LD A, 45 ; ADD A, 38 ; DAA -> 83
    f.step();
    f.step();
    f.step();
    CHECK(f.cpu.regs().a == 0x83);
    CHECK_FALSE(f.cpu.regs().flag(flag::kC));
}

TEST_CASE("PUSH / POP AF masks the low nibble of F") {
    Fixture f({0x01, 0xFF, 0x12, 0xC5, 0xF1});  // LD BC, 12FF ; PUSH BC ; POP AF
    f.step();
    CHECK(f.step() == 4);
    CHECK(f.step() == 3);
    CHECK(f.cpu.regs().af() == 0x12F0);
}

TEST_CASE("JR timing depends on whether the branch is taken") {
    Fixture f({0xAF, 0x20, 0x05, 0x28, 0x05});  // XOR A (Z=1) ; JR NZ,+5 ; JR Z,+5
    f.step();
    CHECK(f.step() == 2);  // not taken
    CHECK(f.step() == 3);  // taken
    CHECK(f.cpu.regs().pc == 0x0105 + 5);
}

TEST_CASE("CALL and RET") {
    Fixture f({0xCD, 0x00, 0x02});  // CALL 0200
    f.bus.load(0x0200, {0xC9});     // RET
    CHECK(f.step() == 6);
    CHECK(f.cpu.regs().pc == 0x0200);
    CHECK(f.cpu.regs().sp == 0xFFFC);
    CHECK(f.step() == 4);
    CHECK(f.cpu.regs().pc == 0x0103);
}

TEST_CASE("ADD SP, e uses unsigned low-byte flags") {
    Fixture f({0x31, 0xFF, 0x00, 0xE8, 0x01});  // LD SP, 00FF ; ADD SP, +1
    f.step();
    CHECK(f.step() == 4);
    CHECK(f.cpu.regs().sp == 0x0100);
    CHECK(f.cpu.regs().f == (flag::kH | flag::kC));
}

TEST_CASE("CB: BIT, SET, SWAP on (HL)") {
    Fixture f({0x21, 0x00, 0xC0, 0xCB, 0xFE, 0xCB, 0x7E, 0xCB, 0x36});  // LD HL,C000 ; SET 7,(HL) ; BIT 7,(HL) ; SWAP (HL)
    f.step();
    CHECK(f.step() == 4);  // read-modify-write
    CHECK(f.step() == 3);  // BIT only reads
    CHECK_FALSE(f.cpu.regs().flag(flag::kZ));
    f.step();
    CHECK(f.bus.memory[0xC000] == 0x08);
}

TEST_CASE("interrupt dispatch pushes PC and jumps to the vector") {
    Fixture f({0x00});
    f.cpu.set_ime(true);
    f.bus.ie = 0x1F;
    f.bus.if_ = 0x04 | 0x10;  // timer + joypad: timer has priority
    CHECK(f.step() == 5);
    CHECK(f.cpu.regs().pc == 0x0050);
    CHECK_FALSE(f.cpu.ime());
    CHECK(f.bus.if_ == 0x10);
    CHECK(f.bus.memory[0xFFFD] == 0x01);  // return address 0x0100
    CHECK(f.bus.memory[0xFFFC] == 0x00);
}

TEST_CASE("EI takes effect after the following instruction") {
    Fixture f({0xFB, 0x00, 0x00});  // EI ; NOP ; NOP
    f.bus.ie = 0x01;
    f.bus.if_ = 0x01;
    f.step();  // EI
    CHECK_FALSE(f.cpu.ime());
    f.step();  // NOP runs, then IME turns on
    CHECK(f.cpu.ime());
    f.step();  // interrupt is serviced instead of the second NOP
    CHECK(f.cpu.regs().pc == 0x0040);
}

TEST_CASE("HALT waits until an interrupt is pending") {
    Fixture f({0x76, 0x3C});  // HALT ; INC A
    f.bus.ie = 0x01;
    f.step();
    CHECK(f.cpu.halted());
    f.step();
    CHECK(f.cpu.halted());
    f.bus.if_ = 0x01;  // IME is off: wake up without servicing
    f.step();
    CHECK_FALSE(f.cpu.halted());
    CHECK(f.cpu.regs().a == 1);
}

TEST_CASE("illegal opcodes lock the CPU") {
    Fixture f({0xD3});
    f.step();
    CHECK(f.cpu.locked());
}

TEST_CASE("ie_push: the push of PC's high byte can cancel the interrupt") {
    Fixture f({0x00});
    f.cpu.set_ime(true);
    f.cpu.regs().sp = 0x0000;  // the high byte of PC is pushed to 0xFFFF = IE
    f.bus.ie = 0x01;
    f.bus.if_ = 0x01;
    CHECK(f.step() == 5);
    // PC = 0x0100: its high byte 0x01 lands in IE, so VBlank stays enabled.
    CHECK(f.cpu.regs().pc == 0x0040);

    Fixture g({0x00});
    g.cpu.regs().pc = 0x0000;
    g.bus.memory[0x0000] = 0x00;
    g.cpu.set_ime(true);
    g.cpu.regs().sp = 0x0000;
    g.bus.ie = 0x01;
    g.bus.if_ = 0x01;
    g.step();
    // PC's high byte 0x00 overwrites IE: nothing is pending any more, so the CPU jumps
    // to 0x0000 and IF keeps its bit.
    CHECK(g.cpu.regs().pc == 0x0000);
    CHECK(g.bus.if_ == 0x01);
}
