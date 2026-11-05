#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "gba/bus.hpp"
#include "gba/ppu.hpp"
#include "gba/scheduler.hpp"

using namespace ezy::gba;

namespace {

// A bus with the PPU running, driven by hand: advance time and dispatch PPU events.
struct Fixture {
    Scheduler scheduler;
    Bus bus{std::vector<std::uint8_t>(0x200), {}, &scheduler};

    Fixture() { bus.ppu().start(); }

    void run_to(std::uint64_t cycle) {
        scheduler.advance(cycle - scheduler.now());
        while (const auto e = scheduler.pop_due()) {
            if (e->type == EventType::PpuHDrawEnd) bus.ppu().on_hdraw_end(e->when);
            if (e->type == EventType::PpuHBlankEnd) bus.ppu().on_hblank_end(e->when);
        }
    }
    void run_for(std::uint64_t cycles) { run_to(scheduler.now() + cycles); }
    [[nodiscard]] std::uint16_t dispstat() const { return bus.read16(0x04000004); }
    [[nodiscard]] std::uint16_t vcount() const { return bus.read16(0x04000006); }
    [[nodiscard]] std::uint16_t pixel(int x, int y) const {
        return bus.ppu().framebuffer()[static_cast<std::size_t>(y * kScreenWidth + x)];
    }
};

}  // namespace

TEST_CASE("PPU HBlank flag follows the 960/272 split") {
    Fixture f;
    f.run_to(959);
    CHECK((f.dispstat() & 0x2) == 0);
    f.run_to(960);
    CHECK((f.dispstat() & 0x2) != 0);
    f.run_to(1231);
    CHECK((f.dispstat() & 0x2) != 0);
    CHECK(f.vcount() == 0);
    f.run_to(1232);
    CHECK((f.dispstat() & 0x2) == 0);
    CHECK(f.vcount() == 1);
}

TEST_CASE("PPU VCOUNT wraps after 227 and VBlank covers lines 160-226") {
    Fixture f;
    f.run_to(159ULL * kCyclesPerLine);
    CHECK(f.vcount() == 159);
    CHECK((f.dispstat() & 0x1) == 0);
    f.run_to(160ULL * kCyclesPerLine);
    CHECK(f.vcount() == 160);
    CHECK((f.dispstat() & 0x1) != 0);
    CHECK(f.bus.ppu().take_frame_done());
    CHECK_FALSE(f.bus.ppu().take_frame_done());
    f.run_to(227ULL * kCyclesPerLine);
    CHECK(f.vcount() == 227);
    CHECK((f.dispstat() & 0x1) == 0);  // line 227 is not flagged
    f.run_to(228ULL * kCyclesPerLine);
    CHECK(f.vcount() == 0);
}

TEST_CASE("PPU status bits are read-only, LYC and IRQ enables are writable") {
    Fixture f;
    f.run_to(960);  // in HBlank
    f.bus.write16(0x04000004, 0xFF38);  // try to clear HBlank, set enables + LYC=0xFF
    CHECK(f.dispstat() == (0xFF38 | 0x2));
    f.bus.write16(0x04000006, 0x0055);  // VCOUNT is read-only
    CHECK(f.vcount() == 0);
}

TEST_CASE("PPU interrupts fire only when enabled in DISPSTAT") {
    Fixture f;
    f.run_to(160ULL * kCyclesPerLine);
    CHECK(f.bus.interrupt_flags() == 0);  // nothing enabled yet

    f.bus.write16(0x04000004, 0x0008 | 0x0010 | 0x0020 | (5U << 8));  // VBlank, HBlank, VCount (LYC 5)
    f.run_to(228ULL * kCyclesPerLine + 5 * kCyclesPerLine);  // next frame, line 5
    CHECK((f.bus.interrupt_flags() & irq::kVCount) != 0);
    CHECK((f.dispstat() & 0x4) != 0);
    CHECK((f.bus.interrupt_flags() & irq::kHBlank) != 0);
    f.run_to(228ULL * kCyclesPerLine + 160 * kCyclesPerLine);
    CHECK((f.bus.interrupt_flags() & irq::kVBlank) != 0);
}

