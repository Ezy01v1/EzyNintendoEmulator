#include <doctest/doctest.h>

#include <array>
#include <initializer_list>
#include <span>
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

namespace {

// A CGB-mode PPU with BG palette 0 = {white, red, green, blue} and OBJ palette 2 = the same.
Ppu make_cgb_ppu() {
    Ppu ppu;
    ppu.set_model(Model::Cgb, true);
    ppu.set_opri(0);  // what the bus sets for a CGB game
    const std::array<std::uint16_t, 4> colors = {0x7FFF, 0x001F, 0x03E0, 0x7C00};
    for (const std::uint16_t spec : {std::uint16_t{0xFF68}, std::uint16_t{0xFF6A}}) {
        for (int palette = 0; palette < 8; ++palette) {
            ppu.write_register(spec, static_cast<std::uint8_t>(0x80 | (palette * 8)));
            for (const auto c : colors) {
                ppu.write_register(static_cast<std::uint16_t>(spec + 1), static_cast<std::uint8_t>(c & 0xFF));
                ppu.write_register(static_cast<std::uint16_t>(spec + 1), static_cast<std::uint8_t>(c >> 8));
            }
        }
    }
    return ppu;
}

// Tile 0 row r: left pixel color index 1, the other 7 pixels index 0. Rows 0-3 only.
void write_marker_tile(Ppu& ppu, int bank) {
    for (int row = 0; row < 4; ++row) {
        ppu.write_vram_bank(bank, static_cast<std::uint16_t>(0x8000 + row * 2), 0x80);
    }
}

}  // namespace

TEST_CASE("BG attribute flips and bank") {
    Ppu ppu = make_cgb_ppu();
    write_marker_tile(ppu, 1);  // only bank 1 has the marker
    // Attribute for map entry (0,0): tile bank 1, X-flip, Y-flip.
    ppu.write_vram_bank(1, 0x9800, 0x08 | 0x20 | 0x40);
    run_dots(ppu, 456 * 8);  // draw lines 0-7
    const auto fb = ppu.framebuffer();
    // Flipped both ways: the marker column moves to x = 7 and rows 4-7.
    CHECK(fb[0] == 0x7FFF);
    CHECK(fb[7] == 0x7FFF);  // row 0 is now a blank row (source row 7)
    CHECK(fb[static_cast<std::size_t>(7 * kLcdWidth + 7)] == 0x001F);
    CHECK(fb[static_cast<std::size_t>(7 * kLcdWidth + 0)] == 0x7FFF);
}

TEST_CASE("BG attribute palette") {
    Ppu ppu = make_cgb_ppu();
    fill_tile(ppu, 0x8000, 1);
    ppu.write_register(0xFF68, 0x80 | (3 * 8 + 2));  // palette 3, colour 1
    ppu.write_register(0xFF69, 0x34);
    ppu.write_register(0xFF69, 0x12);
    ppu.write_vram_bank(1, 0x9800, 0x03);
    run_dots(ppu, 456);
    CHECK(ppu.framebuffer()[0] == 0x1234);
    CHECK(ppu.framebuffer()[8] == 0x001F);  // map entry (1,0) still palette 0
}

TEST_CASE("CGB sprites: palette from attributes, BG priority and LCDC bit 0") {
    Ppu ppu = make_cgb_ppu();
    fill_tile(ppu, 0x8000, 1);  // BG tile 0: colour 1 everywhere
    fill_tile(ppu, 0x8010, 3);  // sprite tile 1: colour 3
    ppu.write_vram_bank(1, 0x9800, 0x80);  // map (0,0) has BG-over-OBJ priority
    ppu.write_oam(0xFE00, 16);  // y
    ppu.write_oam(0xFE01, 8);   // x
    ppu.write_oam(0xFE02, 1);
    ppu.write_oam(0xFE03, 0x02);  // OBJ palette 2
    ppu.write_oam(0xFE04, 16);
    ppu.write_oam(0xFE05, 16);
    ppu.write_oam(0xFE06, 1);
    ppu.write_oam(0xFE07, 0x02);
    ppu.write_register(0xFF40, 0x93);  // LCD on, OBJ on, BG "priority" on
    run_dots(ppu, 456);
    CHECK(ppu.framebuffer()[0] == 0x001F);  // BG wins over the first sprite
    CHECK(ppu.framebuffer()[8] == 0x7C00);  // second sprite shows (no BG priority there)

    Ppu ppu2 = make_cgb_ppu();
    fill_tile(ppu2, 0x8000, 1);
    fill_tile(ppu2, 0x8010, 3);
    ppu2.write_vram_bank(1, 0x9800, 0x80);
    ppu2.write_oam(0xFE00, 16);
    ppu2.write_oam(0xFE01, 8);
    ppu2.write_oam(0xFE02, 1);
    ppu2.write_register(0xFF40, 0x92);  // LCDC bit 0 = 0: sprites always on top, BG still drawn
    run_dots(ppu2, 456);
    CHECK(ppu2.framebuffer()[0] == 0x7C00);
    CHECK(ppu2.framebuffer()[8] == 0x001F);
}

