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

TEST_CASE("step fetches two bytes and advances pc") {
    Chip8 chip8;
    const std::array<std::uint8_t, 2> rom = {0x00, 0x00};
    REQUIRE(chip8.load_rom(rom));
    chip8.step();
    CHECK(chip8.pc() == kProgramStart + 2);
}