TEST_CASE("IF: writing 1 acknowledges an interrupt, writing 0 leaves it") {
    Fixture f;
    f.bus.request_interrupt(irq::kVBlank | irq::kHBlank);
    f.bus.write16(0x04000202, irq::kHBlank);
    CHECK(f.bus.interrupt_flags() == irq::kVBlank);
    f.bus.write16(0x04000202, 0);
    CHECK(f.bus.interrupt_flags() == irq::kVBlank);
    f.bus.write32(0x04000200, 0x00010003);  // IE=3 and ack VBlank in one word
    CHECK(f.bus.interrupt_enable() == 3);
    CHECK(f.bus.interrupt_flags() == 0);
}

TEST_CASE("KEYINPUT is active-low and read-only") {
    Fixture f;
    CHECK(f.bus.read16(0x04000130) == 0x03FF);
    f.bus.set_keys(0x0009);  // A + Start
    CHECK(f.bus.read16(0x04000130) == 0x03F6);
    f.bus.write16(0x04000130, 0);
    CHECK(f.bus.read16(0x04000130) == 0x03F6);
}

TEST_CASE("PPU mode 3 draws direct colors") {
    Fixture f;
    f.bus.write16(0x04000000, 0x0403);  // mode 3, BG2 on
    f.bus.write16(0x06000000 + (10 * 240 + 7) * 2, 0x001F);  // red at (7, 10)
    f.run_to(11ULL * kCyclesPerLine);
    CHECK(f.pixel(7, 10) == 0x001F);
    CHECK(f.pixel(8, 10) == 0x0000);
}

TEST_CASE("PPU mode 4 uses the palette and the page bit") {
    Fixture f;
    f.bus.write16(0x05000000 + 3 * 2, 0x03E0);  // palette[3] = green
    f.bus.write16(0x05000000 + 4 * 2, 0x7C00);  // palette[4] = blue
    f.bus.write8(0x06000000 + 2 * 240 + 1, 3);  // byte writes go to both halves: (0..1, 2) = 3
    f.bus.write8(0x0600A000 + 2 * 240 + 1, 4);  // page 1
    f.bus.write16(0x04000000, 0x0404);          // mode 4, page 0
    f.run_to(3ULL * kCyclesPerLine);
    CHECK(f.pixel(1, 2) == 0x03E0);
    f.bus.write16(0x04000000, 0x0414);  // page 1, from the next frame on
    f.run_to(228ULL * kCyclesPerLine + 3 * kCyclesPerLine);
    CHECK(f.pixel(1, 2) == 0x7C00);
}

TEST_CASE("PPU mode 5 is 160x128 with the backdrop around it") {
    Fixture f;
    f.bus.write16(0x05000000, 0x1234);  // backdrop
    f.bus.write16(0x06000000 + (0 * 160 + 159) * 2, 0x7FFF);
    f.bus.write16(0x04000000, 0x0405);
    f.run_to(1ULL * kCyclesPerLine);
    CHECK(f.pixel(159, 0) == 0x7FFF);
    CHECK(f.pixel(160, 0) == 0x1234);
}

TEST_CASE("PPU forced blank is white and BG2 off shows the backdrop") {
    Fixture f;
    f.bus.write16(0x05000000, 0x0421);
    f.bus.write16(0x04000000, 0x0083);  // mode 3, forced blank
    f.run_to(1ULL * kCyclesPerLine);
    CHECK(f.pixel(0, 0) == 0x7FFF);
    f.bus.write16(0x04000000, 0x0003);  // mode 3, BG2 off
    f.run_to(2ULL * kCyclesPerLine);
    CHECK(f.pixel(0, 1) == 0x0421);
}

// --- Tile modes and sprites (exercise 3.7) ---

