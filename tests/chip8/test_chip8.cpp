#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "chip8/chip8.hpp"

using namespace ezy::chip8;

TEST_CASE("reset puts the machine in its power-on state") {
    Chip8 chip8;
    CHECK(chip8.pc() == kProgramStart);
    CHECK(chip8.index() == 0);
    for (std::size_t reg = 0; reg < 16; ++reg) {
        CHECK(chip8.v(reg) == 0);
    }
}

TEST_CASE("font is loaded at 0x050") {
    Chip8 chip8;
    CHECK(chip8.read(kFontStart) == 0xF0);      // first row of '0'
    CHECK(chip8.read(kFontStart + 5) == 0x20);  // first row of '1'
}

TEST_CASE("load_rom copies the program to 0x200") {
    Chip8 chip8;
    const std::array<std::uint8_t, 3> rom = {0x12, 0x34, 0x56};
    REQUIRE(chip8.load_rom(rom));
    CHECK(chip8.read(0x200) == 0x12);
    CHECK(chip8.read(0x201) == 0x34);
    CHECK(chip8.read(0x202) == 0x56);
}

TEST_CASE("load_rom rejects empty and oversized ROMs") {
    Chip8 chip8;
    CHECK_FALSE(chip8.load_rom({}));
    const std::vector<std::uint8_t> too_big(kMaxRomSize + 1, 0);
    CHECK_FALSE(chip8.load_rom(too_big));
}

namespace {

// Loads `rom` into a fresh machine and executes `steps` instructions.
Chip8 run(std::vector<std::uint8_t> rom, int steps) {
    Chip8 chip8;
    REQUIRE(chip8.load_rom(rom));
    for (int i = 0; i < steps; ++i) chip8.step();
    return chip8;
}

bool pixel(const Chip8& chip8, int x, int y) {
    return chip8.framebuffer()[static_cast<std::size_t>(y * kScreenWidth + x)] != 0;
}

}  // namespace

TEST_CASE("6XNN loads an immediate into VX") {
    const auto chip8 = run({0x6A, 0x2F}, 1);
    CHECK(chip8.v(0xA) == 0x2F);
}

TEST_CASE("7XNN adds without touching VF and wraps at 256") {
    const auto chip8 = run({0x60, 0xFF, 0x70, 0x02}, 2);
    CHECK(chip8.v(0x0) == 0x01);
    CHECK(chip8.v(0xF) == 0x00);
}

TEST_CASE("1NNN jumps") {
    const auto chip8 = run({0x13, 0x45}, 1);
    CHECK(chip8.pc() == 0x345);
}

TEST_CASE("ANNN sets I") {
    const auto chip8 = run({0xA1, 0x23}, 1);
    CHECK(chip8.index() == 0x123);
}

TEST_CASE("DXYN draws a font glyph and 00E0 clears it") {
    // I = font '0' (0x050); V0 = V1 = 0; draw 5 rows at (0, 0).
    const std::vector<std::uint8_t> program = {0xA0, 0x50, 0xD0, 0x15, 0x00, 0xE0};
    auto chip8 = run(program, 2);
    CHECK(pixel(chip8, 0, 0));       // 0xF0 -> top row lit
    CHECK(pixel(chip8, 3, 0));
    CHECK_FALSE(pixel(chip8, 4, 0));
    CHECK_FALSE(pixel(chip8, 1, 1));  // 0x90 -> middle of row 1 off
    CHECK(chip8.v(0xF) == 0);

    chip8.step();  // 00E0
    CHECK_FALSE(pixel(chip8, 0, 0));
}

TEST_CASE("DXYN drawing twice erases and sets VF") {
    auto chip8 = run({0xA0, 0x50, 0xD0, 0x15, 0xD0, 0x15}, 2);
    chip8.tick_timers();  // display wait: one draw per frame
    chip8.step();
    CHECK_FALSE(pixel(chip8, 0, 0));
    CHECK(chip8.v(0xF) == 1);
}

