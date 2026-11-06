#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "gba/gba.hpp"
#include "gba/hle_bios.hpp"

using namespace ezy::gba;

namespace {

// A ROM with the given ARM words at 0x08000000 + offset (the header area is left zero,
// which parse_header accepts).
struct RomBuilder {
    std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(0x400, 0);
    RomBuilder& at(std::uint32_t offset, std::initializer_list<std::uint32_t> words) {
        for (const auto w : words) {
            for (int b = 0; b < 4; ++b) rom[offset + static_cast<std::uint32_t>(b)] = static_cast<std::uint8_t>(w >> (8 * b));
            offset += 4;
        }
        return *this;
    }
    [[nodiscard]] std::unique_ptr<Gba> boot() const { return Gba::create(rom); }
};

// Installs `handler` as the game's IRQ handler and enables the VBlank IRQ (DISPSTAT + IE).
// Ends at offset 0x24; the literal pool is at 0x40/0x44.
RomBuilder& vblank_irq_setup(RomBuilder& b, std::uint32_t handler) {
    return b
        .at(0x00, {
            0xE59F0038,  // LDR  r0, =handler          (0x40)
            0xE59F1038,  // LDR  r1, =0x03007FFC       (0x44)
            0xE5810000,  // STR  r0, [r1]
            0xE3A02301,  // MOV  r2, #0x04000000
            0xE3A03008,  // MOV  r3, #8                 DISPSTAT: VBlank IRQ enable
            0xE1C230B4,  // STRH r3, [r2, #4]
            0xE3A03001,  // MOV  r3, #1
            0xE2824C02,  // ADD  r4, r2, #0x200
            0xE1C430B0,  // STRH r3, [r4]               IE = VBlank
        })
        .at(0x40, {handler, kBiosIrqHandlerAddr});
}

// Game IRQ handler (ARM, at 0x60): count in r6, acknowledge IF, set BIOS_IF, return.
RomBuilder& counting_handler(RomBuilder& b) {
    return b.at(0x60, {
        0xE2866001,  // ADD  r6, r6, #1
        0xE3A00301,  // MOV  r0, #0x04000000
        0xE2800C02,  // ADD  r0, r0, #0x200
        0xE3A01001,  // MOV  r1, #1
        0xE1C010B2,  // STRH r1, [r0, #2]            IF = VBlank (acknowledge)
        0xE59F2010,  // LDR  r2, =0x03007FF8         (literal at 0x8C)
        0xE1D230B0,  // LDRH r3, [r2]
        0xE3833001,  // ORR  r3, r3, #1
        0xE1C230B0,  // STRH r3, [r2]                BIOS_IF |= VBlank
        0xE12FFF1E,  // BX   lr
        0x00000000,
        kBiosIfAddr,  // 0x8C
    });
}

}  // namespace

TEST_CASE("our BIOS image has the IRQ trampoline at 0x18") {
    const auto gba = RomBuilder{}.at(0, {0xEAFFFFFE}).boot();
    REQUIRE(gba);
    CHECK(gba->bus().read32(0x18) == 0xE92D500F);
    CHECK(gba->bus().read32(0x24) == 0xE510F004);
}

TEST_CASE("VBlank IRQ goes through the BIOS to the game's handler and back") {
    RomBuilder b;
    vblank_irq_setup(b, 0x08000060).at(0x24, {
        0xE5843008,  // STR r3, [r4, #8]    IME = 1
        0xE2855001,  // ADD r5, r5, #1      (main loop)
        0xEAFFFFFD,  // B   loop
    });
    counting_handler(b);
    const auto gba = b.boot();
    REQUIRE(gba);
    for (int i = 0; i < 3; ++i) gba->run_frame();
    gba->run_until(gba->scheduler().now() + 2000);  // let the third IRQ finish
    CHECK(gba->cpu().reg(6) == 3);                   // one IRQ per VBlank
    CHECK(gba->cpu().reg(5) > 1000);                 // the main loop kept running
    CHECK(gba->cpu().mode() == Mode::System);        // back from IRQ mode
    CHECK((gba->bus().interrupt_flags() & 1) == 0);  // acknowledged
}

TEST_CASE("IRQs are not taken while IME is off or CPSR.I is set") {
    RomBuilder b;
    vblank_irq_setup(b, 0x08000060).at(0x24, {
        0xE2855001,  // ADD r5, r5, #1   (IME stays 0)
        0xEAFFFFFD,  // B   loop
    });
    counting_handler(b);
    const auto gba = b.boot();
    REQUIRE(gba);
    gba->run_frame();
    gba->run_until(gba->scheduler().now() + 2000);
    CHECK(gba->cpu().reg(6) == 0);
    CHECK((gba->bus().interrupt_flags() & 1) != 0);  // requested but not taken

    gba->bus().write16(0x04000208, 1);  // IME on, but mask IRQs in the CPSR
    gba->cpu().set_cpsr_raw(gba->cpu().cpsr() | psr::kI);
    gba->run_until(gba->scheduler().now() + 2000);
    CHECK(gba->cpu().reg(6) == 0);
}