namespace {

constexpr std::uint32_t kVram = 0x06000000;
constexpr std::uint32_t kPal = 0x05000000;
constexpr std::uint32_t kObjPal = 0x05000200;
constexpr std::uint32_t kObjVram = 0x06010000;
constexpr std::uint32_t kOam = 0x07000000;

// Fills a 4bpp tile (32 bytes) with one color index.
void fill_tile4(Fixture& f, std::uint32_t at, std::uint32_t index) {
    const auto half = static_cast<std::uint16_t>((index | (index << 4)) * 0x0101);
    for (std::uint32_t i = 0; i < 32; i += 2) f.bus.write16(at + i, half);
}

// Fills an 8bpp tile (64 bytes) with one color index.
void fill_tile8(Fixture& f, std::uint32_t at, std::uint32_t index) {
    for (std::uint32_t i = 0; i < 64; i += 2) f.bus.write16(at + i, static_cast<std::uint16_t>(index * 0x0101));
}

// Sets the color index of one pixel of a 4bpp tile.
void set_pixel4(Fixture& f, std::uint32_t tile_at, std::uint32_t x, std::uint32_t y, std::uint32_t index) {
    const std::uint32_t at = tile_at + y * 4 + x / 2;
    const std::uint32_t shift = (at & 1) * 8 + (x & 1) * 4;
    const std::uint32_t half = (f.bus.read16(at & ~1U) & ~(0xFU << shift)) | (index << shift);
    f.bus.write16(at & ~1U, static_cast<std::uint16_t>(half));
}

void set_sprite(Fixture& f, std::uint32_t i, std::uint16_t a0, std::uint16_t a1, std::uint16_t a2) {
    f.bus.write16(kOam + i * 8, a0);
    f.bus.write16(kOam + i * 8 + 2, a1);
    f.bus.write16(kOam + i * 8 + 4, a2);
}

// Zeroed OAM means 128 visible 8x8 sprites at (0,0): hide them all first.
void hide_all_sprites(Fixture& f) {
    for (std::uint32_t i = 0; i < 128; ++i) set_sprite(f, i, 0x0200, 0, 0);
}

// Runs until line `y` (counted from the start, so it may be in a later frame) is drawn.
void run_line(Fixture& f, int y) { f.run_to(static_cast<std::uint64_t>(y + 1) * kCyclesPerLine); }

}  // namespace

TEST_CASE("PPU mode 0: a text BG draws 4bpp tiles from the map with its palette bank") {
    Fixture f;
    f.bus.write16(kPal + (2 * 16 + 5) * 2, 0x001F);               // bank 2, color 5 = red
    fill_tile4(f, kVram + 1 * 32, 5);                              // char block 0, tile 1
    f.bus.write16(kVram + 0x800 * 4 + (1 * 32 + 2) * 2, 0x2001);  // map (2,1): tile 1, bank 2
    f.bus.write16(0x04000008, 0x0400);                             // BG0CNT: screen block 4, 4bpp
    f.bus.write16(0x04000000, 0x0100);                             // mode 0, BG0 on
    run_line(f, 8);
    CHECK(f.pixel(15, 8) == 0x0000);  // tile 0 is empty: the backdrop shows
    CHECK(f.pixel(16, 8) == 0x001F);
    CHECK(f.pixel(23, 8) == 0x001F);
    CHECK(f.pixel(24, 8) == 0x0000);
}

TEST_CASE("PPU text BG: scrolling and flipped tiles") {
    Fixture f;
    f.bus.write16(kPal + 1 * 2, 0x7FFF);
    set_pixel4(f, kVram + 32, 0, 0, 1);        // tile 1: only its top-left pixel is set
    f.bus.write16(kVram + 0x800 * 4, 0x0C01);  // map (0,0): tile 1, H and V flip
    f.bus.write16(0x04000008, 0x0400);
    f.bus.write16(0x04000010, 3);  // HOFS
    f.bus.write16(0x04000012, 2);  // VOFS
    f.bus.write16(0x04000000, 0x0100);
    // Flipped, the pixel is at map (7,7); scrolled, on screen at (4,5).
    run_line(f, 5);
    CHECK(f.pixel(4, 5) == 0x7FFF);
    CHECK(f.pixel(3, 5) == 0x0000);
    CHECK(f.pixel(5, 5) == 0x0000);
}