TEST_CASE("CGB sprite tile bank and OAM-order priority") {
    Ppu ppu = make_cgb_ppu();
    fill_tile(ppu, 0x8010, 2);
    for (int row = 0; row < 8; ++row) {  // bank 1 tile 1: colour 3
        ppu.write_vram_bank(1, static_cast<std::uint16_t>(0x8010 + row * 2), 0xFF);
        ppu.write_vram_bank(1, static_cast<std::uint16_t>(0x8011 + row * 2), 0xFF);
    }
    // Sprite 0 at x=4 from bank 1; sprite 1 at x=0 from bank 0. They overlap at x 4-7.
    ppu.write_oam(0xFE00, 16);
    ppu.write_oam(0xFE01, 12);
    ppu.write_oam(0xFE02, 1);
    ppu.write_oam(0xFE03, 0x08);
    ppu.write_oam(0xFE04, 16);
    ppu.write_oam(0xFE05, 8);
    ppu.write_oam(0xFE06, 1);
    ppu.write_oam(0xFE07, 0x00);
    ppu.write_register(0xFF40, 0x93);
    run_dots(ppu, 456);
    CHECK(ppu.framebuffer()[0] == 0x03E0);  // sprite 1 (colour 2) alone
    CHECK(ppu.framebuffer()[4] == 0x7C00);  // overlap: OAM index 0 wins on CGB despite larger X
}

TEST_CASE("VRAM and OAM locks follow the PPU mode") {
    Ppu ppu;  // LCD on after boot: line 0 starts in mode 2
    CHECK_FALSE(ppu.oam_accessible());
    CHECK(ppu.vram_accessible());
    run_dots(ppu, 80);
    REQUIRE(ppu.mode() == 3);
    CHECK_FALSE(ppu.oam_accessible());
    CHECK_FALSE(ppu.vram_accessible());
    run_dots(ppu, 172);
    REQUIRE(ppu.mode() == 0);
    CHECK(ppu.oam_accessible());
    CHECK(ppu.vram_accessible());
}

TEST_CASE("locks are lifted while the LCD is off") {
    Ppu ppu;
    run_dots(ppu, 80);
    ppu.write_register(0xFF40, 0x00);
    CHECK(ppu.oam_accessible());
    CHECK(ppu.vram_accessible());
}

TEST_CASE("SCX fine scroll lengthens mode 3") {
    Ppu ppu;
    ppu.write_register(0xFF43, 5);
    run_dots(ppu, 80 + 168);
    CHECK(ppu.mode() == 3);
    run_dots(ppu, 4);
    CHECK(ppu.mode() == 3);  // 169 + 5 = 174 dots: still drawing at dot 252
    run_dots(ppu, 4);
    CHECK(ppu.mode() == 0);
}

TEST_CASE("sprites on the line lengthen mode 3") {
    Ppu ppu;
    ppu.write_register(0xFF40, 0x00);
    for (std::uint16_t i = 0; i < 2; ++i) {
        ppu.write_oam(static_cast<std::uint16_t>(0xFE00 + i * 4), 16);  // on line 0
        ppu.write_oam(static_cast<std::uint16_t>(0xFE01 + i * 4), 8);
    }
    ppu.write_register(0xFF40, 0x93);
    run_dots(ppu, 80 + 172 + 8);
    CHECK(ppu.mode() == 3);  // 2 sprites add at least 12 dots
    run_dots(ppu, 40);
    CHECK(ppu.mode() == 0);
}

namespace {

// Dots spent in mode 3 on line 0 of a fresh frame (LCD switched off, configured, back on).
int mode3_dots(Ppu& ppu) {
    while (!(ppu.ly() == 1 && ppu.mode() == 2)) ppu.tick(1);  // finish the special first line
    while (ppu.mode() != 3) ppu.tick(1);
    int dots = 0;
    while (ppu.mode() == 3) {
        ppu.tick(1);
        ++dots;
    }
    return dots;
}

}  // namespace

// 169, not Pan Docs' 172: see the timing note at the top of ppu_draw.cpp.
TEST_CASE("FIFO: mode 3 lasts 169 dots with SCX = 0 and no sprites") {
    Ppu ppu;
    CHECK(mode3_dots(ppu) == 169);
}

TEST_CASE("FIFO: SCX % 8 adds its value to mode 3") {
    Ppu ppu;
    ppu.write_register(0xFF43, 3);
    CHECK(mode3_dots(ppu) == 172);
}

