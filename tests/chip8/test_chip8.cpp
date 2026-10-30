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
    const auto chip8 = run({0xA0, 0x50, 0xD0, 0x15, 0xD0, 0x15}, 3);
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

TEST_CASE("step fetches two bytes and advances pc") {
    Chip8 chip8;
    const std::array<std::uint8_t, 2> rom = {0x00, 0x00};
    REQUIRE(chip8.load_rom(rom));
    chip8.step();
    CHECK(chip8.pc() == kProgramStart + 2);
}