TEST_CASE("PPU text BG 512x256: the right half comes from the next screen block") {
    Fixture f;
    f.bus.write16(kPal + 1 * 2, 0x03E0);
    fill_tile4(f, kVram + 32, 1);
    f.bus.write16(kVram + 0x800 * 5, 0x0001);  // block 5 holds map x 256..511
    f.bus.write16(0x04000008, 0x4400);         // size 1 (512x256), screen block 4
    f.bus.write16(0x04000010, 256);
    f.bus.write16(0x04000000, 0x0100);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == 0x03E0);
    CHECK(f.pixel(8, 0) == 0x0000);
}

TEST_CASE("PPU text BG in 256 colors uses 64-byte tiles and the whole palette") {
    Fixture f;
    f.bus.write16(kPal + 200 * 2, 0x1234);
    fill_tile8(f, kVram + 0x4000 + 1 * 64, 200);
    f.bus.write16(kVram + 0x800 * 4, 0x0001);
    f.bus.write16(0x04000008, 0x0484);  // 256 colors, char block 1, screen block 4
    f.bus.write16(0x04000000, 0x0100);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == 0x1234);
}

TEST_CASE("PPU BG priority: lower value in front, then the lower BG number") {
    Fixture f;
    for (std::uint32_t i = 1; i <= 3; ++i) {
        f.bus.write16(kPal + i * 2, static_cast<std::uint16_t>(i));
        fill_tile4(f, kVram + i * 32, i);
        f.bus.write16(kVram + 0x800 * (9 + i), static_cast<std::uint16_t>(i));  // BG(i-1) map: tile i
    }
    f.bus.write16(0x04000008, 0x0A01);  // BG0: block 10, priority 1
    f.bus.write16(0x0400000A, 0x0B01);  // BG1: block 11, priority 1
    f.bus.write16(0x0400000C, 0x0C02);  // BG2: block 12, priority 2
    f.bus.write16(0x04000000, 0x0700);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == 1);  // BG0 beats BG1 at the same priority, both beat BG2
    f.bus.write16(0x0400000C, 0x0C00);
    run_line(f, 1);
    CHECK(f.pixel(0, 1) == 3);  // BG2 at priority 0
    f.bus.write16(0x04000000, 0x0200);
    run_line(f, 2);
    CHECK(f.pixel(0, 2) == 2);  // only BG1 enabled
}

TEST_CASE("PPU mode 1: BG2 is affine, sampled from the reference point stepping by PA/PC") {
    Fixture f;
    f.bus.write16(kPal + 7 * 2, 0x7C00);
    fill_tile8(f, kVram + 1 * 64, 7);              // affine tiles are always 8bpp
    f.bus.write16(kVram + 0x800 * 8 + 2, 0x0100);  // 1-byte entries: map (3,0) = tile 1
    f.bus.write16(0x0400000C, 0x0800);             // BG2CNT: 128x128, screen block 8
    f.bus.write16(0x04000020, 0x0080);             // PA = 0.5: each texel spans two pixels
    f.bus.write16(0x04000026, 0x0100);             // PD = 1
    f.bus.write16(0x04000000, 0x0401);             // mode 1, BG2 on
    run_line(f, 0);
    CHECK(f.pixel(47, 0) == 0x0000);
    CHECK(f.pixel(48, 0) == 0x7C00);  // texels 24..31 land on x 48..63
    CHECK(f.pixel(63, 0) == 0x7C00);
    CHECK(f.pixel(64, 0) == 0x0000);
}

