#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "gb/interrupts.hpp"
#include "gb/model.hpp"
#include "gb/ppu.hpp"

using namespace ezy::gb;

namespace {

void run_dots(Ppu& ppu, int dots) {
    for (int i = 0; i < dots / 4; ++i) ppu.tick();
}

// Writes an 8x8 tile where every pixel has `color_index`.
void fill_tile(Ppu& ppu, std::uint16_t tile_addr, std::uint8_t color_index) {
    for (int row = 0; row < 8; ++row) {
        ppu.write_vram(static_cast<std::uint16_t>(tile_addr + row * 2), (color_index & 1) ? 0xFF : 0x00);
        ppu.write_vram(static_cast<std::uint16_t>(tile_addr + row * 2 + 1), (color_index & 2) ? 0xFF : 0x00);
    }
}

// The DMG shade (0-3) of a pixel: the DMG model outputs one fixed RGB555 colour per shade.
std::uint8_t pixel(const Ppu& ppu, int x, int y) {
    const std::uint16_t color = ppu.framebuffer()[static_cast<std::size_t>(y * kLcdWidth + x)];
    for (std::uint8_t shade = 0; shade < 4; ++shade) {
        if (kDmgGreys[shade] == color) return shade;
    }
    return 0xFF;
}

}  // namespace

TEST_CASE("mode sequence within a visible line") {
    Ppu ppu;
    CHECK(ppu.mode() == 2);
    run_dots(ppu, 80);
    CHECK(ppu.mode() == 3);
    run_dots(ppu, 172);
    CHECK(ppu.mode() == 0);
    run_dots(ppu, 456 - 252);
    CHECK(ppu.ly() == 1);
    CHECK(ppu.mode() == 2);
}

TEST_CASE("VBlank starts at line 144 and the frame wraps after 154 lines") {
    Ppu ppu;
    run_dots(ppu, kDotsPerLine * 144);
    CHECK(ppu.ly() == 144);
    CHECK(ppu.mode() == 1);
    CHECK((ppu.take_interrupts() & interrupt::kVBlank) != 0);
    CHECK(ppu.take_frame_ready());
    run_dots(ppu, kDotsPerLine * 10);
    CHECK(ppu.ly() == 0);
}

TEST_CASE("STAT interrupt on LY == LYC") {
    Ppu ppu;
    ppu.write_register(0xFF45, 3);     // LYC = 3
    ppu.write_register(0xFF41, 0x40);  // enable the LYC source
    ppu.take_interrupts();
    run_dots(ppu, kDotsPerLine * 3);
    CHECK((ppu.take_interrupts() & interrupt::kStat) != 0);
    CHECK((ppu.read_register(0xFF41) & 0x04) != 0);  // coincidence flag
}

TEST_CASE("turning the LCD off resets LY") {
    Ppu ppu;
    run_dots(ppu, kDotsPerLine * 10);
    ppu.write_register(0xFF40, 0x11);
    CHECK(ppu.ly() == 0);
    CHECK((ppu.read_register(0xFF41) & 0x03) == 0);
}