TEST_CASE("FIFO: the window adds 6 dots") {
    Ppu ppu;
    ppu.write_register(0xFF4A, 0);   // WY
    ppu.write_register(0xFF4B, 87);  // WX: window from x = 80
    ppu.write_register(0xFF40, 0x91 | 0x20);
    CHECK(mode3_dots(ppu) == 175);
}

TEST_CASE("FIFO: a mid-line BGP write changes only the pixels after it") {
    Ppu ppu;
    ppu.write_register(0xFF40, 0x00);
    fill_tile(ppu, 0x8000, 3);       // every BG tile: colour 3
    ppu.write_register(0xFF47, 0xFF);  // colour 3 -> shade 3
    ppu.write_register(0xFF40, 0x91);
    while (!(ppu.ly() == 1 && ppu.mode() == 3)) ppu.tick(1);
    for (int i = 0; i < 12 + 80; ++i) ppu.tick(1);  // about 80 pixels out
    ppu.write_register(0xFF47, 0x00);  // colour 3 -> shade 0
    while (ppu.mode() == 3) ppu.tick(1);
    const auto line = ppu.framebuffer().subspan(kLcdWidth, kLcdWidth);
    CHECK(line[0] == kDmgGreys[3]);
    CHECK(line[70] == kDmgGreys[3]);
    CHECK(line[90] == kDmgGreys[0]);
    CHECK(line[159] == kDmgGreys[0]);
}

TEST_CASE("FIFO: VRAM lock follows the real mode 3 end") {
    Ppu ppu;
    ppu.write_register(0xFF43, 7);
    while (!(ppu.ly() == 1 && ppu.mode() == 3)) ppu.tick(1);
    for (int i = 0; i < 176; ++i) {  // 169 + SCX % 8
        CHECK_FALSE(ppu.vram_accessible());
        ppu.tick(1);
    }
    CHECK(ppu.vram_accessible());
}

namespace {

// LCD off, sprites on (8x8), all of OAM moved off-screen; then the listed sprites on line 1.
Ppu make_sprite_ppu(std::initializer_list<std::uint8_t> xs) {
    Ppu ppu;
    ppu.write_register(0xFF40, 0x00);
    for (std::uint16_t i = 0; i < 0xA0; ++i) ppu.write_oam(static_cast<std::uint16_t>(0xFE00 + i), 0);
    std::uint16_t slot = 0;
    for (const auto x : xs) {
        ppu.write_oam(static_cast<std::uint16_t>(0xFE00 + slot * 4), 17);  // covers line 1
        ppu.write_oam(static_cast<std::uint16_t>(0xFE01 + slot * 4), x);
        ++slot;
    }
    ppu.write_register(0xFF40, 0x93);
    return ppu;
}

// Line 1, as written to the framebuffer (it is drawn after mode3_dots runs).
std::span<const std::uint16_t> line1(const Ppu& ppu) { return ppu.framebuffer().subspan(kLcdWidth, kLcdWidth); }

}  // namespace

TEST_CASE("FIFO sprites: one sprite at X=8 adds 11 dots") {
    Ppu ppu = make_sprite_ppu({8});
    CHECK(mode3_dots(ppu) == 169 + 11);
}

TEST_CASE("FIFO sprites: a sprite at X=0 is hidden but still stalls") {
    Ppu ppu = make_sprite_ppu({0});
    fill_tile(ppu, 0x8000, 3);
    ppu.write_register(0xFF47, 0x00);  // BG white, so a visible sprite pixel would show
    ppu.write_register(0xFF48, 0xFF);
    CHECK(mode3_dots(ppu) == 169 + 11);
    CHECK(line1(ppu)[0] == kDmgGreys[0]);
}

TEST_CASE("FIFO sprites: only 10 sprites per line are drawn") {
    Ppu ppu = make_sprite_ppu({8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88});
    fill_tile(ppu, 0x8000, 3);
    ppu.write_register(0xFF47, 0x00);  // BG all white
    ppu.write_register(0xFF48, 0xFF);  // OBP0: colour 3 -> black
    mode3_dots(ppu);
    CHECK(line1(ppu)[72] == kDmgGreys[3]);  // 10th sprite
    CHECK(line1(ppu)[80] == kDmgGreys[0]);  // 11th sprite: not drawn
}

TEST_CASE("FIFO sprites: sprites at X >= 168 count toward 10 but are not drawn") {
    Ppu ppu = make_sprite_ppu({168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 8});
    fill_tile(ppu, 0x8000, 3);
    ppu.write_register(0xFF47, 0x00);
    ppu.write_register(0xFF48, 0xFF);
    CHECK(mode3_dots(ppu) == 169);  // nothing fetched: the 11th sprite never made the list
    CHECK(line1(ppu)[0] == kDmgGreys[0]);
}