TEST_CASE("IRQ return address is the next instruction in Thumb too") {
    // Thumb main loop: enter Thumb at 0x100, then "ADD r5, #1 ; B loop".
    RomBuilder b;
    vblank_irq_setup(b, 0x08000060).at(0x24, {
        0xE5843008,  // STR r3, [r4, #8]      IME = 1
        0xE28F0001,  // ADD r0, pc, #1        -> 0x08000031 (Thumb)
        0xE2800E0D,  // ADD r0, r0, #0xD0     -> 0x08000101
        0xE12FFF10,  // BX  r0
    });
    counting_handler(b);
    b.rom[0x100] = 0x01;  // ADD r5, #1   (0x3501)
    b.rom[0x101] = 0x35;
    b.rom[0x102] = 0xFD;  // B loop       (0xE7FD)
    b.rom[0x103] = 0xE7;
    const auto gba = b.boot();
    REQUIRE(gba);
    for (int i = 0; i < 2; ++i) gba->run_frame();
    gba->run_until(gba->scheduler().now() + 2000);
    CHECK(gba->cpu().reg(6) == 2);
    CHECK(gba->cpu().thumb());
    const std::uint32_t pc = gba->cpu().next_instruction_address();
    CHECK((pc == 0x08000100 || pc == 0x08000102));
}

TEST_CASE("HALTCNT halts until IE & IF, even with IME off, and skips idle time") {
    RomBuilder b;
    vblank_irq_setup(b, 0x08000060).at(0x24, {
        0xE3A03000,  // MOV  r3, #0
        0xE2824C03,  // ADD  r4, r2, #0x300
        0xE5C43001,  // STRB r3, [r4, #1]   HALTCNT
        0xE2855001,  // ADD  r5, r5, #1
        0xEAFFFFFE,  // B .
    });
    const auto gba = b.boot();
    REQUIRE(gba);
    gba->run_until(100'000);
    CHECK(gba->bus().halted());
    CHECK(gba->cpu().reg(5) == 0);
    gba->run_until(200'000);  // VBlank at 197,120 sets IF: wakes up (no IRQ: IME is 0)
    CHECK_FALSE(gba->bus().halted());
    CHECK(gba->cpu().reg(5) == 1);
}

TEST_CASE("VBlankIntrWait returns once per frame") {
    RomBuilder b;
    vblank_irq_setup(b, 0x08000060).at(0x24, {
        0xEF050000,  // SWI  0x05           VBlankIntrWait (loop)
        0xE2877001,  // ADD  r7, r7, #1
        0xEAFFFFFC,  // B    loop
    });
    counting_handler(b);
    const auto gba = b.boot();
    REQUIRE(gba);
    for (int i = 0; i < 5; ++i) gba->run_frame();
    gba->run_until(gba->scheduler().now() + 2000);
    CHECK(gba->cpu().reg(7) == 5);
    CHECK(gba->cpu().reg(6) == 5);
    // It halts in between: almost no instructions run, so the main loop count stays exact.
    gba->run_until(gba->scheduler().now() + 100'000);
    CHECK(gba->cpu().reg(7) == 5);
}

TEST_CASE("Div, DivArm and Sqrt") {
    const auto gba = RomBuilder{}
                         .at(0, {
                             0xE3E00006,  // MVN r0, #6      r0 = -7
                             0xE3A01002,  // MOV r1, #2
                             0xEF060000,  // SWI 0x06        Div
                             0xE1A08000,  // MOV r8, r0
                             0xE1A09001,  // MOV r9, r1
                             0xE1A0A003,  // MOV r10, r3
                             0xE3A00F19,  // MOV r0, #100
                             0xEF080000,  // SWI 0x08        Sqrt
                             0xEAFFFFFE,  // B .
                         })
                         .boot();
    REQUIRE(gba);
    gba->run_until(2000);
    CHECK(gba->cpu().reg(8) == 0xFFFFFFFD);  // -3
    CHECK(gba->cpu().reg(9) == 0xFFFFFFFF);  // -1
    CHECK(gba->cpu().reg(10) == 3);
    CHECK(gba->cpu().reg(0) == 10);
}

TEST_CASE("CpuSet copies halfwords and CpuFastSet fills words") {
    const auto gba = RomBuilder{}
                         .at(0, {
                             0xE59F0020,  // LDR r0, =src     (0x28)
                             0xE59F1020,  // LDR r1, =dst     (0x2C)
                             0xE3A02004,  // MOV r2, #4       4 halfwords
                             0xEF0B0000,  // SWI 0x0B         CpuSet
                             0xE59F0018,  // LDR r0, =src     (0x30)
                             0xE59F1018,  // LDR r1, =dst2    (0x34)
                             0xE59F2018,  // LDR r2, =0x01000005  fill, 5 -> 8 words
                             0xEF0C0000,  // SWI 0x0C         CpuFastSet
                             0xEAFFFFFE,  // B .
                             0x00000000,
                             0x08000100,  // 0x28 src
                             0x02000000,  // 0x2C dst
                             0x08000100,  // 0x30 src
                             0x02000100,  // 0x34 dst2
                             0x01000005,  // 0x38
                         })
                         .at(0x100, {0x22221111, 0x44443333})
                         .boot();
    REQUIRE(gba);
    gba->run_until(5000);
    CHECK(gba->bus().read32(0x02000000) == 0x22221111);
    CHECK(gba->bus().read32(0x02000004) == 0x44443333);
    for (std::uint32_t i = 0; i < 8; ++i) CHECK(gba->bus().read32(0x02000100 + i * 4) == 0x22221111);
    CHECK(gba->bus().read32(0x02000120) == 0);
}