TEST_CASE("background tile is drawn through BGP") {
    Ppu ppu;
    fill_tile(ppu, 0x8010, 3);                        // tile 1, color index 3
    ppu.write_vram(0x9800, 1);                        // map (0,0) -> tile 1
    ppu.write_register(0xFF40, 0x91);                 // LCD on, BG on, tile data 8000
    ppu.write_register(0xFF47, 0b11'10'01'00);        // identity palette
    run_dots(ppu, kDotsPerLine);
    CHECK(pixel(ppu, 0, 0) == 3);
    CHECK(pixel(ppu, 8, 0) == 0);  // map (1,0) -> tile 0, empty
}

TEST_CASE("sprite drawn over BG, hidden behind non-zero BG when priority bit set") {
    Ppu ppu;
    fill_tile(ppu, 0x8010, 2);  // tile 1: color 2 (sprite)
    fill_tile(ppu, 0x8020, 1);  // tile 2: color 1 (background)
    ppu.write_vram(0x9800, 2);
    ppu.write_vram(0x9801, 2);
    ppu.write_register(0xFF47, 0b11'10'01'00);
    ppu.write_register(0xFF48, 0b11'10'01'00);
    ppu.write_register(0xFF40, 0x93);  // LCD, BG and OBJ on

    // Sprite 0 at screen (0,0); sprite 1 at screen (8,0) with BG priority.
    ppu.write_oam(0xFE00, 16);
    ppu.write_oam(0xFE01, 8);
    ppu.write_oam(0xFE02, 1);
    ppu.write_oam(0xFE04, 16);
    ppu.write_oam(0xFE05, 16);
    ppu.write_oam(0xFE06, 1);
    ppu.write_oam(0xFE07, 0x80);
    run_dots(ppu, kDotsPerLine);
    CHECK(pixel(ppu, 0, 0) == 2);  // sprite over BG
    CHECK(pixel(ppu, 8, 0) == 1);  // BG color 1 is not 0, so it covers the sprite
}

TEST_CASE("DMG model outputs fixed greys") {
    CHECK(kDmgGreys == std::array<std::uint16_t, 4>{0x7FFF, 0x56B5, 0x294A, 0x0000});
}

TEST_CASE("BCPD auto-increments on write only") {
    Ppu ppu;
    ppu.set_model(Model::Cgb, true);
    ppu.write_register(0xFF40, 0x00);  // LCD off: palettes always accessible
    ppu.write_register(0xFF68, 0x80);  // index 0, auto-increment
    CHECK(ppu.read_register(0xFF68) == 0xC0);
    ppu.write_register(0xFF69, 0x1F);
    ppu.write_register(0xFF69, 0x00);
    CHECK(ppu.read_register(0xFF68) == 0xC2);
    CHECK(ppu.read_register(0xFF69) == 0x00);  // index 2: reading does not increment
    CHECK(ppu.read_register(0xFF68) == 0xC2);
    CHECK(ppu.bg_palette_ram()[0] == 0x1F);
    ppu.write_register(0xFF68, 0x3F);  // index 63, no auto-increment
    ppu.write_register(0xFF69, 0x7F);
    CHECK(ppu.read_register(0xFF68) == 0x7F);
    CHECK(ppu.bg_palette_ram()[63] == 0x7F);
    ppu.write_register(0xFF6A, 0x81);
    ppu.write_register(0xFF6B, 0x55);
    CHECK(ppu.obj_palette_ram()[1] == 0x55);
    CHECK(ppu.read_register(0xFF6A) == 0xC2);
}

TEST_CASE("palette data is locked during mode 3") {
    Ppu ppu;
    ppu.set_model(Model::Cgb, true);
    ppu.write_register(0xFF68, 0x80);
    run_dots(ppu, 80);  // LCD on (post-boot): now in mode 3
    REQUIRE(ppu.mode() == 3);
    CHECK(ppu.read_register(0xFF69) == 0xFF);
    ppu.write_register(0xFF69, 0x12);
    CHECK(ppu.bg_palette_ram()[0] == 0x00);       // dropped...
    CHECK(ppu.read_register(0xFF68) == 0xC1);  // ...but the index still moved
}

TEST_CASE("compat mode maps BGP through palette RAM") {
    Ppu ppu;
    ppu.set_model(Model::Cgb, false);
    const std::array<std::uint16_t, 4> bg = {0x001F, 0x03E0, 0x7C00, 0x1234};
    ppu.set_compat_palettes(bg, bg, bg);
    fill_tile(ppu, 0x8000, 1);  // every BG tile = tile 0 = color index 1
    ppu.write_register(0xFF47, 0xE8);  // index 1 -> shade 2
    run_dots(ppu, 456);
    CHECK(ppu.framebuffer()[0] == 0x7C00);
}