TEST_CASE("PPU affine BG: wraparound, per-line PB/PD steps and BGxY writes") {
    Fixture f;
    f.bus.write16(kPal + 7 * 2, 0x7C00);
    fill_tile8(f, kVram + 1 * 64, 7);
    f.bus.write16(kVram + 0x800 * 8 + 16, 0x0001);  // map (0,1) = tile 1: texel rows 8..15
    f.bus.write16(0x0400000C, 0x2800);               // wrap on
    f.bus.write16(0x04000020, 0x0100);               // PA = 1
    f.bus.write16(0x04000026, 0x0100);               // PD = 1
    f.bus.write16(0x04000000, 0x0401);
    run_line(f, 7);
    CHECK(f.pixel(0, 7) == 0x0000);  // texel row 7
    run_line(f, 8);
    CHECK(f.pixel(0, 8) == 0x7C00);    // texel row 8
    CHECK(f.pixel(128, 8) == 0x7C00);  // x wrapped around the 128-pixel map
    CHECK(f.pixel(8, 8) == 0x0000);
    // A BG2Y write reloads the internal reference at once.
    f.bus.write32(0x0400002C, static_cast<std::uint32_t>(-256));  // y = -1, wraps to row 127
    run_line(f, 9);
    CHECK(f.pixel(0, 9) == 0x0000);
    f.bus.write32(0x0400002C, 8 * 256);
    run_line(f, 10);
    CHECK(f.pixel(0, 10) == 0x7C00);
}

TEST_CASE("PPU sprites: a regular 4bpp sprite with its palette bank, and the hidden flag") {
    Fixture f;
    hide_all_sprites(f);
    f.bus.write16(kObjPal + (3 * 16 + 1) * 2, 0x03FF);  // OBJ bank 3, color 1
    fill_tile4(f, kObjVram + 5 * 32, 1);
    set_sprite(f, 0, 10, 20, 0x3000 | 5);  // 8x8 at (20,10), tile 5, bank 3
    f.bus.write16(0x04000000, 0x1000);     // mode 0, OBJ on
    run_line(f, 10);
    CHECK(f.pixel(19, 10) == 0x0000);
    CHECK(f.pixel(20, 10) == 0x03FF);
    CHECK(f.pixel(27, 10) == 0x03FF);
    CHECK(f.pixel(28, 10) == 0x0000);
    set_sprite(f, 0, 0x0200 | 10, 20, 0x3000 | 5);
    run_line(f, 11);
    CHECK(f.pixel(20, 11) == 0x0000);
}

TEST_CASE("PPU sprites: X is 9-bit signed and Y wraps at 256") {
    Fixture f;
    hide_all_sprites(f);
    f.bus.write16(kObjPal + 1 * 2, 0x0011);
    fill_tile4(f, kObjVram, 1);
    set_sprite(f, 0, 252, 0x1FC, 0);  // y = 252: rows 4..7 on lines 0..3; x = -4
    f.bus.write16(0x04000000, 0x1000);
    run_line(f, 3);
    CHECK(f.pixel(0, 3) == 0x0011);
    CHECK(f.pixel(3, 3) == 0x0011);
    CHECK(f.pixel(4, 3) == 0x0000);
    run_line(f, 4);
    CHECK(f.pixel(0, 4) == 0x0000);
}

TEST_CASE("PPU sprites: 1D vs 2D tile mapping, and vertical flip") {
    Fixture f;
    hide_all_sprites(f);
    f.bus.write16(kObjPal + 1 * 2, 0x0001);
    f.bus.write16(kObjPal + 2 * 2, 0x0002);
    fill_tile4(f, kObjVram + 2 * 32, 1);   // 1D: a 16x16's second tile row starts at tile 2
    fill_tile4(f, kObjVram + 32 * 32, 2);  // 2D: ... or 32 tiles later
    set_sprite(f, 0, 0x0000, 0x4000, 0);   // square, size 1 = 16x16, at (0,0)
    f.bus.write16(0x04000000, 0x1040);     // 1D
    run_line(f, 8);
    CHECK(f.pixel(0, 8) == 0x0001);
    f.bus.write16(0x04000000, 0x1000);  // 2D
    run_line(f, 9);
    CHECK(f.pixel(0, 9) == 0x0002);
    set_sprite(f, 0, 0x0000, 0x4000 | 0x2000, 0);  // V flip
    run_line(f, 10);  // line 10 shows texture row 5: first tile row, empty
    CHECK(f.pixel(0, 10) == 0x0000);
    run_line(f, kLinesPerFrame + 2);  // next frame, line 2 shows texture row 13
    CHECK(f.pixel(0, 2) == 0x0002);
}

