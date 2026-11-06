#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "gba/cpu/arm7tdmi.hpp"
#include "gba/cpu/shifter.hpp"

using namespace ezy::gba;

namespace {

// 64 KiB of flat little-endian memory at address 0.
class FlatBus final : public CpuBus {
public:
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x10000, 0);

    std::uint8_t read8(std::uint32_t a, Access) override { return mem[a & 0xFFFF]; }
    std::uint16_t read16(std::uint32_t a, Access) override { return static_cast<std::uint16_t>(get(a & ~1U, 2)); }
    std::uint32_t read32(std::uint32_t a, Access) override { return get(a & ~3U, 4); }
    void write8(std::uint32_t a, std::uint8_t v, Access) override { mem[a & 0xFFFF] = v; }
    void write16(std::uint32_t a, std::uint16_t v, Access) override { put(a & ~1U, v, 2); }
    void write32(std::uint32_t a, std::uint32_t v, Access) override { put(a & ~3U, v, 4); }

    void program(std::initializer_list<std::uint32_t> ops, std::uint32_t at = 0) {
        for (const auto op : ops) {
            put(at, op, 4);
            at += 4;
        }
    }

private:
    std::uint32_t get(std::uint32_t a, int n) const {
        std::uint32_t v = 0;
        std::memcpy(&v, mem.data() + (a & 0xFFFF), static_cast<std::size_t>(n));
        return v;
    }
    void put(std::uint32_t a, std::uint32_t v, int n) { std::memcpy(mem.data() + (a & 0xFFFF), &v, static_cast<std::size_t>(n)); }
};

struct Fixture {
    FlatBus bus;
    Arm7tdmi cpu{bus};
    void run(int steps) {
        for (int i = 0; i < steps; ++i) cpu.step();
    }
};

}  // namespace

TEST_CASE("shifter immediate edge cases") {
    bool c = false;
    CHECK(shifter::by_imm(shifter::Type::Lsl, 0x80000001, 0, c) == 0x80000001);  // LSL #0: unchanged
    CHECK_FALSE(c);
    CHECK(shifter::by_imm(shifter::Type::Lsr, 0x80000000, 0, c) == 0);  // LSR #0 = LSR #32
    CHECK(c);
    CHECK(shifter::by_imm(shifter::Type::Asr, 0x80000000, 0, c) == 0xFFFFFFFF);  // ASR #32
    CHECK(c);
    c = true;
    CHECK(shifter::by_imm(shifter::Type::Ror, 0x00000002, 0, c) == 0x80000001);  // RRX
    CHECK_FALSE(c);
}

TEST_CASE("shifter register edge cases") {
    bool c = true;
    CHECK(shifter::by_reg(shifter::Type::Lsl, 0x12345678, 0, c) == 0x12345678);  // amount 0: C kept
    CHECK(c);
    CHECK(shifter::by_reg(shifter::Type::Lsl, 0x00000001, 32, c) == 0);
    CHECK(c);  // bit 0
    CHECK(shifter::by_reg(shifter::Type::Lsl, 0xFFFFFFFF, 33, c) == 0);
    CHECK_FALSE(c);
    CHECK(shifter::by_reg(shifter::Type::Ror, 0x80000000, 32, c) == 0x80000000);
    CHECK(c);  // bit 31
    CHECK(shifter::by_reg(shifter::Type::Lsr, 0x12345678, 0x104, c) == 0x01234567);  // only the low byte
}

TEST_CASE("ARM reset starts in Supervisor at 0 and reads PC as +8") {
    Fixture f;
    f.bus.program({0xE1A0000F});  // MOV r0, pc
    f.cpu.reset();
    CHECK(f.cpu.mode() == Mode::Supervisor);
    CHECK((f.cpu.cpsr() & (psr::kI | psr::kF)) == (psr::kI | psr::kF));
    f.run(1);
    CHECK(f.cpu.reg(0) == 8);
}

TEST_CASE("ARM data processing sets flags") {
    Fixture f;
    f.bus.program({
        0xE3E00000,  // MVN r0, #0        -> 0xFFFFFFFF
        0xE2901001,  // ADDS r1, r0, #1   -> 0, Z C
        0xE3A02102,  // MOV r2, #0x80000000
        0xE2523001,  // SUBS r3, r2, #1   -> 0x7FFFFFFF, V C
    });
    f.cpu.reset();
    f.run(2);
    CHECK(f.cpu.reg(1) == 0);
    CHECK((f.cpu.cpsr() & (psr::kZ | psr::kC)) == (psr::kZ | psr::kC));
    f.run(2);
    CHECK(f.cpu.reg(3) == 0x7FFFFFFF);
    CHECK((f.cpu.cpsr() & psr::kV) != 0);
    CHECK((f.cpu.cpsr() & psr::kC) != 0);  // no borrow
}

TEST_CASE("ARM conditions skip instructions") {
    Fixture f;
    f.bus.program({
        0xE3B00000,  // MOVS r0, #0  (Z=1)
        0x13A01001,  // MOVNE r1, #1 (skipped)
        0x03A02002,  // MOVEQ r2, #2
    });
    f.cpu.reset();
    f.run(3);
    CHECK(f.cpu.reg(1) == 0);
    CHECK(f.cpu.reg(2) == 2);
}