TEST_CASE("DXYN wraps the start position but clips at the edge") {
    // V0 = 62 + 64 (wraps to 62), V1 = 0: only 2 columns fit on screen.
    const auto chip8 = run({0x60, 126, 0xA0, 0x50, 0xD0, 0x11}, 3);
    CHECK(pixel(chip8, 62, 0));
    CHECK(pixel(chip8, 63, 0));
    CHECK_FALSE(pixel(chip8, 0, 0));  // no wrap-around to the left side
}

TEST_CASE("2NNN calls a subroutine and 00EE returns") {
    // 0x200: CALL 0x206 | 0x202: V0 = 1 | 0x204: (unused) | 0x206: V1 = 2 | 0x208: RET
    const std::vector<std::uint8_t> program = {0x22, 0x06, 0x60, 0x01, 0x00, 0x00, 0x61, 0x02, 0x00, 0xEE};
    auto chip8 = run(program, 1);
    CHECK(chip8.pc() == 0x206);
    chip8.step();  // V1 = 2
    chip8.step();  // RET
    CHECK(chip8.pc() == 0x202);
    CHECK(chip8.v(1) == 2);
}

TEST_CASE("3XNN skips only when equal") {
    CHECK(run({0x60, 0x05, 0x30, 0x05}, 2).pc() == 0x206);
    CHECK(run({0x60, 0x05, 0x30, 0x06}, 2).pc() == 0x204);
}

TEST_CASE("8XY4 adds with carry in VF") {
    auto chip8 = run({0x60, 0xFF, 0x61, 0x02, 0x80, 0x14}, 3);
    CHECK(chip8.v(0) == 0x01);
    CHECK(chip8.v(0xF) == 1);

    chip8 = run({0x60, 0x10, 0x61, 0x02, 0x80, 0x14}, 3);
    CHECK(chip8.v(0) == 0x12);
    CHECK(chip8.v(0xF) == 0);
}

TEST_CASE("8XY5 subtracts with VF = not borrow") {
    auto chip8 = run({0x60, 0x05, 0x61, 0x03, 0x80, 0x15}, 3);
    CHECK(chip8.v(0) == 0x02);
    CHECK(chip8.v(0xF) == 1);

    chip8 = run({0x60, 0x03, 0x61, 0x05, 0x80, 0x15}, 3);
    CHECK(chip8.v(0) == 0xFE);
    CHECK(chip8.v(0xF) == 0);
}

TEST_CASE("VF as destination ends up holding the flag") {
    // VF = 0xFF, V1 = 2, VF += V1 -> result 0x01 is overwritten by carry 1.
    const auto chip8 = run({0x6F, 0xFF, 0x61, 0x02, 0x8F, 0x14}, 3);
    CHECK(chip8.v(0xF) == 1);
}

TEST_CASE("8XY6 and 8XYE shift VY into VX (COSMAC VIP)") {
    auto chip8 = run({0x61, 0x05, 0x80, 0x16}, 2);
    CHECK(chip8.v(0) == 0x02);
    CHECK(chip8.v(0xF) == 1);

    chip8 = run({0x61, 0x81, 0x80, 0x1E}, 2);
    CHECK(chip8.v(0) == 0x02);
    CHECK(chip8.v(0xF) == 1);
}

TEST_CASE("FX33 stores BCD") {
    // V0 = 254, I = 0x300, BCD
    const auto chip8 = run({0x60, 0xFE, 0xA3, 0x00, 0xF0, 0x33}, 3);
    CHECK(chip8.read(0x300) == 2);
    CHECK(chip8.read(0x301) == 5);
    CHECK(chip8.read(0x302) == 4);
}