TEST_CASE("PPU sprites: priority against BGs and against each other") {
    Fixture f;
    hide_all_sprites(f);
    f.bus.write16(kPal + 1 * 2, 0x0BB0);  // BG0
    f.bus.write16(kObjPal + 1 * 2, 0x0001);
    f.bus.write16(kObjPal + 2 * 2, 0x0002);
    fill_tile4(f, kVram + 32, 1);
    f.bus.write16(kVram + 0x800 * 4, 0x0001);  // BG0 covers x 0..7 of the first tile row
    f.bus.write16(0x04000008, 0x0401);         // BG0 priority 1
    fill_tile4(f, kObjVram + 1 * 32, 1);
    fill_tile4(f, kObjVram + 2 * 32, 2);
    set_sprite(f, 0, 0, 0, (2U << 10) | 1);  // A: x 0..7, priority 2
    set_sprite(f, 1, 0, 4, (1U << 10) | 2);  // B: x 4..11, priority 1
    f.bus.write16(0x04000000, 0x1100);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == 0x0BB0);  // A is behind BG0
    CHECK(f.pixel(4, 0) == 0x0002);  // B beats A, and wins the tie with BG0
    CHECK(f.pixel(8, 0) == 0x0002);
    set_sprite(f, 0, 0, 0, (1U << 10) | 1);  // A to priority 1 too
    run_line(f, 1);
    CHECK(f.pixel(4, 1) == 0x0001);  // same priority: the lower OAM index wins
}

TEST_CASE("PPU sprites: an affine double-size sprite fills a 2x box around its centre") {
    Fixture f;
    hide_all_sprites(f);
    f.bus.write16(kObjPal + 1 * 2, 0x0055);
    fill_tile4(f, kObjVram, 1);
    set_sprite(f, 0, 0x0300, 0, 0);  // affine + double size: an 8x8 in a 16x16 box, group 0
    // Group 0 is the 4th halfword of OAM entries 0-3: identity.
    f.bus.write16(kOam + 6, 0x0100);
    f.bus.write16(kOam + 14, 0);
    f.bus.write16(kOam + 22, 0);
    f.bus.write16(kOam + 30, 0x0100);
    f.bus.write16(0x04000000, 0x1000);
    run_line(f, 3);
    CHECK(f.pixel(3, 3) == 0x0000);  // inside the box, outside the texture
    run_line(f, 4);
    CHECK(f.pixel(3, 4) == 0x0000);
    CHECK(f.pixel(4, 4) == 0x0055);
    CHECK(f.pixel(11, 4) == 0x0055);
    CHECK(f.pixel(12, 4) == 0x0000);
    f.bus.write16(kOam + 6, 0x0200);  // PA = 2: the 8 texels squeeze into 4 pixels
    run_line(f, 5);
    CHECK(f.pixel(5, 5) == 0x0000);
    CHECK(f.pixel(6, 5) == 0x0055);
    CHECK(f.pixel(9, 5) == 0x0055);
    CHECK(f.pixel(10, 5) == 0x0000);
}

TEST_CASE("PPU sprites in bitmap modes can't use tiles below 512") {
    Fixture f;
    hide_all_sprites(f);
    f.bus.write16(kObjPal + 1 * 2, 0x0077);
    fill_tile4(f, kObjVram, 1);
    fill_tile4(f, kObjVram + 512 * 32, 1);
    set_sprite(f, 0, 0, 0, 0);
    set_sprite(f, 1, 0, 8, 512);
    f.bus.write16(0x04000000, 0x1003);  // mode 3, BG2 off, OBJ on
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == 0x0000);
    CHECK(f.pixel(8, 0) == 0x0077);
}