TEST_CASE("FIFO sprites: on DMG the lower X wins an overlap") {
    Ppu ppu = make_sprite_ppu({12, 8});  // OAM 0 at x=12, OAM 1 at x=8
    fill_tile(ppu, 0x8000, 3);
    ppu.write_oam(0xFE03, 0x10);  // OAM 0 uses OBP1
    ppu.write_register(0xFF47, 0x00);
    ppu.write_register(0xFF48, 0xFF);  // OBP0 -> black
    ppu.write_register(0xFF49, 0x55);  // OBP1 -> light grey
    mode3_dots(ppu);
    CHECK(line1(ppu)[6] == kDmgGreys[3]);  // overlap: OAM 1 (x=8) wins
}

TEST_CASE("FIFO sprites: on CGB the lower OAM index wins an overlap") {
    Ppu ppu = make_cgb_ppu();
    ppu.write_register(0xFF40, 0x00);
    for (std::uint16_t i = 0; i < 0xA0; ++i) ppu.write_oam(static_cast<std::uint16_t>(0xFE00 + i), 0);
    fill_tile(ppu, 0x8000, 0);
    fill_tile(ppu, 0x8010, 3);
    ppu.write_oam(0xFE00, 17);
    ppu.write_oam(0xFE01, 12);
    ppu.write_oam(0xFE02, 1);
    ppu.write_oam(0xFE03, 0x01);  // OAM 0: palette 1
    ppu.write_oam(0xFE04, 17);
    ppu.write_oam(0xFE05, 8);
    ppu.write_oam(0xFE06, 1);
    ppu.write_oam(0xFE07, 0x02);  // OAM 1: palette 2
    ppu.write_register(0xFF6A, 0x80 | (1 * 8 + 6));  // OBJ palette 1 colour 3
    ppu.write_register(0xFF6B, 0x34);
    ppu.write_register(0xFF6B, 0x12);
    ppu.write_register(0xFF40, 0x93);
    mode3_dots(ppu);
    CHECK(line1(ppu)[6] == 0x1234);  // OAM 0 wins although its X is larger
}

TEST_CASE("LY: line 153 reads 153 only briefly, then 0") {
    Ppu ppu;
    while (ppu.read_register(0xFF44) != 153) ppu.tick(1);
    int dots_at_153 = 0;
    while (ppu.read_register(0xFF44) == 153) {
        ppu.tick(1);
        ++dots_at_153;
    }
    CHECK(dots_at_153 == 4);
    CHECK(ppu.read_register(0xFF44) == 0);
    CHECK(ppu.mode() == 1);  // still VBlank
}

TEST_CASE("LCD on: the first line starts in mode 0, without an OAM scan") {
    Ppu ppu;
    ppu.write_register(0xFF40, 0x00);
    ppu.write_register(0xFF40, 0x91);
    CHECK(ppu.mode() == 0);
    ppu.tick(4);
    CHECK(ppu.mode() == 0);
    CHECK(ppu.oam_accessible());
    while (ppu.mode() == 0) ppu.tick(1);
    CHECK(ppu.mode() == 3);
}

TEST_CASE("LCD off and on mid-frame restarts at line 0 with nothing left over") {
    Ppu ppu;
    for (int i = 0; i < 456 * 5 + 120; ++i) ppu.tick(1);  // in the middle of mode 3
    ppu.write_register(0xFF40, 0x00);
    CHECK(ppu.read_register(0xFF44) == 0);
    ppu.write_register(0xFF40, 0x91);
    while (ppu.mode() != 0 || ppu.read_register(0xFF44) == 0) ppu.tick(1);  // finish line 0
    CHECK(ppu.read_register(0xFF44) == 1);
}

TEST_CASE("STAT LY=LYC bit is kept while the LCD is off") {
    Ppu ppu;
    ppu.write_register(0xFF45, 0);       // LYC = 0 = LY
    CHECK((ppu.read_register(0xFF41) & 0x04) != 0);
    ppu.write_register(0xFF40, 0x00);
    ppu.write_register(0xFF45, 5);       // the comparison doesn't run while off
    CHECK((ppu.read_register(0xFF41) & 0x04) != 0);
}

TEST_CASE("entering VBlank also fires the mode 2 STAT source") {
    Ppu ppu;
    ppu.write_register(0xFF41, 0x20);  // mode 2 interrupt only
    while (ppu.ly() != 143) ppu.tick(1);
    ppu.take_interrupts();
    while (ppu.ly() != 144) ppu.tick(1);
    CHECK((ppu.take_interrupts() & interrupt::kStat) != 0);
}