TEST_CASE("FX55 / FX65 round-trip registers and advance I") {
    // V0 = 1, V1 = 2, I = 0x300, store V0..V1, V0 = V1 = 0, I = 0x300, load V0..V1
    const std::vector<std::uint8_t> program = {0x60, 0x01, 0x61, 0x02, 0xA3, 0x00, 0xF1, 0x55,
                                               0x60, 0x00, 0x61, 0x00, 0xA3, 0x00, 0xF1, 0x65};
    const auto chip8 = run(program, 8);
    CHECK(chip8.v(0) == 1);
    CHECK(chip8.v(1) == 2);
    CHECK(chip8.index() == 0x302);
}

TEST_CASE("FX29 points I at the font glyph") {
    const auto chip8 = run({0x60, 0x0A, 0xF0, 0x29}, 2);
    CHECK(chip8.index() == kFontStart + 0xA * 5);
}

TEST_CASE("CXNN is reproducible with a fixed seed and masked by NN") {
    const std::array<std::uint8_t, 2> rom = {0xC0, 0x0F};
    Chip8 a(1234);
    Chip8 b(1234);
    REQUIRE(a.load_rom(rom));
    REQUIRE(b.load_rom(rom));
    a.step();
    b.step();
    CHECK(a.v(0) == b.v(0));
    CHECK(a.v(0) <= 0x0F);
}

TEST_CASE("FX0A waits for a key press and release") {
    Chip8 chip8;
    const std::array<std::uint8_t, 2> rom = {0xF3, 0x0A};
    REQUIRE(chip8.load_rom(rom));
    chip8.step();
    CHECK(chip8.pc() == 0x200);  // still waiting
    chip8.set_key(0x7, true);
    chip8.step();
    CHECK(chip8.pc() == 0x200);  // pressed but not released yet
    chip8.set_key(0x7, false);
    chip8.step();
    CHECK(chip8.pc() == 0x202);
    CHECK(chip8.v(3) == 0x7);
}

TEST_CASE("quirk: display wait allows one draw per frame") {
    auto chip8 = run({0xA0, 0x50, 0xD0, 0x15, 0xD0, 0x15}, 3);
    CHECK(chip8.pc() == 0x204);  // second DXYN stalled
    chip8.tick_timers();
    chip8.step();
    CHECK(chip8.pc() == 0x206);
}

TEST_CASE("quirk: shift in place when shift_uses_vy is off") {
    Chip8 chip8;
    chip8.set_quirks(Quirks::superchip_modern());
    const std::array<std::uint8_t, 6> rom = {0x60, 0x04, 0x61, 0xFF, 0x80, 0x16};
    REQUIRE(chip8.load_rom(rom));
    for (int i = 0; i < 3; ++i) chip8.step();
    CHECK(chip8.v(0) == 0x02);  // V0 >> 1, V1 ignored
    CHECK(chip8.v(0xF) == 0);
}

TEST_CASE("quirk: FX55 leaves I untouched when memory_increment is off") {
    Chip8 chip8;
    chip8.set_quirks(Quirks::superchip_modern());
    const std::array<std::uint8_t, 4> rom = {0xA3, 0x00, 0xF2, 0x55};
    REQUIRE(chip8.load_rom(rom));
    chip8.step();
    chip8.step();
    CHECK(chip8.index() == 0x300);
}

TEST_CASE("quirk: BXNN jumps with VX when jump_uses_v0 is off") {
    Chip8 chip8;
    chip8.set_quirks(Quirks::superchip_modern());
    const std::array<std::uint8_t, 6> rom = {0x60, 0x10, 0x62, 0x01, 0xB2, 0x00};
    REQUIRE(chip8.load_rom(rom));
    for (int i = 0; i < 3; ++i) chip8.step();
    CHECK(chip8.pc() == 0x201);  // 0x200 + V2
}

TEST_CASE("step fetches two bytes and advances pc") {
    Chip8 chip8;
    const std::array<std::uint8_t, 2> rom = {0x00, 0x00};
    REQUIRE(chip8.load_rom(rom));
    chip8.step();
    CHECK(chip8.pc() == kProgramStart + 2);
}