// --- Windows and color effects (exercise 3.8) ---

namespace {

constexpr std::uint16_t kRed = 0x001F;
constexpr std::uint16_t kBlue = 0x7C00;

// BG0 solid red (priority 0) in front of BG1 solid blue (priority 1), in mode 0. A zeroed
// map points every entry at tile 0, so filling tile 0 fills the whole BG.
void two_solid_bgs(Fixture& f) {
    f.bus.write16(kPal + 1 * 2, kRed);
    f.bus.write16(kPal + 2 * 2, kBlue);
    fill_tile4(f, kVram, 1);           // BG0: char block 0
    fill_tile4(f, kVram + 0x4000, 2);  // BG1: char block 1
    f.bus.write16(0x04000008, 0x0400);  // BG0: screen block 4, priority 0
    f.bus.write16(0x0400000A, 0x0405);  // BG1: same (empty) map, char block 1, priority 1
}

}  // namespace

TEST_CASE("PPU WIN0: layers inside and outside the rectangle") {
    Fixture f;
    two_solid_bgs(f);
    f.bus.write16(0x04000040, (10 << 8) | 20);  // WIN0H: x 10..19
    f.bus.write16(0x04000044, (5 << 8) | 15);   // WIN0V: y 5..14
    f.bus.write16(0x04000048, 0x0002);          // inside WIN0: BG1 only
    f.bus.write16(0x0400004A, 0x0001);          // outside: BG0 only
    f.bus.write16(0x04000000, 0x2300);          // BG0, BG1, WIN0
    run_line(f, 4);
    CHECK(f.pixel(10, 4) == kRed);
    run_line(f, 5);
    CHECK(f.pixel(9, 5) == kRed);
    CHECK(f.pixel(10, 5) == kBlue);
    CHECK(f.pixel(19, 5) == kBlue);
    CHECK(f.pixel(20, 5) == kRed);
    run_line(f, 15);
    CHECK(f.pixel(10, 15) == kRed);
}

TEST_CASE("PPU windows: WIN0 beats WIN1, edges wrap, and a window can hide everything") {
    Fixture f;
    two_solid_bgs(f);
    f.bus.write16(0x04000042, (230 << 8) | 5);  // WIN1H: x 230..239 and 0..4
    f.bus.write16(0x04000046, (0 << 8) | 160);  // WIN1V: every line
    f.bus.write16(0x04000048, 0x0000);          // inside WIN1: nothing (backdrop)
    f.bus.write16(0x0400004A, 0x0002);          // outside: BG1
    f.bus.write16(0x04000000, 0x4300);          // WIN1
    run_line(f, 0);
    CHECK(f.pixel(2, 0) == 0x0000);
    CHECK(f.pixel(232, 0) == 0x0000);
    CHECK(f.pixel(5, 0) == kBlue);
    CHECK(f.pixel(229, 0) == kBlue);
    f.bus.write16(0x04000040, (0 << 8) | 4);    // WIN0H: x 0..3
    f.bus.write16(0x04000044, (0 << 8) | 160);
    f.bus.write16(0x04000048, 0x0001);          // inside WIN0: BG0
    f.bus.write16(0x04000000, 0x6300);          // WIN0 + WIN1
    run_line(f, 1);
    CHECK(f.pixel(2, 1) == kRed);     // in both: WIN0 decides
    CHECK(f.pixel(4, 1) == 0x0000);   // only in WIN1
}