TEST_CASE("ARM BL and BX return") {
    Fixture f;
    f.bus.program({
        0xEB000001,  // 0x00: BL 0x0C
        0xE3A00001,  // 0x04: MOV r0, #1  (after return)
        0xEAFFFFFE,  // 0x08: B . (stay)
        0xE3A01007,  // 0x0C: MOV r1, #7
        0xE12FFF1E,  // 0x10: BX lr
    });
    f.cpu.reset();
    f.run(1);
    CHECK(f.cpu.reg(14) == 0x04);
    f.run(3);  // MOV r1, BX lr, MOV r0
    CHECK(f.cpu.reg(1) == 7);
    CHECK(f.cpu.reg(0) == 1);
}

TEST_CASE("ARM mode switch banks r13/r14 and restores them") {
    Fixture f;
    f.bus.program({
        0xE3A0D001,  // MOV sp, #1        (SVC sp)
        0xE321F012,  // MSR CPSR_c, #0x12 (IRQ)
        0xE3A0D002,  // MOV sp, #2        (IRQ sp)
        0xE321F013,  // MSR CPSR_c, #0x13 (back to SVC)
    });
    f.cpu.reset();
    f.run(3);
    CHECK(f.cpu.mode() == Mode::Irq);
    CHECK(f.cpu.reg(13) == 2);
    f.run(1);
    CHECK(f.cpu.mode() == Mode::Supervisor);
    CHECK(f.cpu.reg(13) == 1);
    CHECK(f.cpu.banked(kBankIrq, 5) == 2);
}

TEST_CASE("ARM LDR/STR with writeback and misaligned rotation") {
    Fixture f;
    f.bus.program({
        0xE3A01C01,  // MOV r1, #0x100
        0xE59F2008,  // LDR r2, [pc, #8]  -> 0xAABBCCDD
        0xE4812004,  // STR r2, [r1], #4  -> r1 = 0x104
        0xE5113003,  // LDR r3, [r1, #-3] -> 0x101: rotated
        0xEAFFFFFE,  // B .
        0xAABBCCDD,
    });
    f.cpu.reset();
    f.run(4);
    CHECK(f.cpu.reg(1) == 0x104);
    CHECK(f.bus.read32(0x100, kNonseq) == 0xAABBCCDD);
    CHECK(f.cpu.reg(3) == 0xDDAABBCC);
}

TEST_CASE("ARM STMFD/LDMFD push and pop") {
    Fixture f;
    f.bus.program({
        0xE3A0DC02,  // MOV sp, #0x200
        0xE3A00011,  // MOV r0, #0x11
        0xE3A01022,  // MOV r1, #0x22
        0xE92D0003,  // STMFD sp!, {r0, r1}
        0xE3A00000,  // MOV r0, #0
        0xE3A01000,  // MOV r1, #0
        0xE8BD0003,  // LDMFD sp!, {r0, r1}
    });
    f.cpu.reset();
    f.run(4);
    CHECK(f.cpu.reg(13) == 0x1F8);
    CHECK(f.bus.read32(0x1F8, kNonseq) == 0x11);
    CHECK(f.bus.read32(0x1FC, kNonseq) == 0x22);
    f.run(3);
    CHECK(f.cpu.reg(0) == 0x11);
    CHECK(f.cpu.reg(1) == 0x22);
    CHECK(f.cpu.reg(13) == 0x200);
}

TEST_CASE("ARM SWI enters Supervisor at 0x08 and MOVS pc, lr returns") {
    Fixture f;
    f.bus.program({0xEA000001}, 0x00);             // reset: B 0x0C
    f.bus.program({0xE1B0F00E}, 0x08);             // SWI vector: MOVS pc, lr
    f.bus.program({0xE321F01F, 0xEF000000, 0xE3A00005}, 0x0C);  // MSR to System, SWI, MOV r0, #5
    f.cpu.reset();
    f.run(3);  // B, MSR, SWI
    CHECK(f.cpu.mode() == Mode::Supervisor);
    CHECK(f.cpu.reg(14) == 0x14);
    CHECK(f.cpu.spsr(kBankSvc) == 0x1F);  // MSR CPSR_c, #0x1F also cleared I and F
    f.run(2);  // MOVS pc, lr ; MOV r0, #5
    CHECK(f.cpu.mode() == Mode::System);
    CHECK(f.cpu.reg(0) == 5);
}

TEST_CASE("ARM UMULL and SMLAL") {
    Fixture f;
    f.bus.program({
        0xE3E00000,  // MVN r0, #0         (0xFFFFFFFF)
        0xE3A01002,  // MOV r1, #2
        0xE0832190,  // UMULL r2, r3, r0, r1 -> 0x1_FFFFFFFE
        0xE0E54190,  // SMLAL r4, r5, r0, r1 -> -2
    });
    f.cpu.reset();
    f.run(4);
    CHECK(f.cpu.reg(2) == 0xFFFFFFFE);
    CHECK(f.cpu.reg(3) == 1);
    CHECK(f.cpu.reg(4) == 0xFFFFFFFE);
    CHECK(f.cpu.reg(5) == 0xFFFFFFFF);
}