TEST_CASE("PPU OBJ window: opaque pixels of OBJ-window sprites select WINOUT's high byte") {
    Fixture f;
    two_solid_bgs(f);
    hide_all_sprites(f);
    f.bus.write16(kObjPal + 1 * 2, 0x03E0);
    fill_tile4(f, kObjVram, 1);
    set_sprite(f, 0, 0x0800, 50, 0);    // mode 2 (OBJ window), 8x8 at (50,0)
    f.bus.write16(0x0400004A, 0x0201);  // outside: BG0; OBJ window: BG1
    f.bus.write16(0x04000000, 0x9300);  // BG0, BG1, OBJ, OBJ window
    run_line(f, 0);
    CHECK(f.pixel(49, 0) == kRed);
    CHECK(f.pixel(50, 0) == kBlue);  // the sprite itself isn't drawn
    CHECK(f.pixel(57, 0) == kBlue);
    CHECK(f.pixel(58, 0) == kRed);
}

TEST_CASE("PPU alpha blending: 1st target over 2nd target, weighted by EVA/EVB") {
    Fixture f;
    two_solid_bgs(f);
    f.bus.write16(0x04000000, 0x0300);
    f.bus.write16(0x04000050, 0x0001 | (1U << 6) | 0x0200);  // BG0 over BG1, alpha
    f.bus.write16(0x04000052, (8 << 8) | 8);                 // EVA = EVB = 8/16
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == ((15U << 10) | 15U));
    f.bus.write16(0x04000052, (16 << 8) | 16);  // saturates at 31
    run_line(f, 1);
    CHECK(f.pixel(0, 1) == (kRed | kBlue));
    f.bus.write16(0x04000050, 0x0001 | (1U << 6) | 0x0400);  // 2nd target is BG2: not below
    run_line(f, 2);
    CHECK(f.pixel(0, 2) == kRed);
}

TEST_CASE("PPU brighten and darken the 1st target by EVY, unless the window disables effects") {
    Fixture f;
    two_solid_bgs(f);
    f.bus.write16(0x04000000, 0x0300);
    f.bus.write16(0x04000050, 0x0001 | (2U << 6));  // brighten BG0
    f.bus.write16(0x04000054, 8);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == (31U | (15U << 5) | (15U << 10)));
    f.bus.write16(0x04000050, 0x0001 | (3U << 6));  // darken
    run_line(f, 1);
    CHECK(f.pixel(0, 1) == 16);
    f.bus.write16(0x04000054, 31);  // EVY clamps to 16: black
    run_line(f, 2);
    CHECK(f.pixel(0, 2) == 0x0000);
    f.bus.write16(0x04000040, (0 << 8) | 240);  // WIN0 over the whole line
    f.bus.write16(0x04000044, (0 << 8) | 160);
    f.bus.write16(0x04000048, 0x0003);  // BG0 and BG1 but no effects
    f.bus.write16(0x04000000, 0x2300);
    run_line(f, 3);
    CHECK(f.pixel(0, 3) == kRed);
}

TEST_CASE("PPU effects on the backdrop: it can be a 1st target") {
    Fixture f;
    f.bus.write16(0x04000050, 0x0020 | (2U << 6));  // brighten the backdrop
    f.bus.write16(0x04000054, 16);
    f.bus.write16(0x04000000, 0x0000);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == 0x7FFF);
}

TEST_CASE("PPU semi-transparent sprites alpha-blend over a 2nd target regardless of BLDCNT mode") {
    Fixture f;
    two_solid_bgs(f);
    hide_all_sprites(f);
    f.bus.write16(kObjPal + 1 * 2, 0x03E0);  // green
    fill_tile4(f, kObjVram, 1);
    set_sprite(f, 0, 0x0400, 0, 0);     // mode 1 (semi-transparent), priority 0, at (0,0)
    f.bus.write16(0x04000000, 0x1300);
    f.bus.write16(0x04000050, 0x0100);  // no effect, no 1st targets, BG0 is a 2nd target
    f.bus.write16(0x04000052, (8 << 8) | 8);
    run_line(f, 0);
    CHECK(f.pixel(0, 0) == ((15U << 5) | 15U));
    CHECK(f.pixel(8, 0) == kRed);  // outside the sprite
    f.bus.write16(0x04000050, 0x0200);  // only BG1 is a 2nd target: BG0 is below, no blend
    run_line(f, 1);
    CHECK(f.pixel(0, 1) == 0x03E0);
}