// --- Thumb ---

namespace {

void program16(FlatBus& bus, std::initializer_list<std::uint16_t> ops, std::uint32_t at) {
    for (const auto op : ops) {
        bus.write16(at, op, kNonseq);
        at += 2;
    }
}

}  // namespace

TEST_CASE("BX switches ARM -> Thumb -> ARM, and Thumb reads PC as +4") {
    Fixture f;
    f.bus.program({
        0xE3A00C01,  // 0x00: MOV r0, #0x100
        0xE2800001,  // 0x04: ADD r0, r0, #1  (Thumb bit)
        0xE12FFF10,  // 0x08: BX r0
    });
    program16(f.bus, {0x4679, 0x2000, 0x4778, 0x46C0}, 0x100);  // MOV r1, pc; MOV r0, #0; BX pc; NOP
    f.bus.program({0xE3A02009}, 0x108);                          // ARM again: MOV r2, #9
    f.cpu.reset();
    f.run(3);
    CHECK(f.cpu.thumb());
    f.run(1);
    CHECK(f.cpu.reg(1) == 0x104);
    f.run(2);  // MOV r0, #0 ; BX pc  (pc = 0x108, bit 0 clear -> ARM)
    CHECK_FALSE(f.cpu.thumb());
    f.run(1);
    CHECK(f.cpu.reg(2) == 9);
}

TEST_CASE("Thumb BL pair calls and returns with LR bit 0 set") {
    Fixture f;
    f.bus.program({0xE3A00C01, 0xE2800001, 0xE12FFF10});  // enter Thumb at 0x100
    // 0x100: BL 0x110 (two halves), 0x104: MOV r1, #1, 0x106: B .
    // 0x110: MOV r2, #2, 0x112: BX lr
    program16(f.bus, {0xF000, 0xF806, 0x2101, 0xE7FE}, 0x100);
    program16(f.bus, {0x2202, 0x4770}, 0x110);
    f.cpu.reset();
    f.run(3);
    f.run(2);  // BL prefix + suffix
    CHECK(f.cpu.reg(15) == 0x114);  // at 0x110, +4
    CHECK(f.cpu.reg(14) == 0x105);
    f.run(3);  // MOV r2 ; BX lr ; MOV r1
    CHECK(f.cpu.reg(2) == 2);
    CHECK(f.cpu.reg(1) == 1);
    CHECK(f.cpu.thumb());
}

TEST_CASE("Thumb PUSH/POP and PC-relative LDR alignment") {
    Fixture f;
    f.bus.program({0xE3A00C01, 0xE2800001, 0xE12FFF10, 0xE3A0DC02});  // sp is set below
    // 0x100: MOV r4, #4 ; PUSH {r4, lr} ; MOV r4, #0 ; POP {r4} ... ; LDR r5, [pc, #4]
    program16(f.bus, {0x2404, 0xB510, 0x2400, 0xBC10, 0x4D01, 0xE7FE}, 0x100);
    f.bus.write32(0x10C, 0x12345678, kNonseq);  // decoy: must not be the one loaded
    f.bus.write32(0x110, 0xCAFEF00D, kNonseq);
    f.cpu.reset();
    f.cpu.set_reg(13, 0x200);
    f.run(3);
    f.run(2);  // MOV r4, #4 ; PUSH {r4, lr}
    CHECK(f.cpu.reg(13) == 0x1F8);
    CHECK(f.bus.read32(0x1F8, kNonseq) == 4);
    f.run(2);  // MOV r4, #0 ; POP {r4}
    CHECK(f.cpu.reg(4) == 4);
    CHECK(f.cpu.reg(13) == 0x1FC);
    f.run(1);  // LDR r5, [pc, #4]: at 0x108, pc = 0x10C, aligned 0x10C + 4 = 0x110
    CHECK(f.cpu.reg(5) == 0xCAFEF00D);
}

TEST_CASE("Thumb ALU sets flags and SWI returns to ARM state") {
    Fixture f;
    f.bus.program({0xE3A00C01, 0xE2800001, 0xE12FFF10});
    program16(f.bus, {0x2000, 0x3801, 0xDF00}, 0x100);  // MOV r0, #0 ; SUB r0, #1 ; SWI 0
    f.cpu.reset();
    f.run(3);
    f.run(2);
    CHECK(f.cpu.reg(0) == 0xFFFFFFFF);
    CHECK((f.cpu.cpsr() & psr::kN) != 0);
    CHECK((f.cpu.cpsr() & psr::kC) == 0);  // borrow
    f.run(1);
    CHECK_FALSE(f.cpu.thumb());
    CHECK(f.cpu.mode() == Mode::Supervisor);
    CHECK(f.cpu.reg(14) == 0x106);  // next Thumb instruction
    CHECK((f.cpu.spsr(kBankSvc) & psr::kT) != 0);
}
