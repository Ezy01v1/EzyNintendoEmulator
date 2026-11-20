#include <doctest/doctest.h>
#include "n64/n64.hpp"
#include "n64/vi_filter.hpp"
#include "n64_test_helpers.hpp"
#include <memory>
#include <vector>

using namespace ezy::n64;
using namespace ezy::n64::vi;

namespace {

// VI registers in a real N64, written through the bus (register i at 0x04400000 + 4 * i).
enum ViReg : std::uint32_t { kCtrl, kOrigin, kWidth, kVIntr, kVCurrent, kBurst, kVSync, kHSync, kHSyncLeap,
                             kHVideo, kVVideo, kVBurst, kXScale, kYScale };
void vi_write(Bus& bus, ViReg reg, std::uint32_t value) { bus.write32(0x04400000 + 4 * reg, value); }

constexpr std::uint32_t kBlack = 0xFF000000;
constexpr std::uint32_t kFb = 0x100000;   // framebuffer address, away from the ROM's counter at 0x100

std::unique_ptr<N64> make_n64(char region_code = 'E') {
    auto rom = n64_test::counting_rom("VI OUTPUT");
    rom[0x3E] = static_cast<std::uint8_t>(region_code);
    return N64::create(rom);
}

// The usual NTSC 320x240 16-bit setup: H_VIDEO 108..748 (screen columns 0..639), V_VIDEO
// 37..511 (field lines 1..237), X_SCALE 0.5, Y_SCALE 1.0.
void program_320x240(Bus& bus, std::uint32_t ctrl, std::uint32_t origin = kFb) {
    vi_write(bus, kCtrl, ctrl);
    vi_write(bus, kOrigin, origin);
    vi_write(bus, kWidth, 320);
    vi_write(bus, kHVideo, 0x006C02EC);
    vi_write(bus, kVVideo, 0x002501FF);
    vi_write(bus, kXScale, 0x200);
    vi_write(bus, kYScale, 0x400);
}

void put16(Bus& bus, std::uint32_t origin, std::uint32_t width, int x, int y, unsigned r5, unsigned g5, unsigned b5) {
    const std::uint32_t address = origin + (static_cast<std::uint32_t>(y) * width + static_cast<std::uint32_t>(x)) * 2;
    bus.write16(address, static_cast<std::uint16_t>((r5 << 11) | (g5 << 6) | (b5 << 1) | 1));
}

// The output colour of a 16-bit pixel without filtering: each 5-bit channel << 3.
constexpr std::uint32_t argb16(unsigned r5, unsigned g5, unsigned b5) {
    return 0xFF000000 | ((r5 << 3) << 16) | ((g5 << 3) << 8) | (b5 << 3);
}

std::uint32_t pixel(const Vi::Image& img, int x, int y) {
    return img.argb[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)];
}

bool all_black(const Vi::Image& img) {
    for (const auto p : img.argb) if (p != kBlack) return false;
    return true;
}

struct Image {
    std::vector<std::uint8_t> rdram = std::vector<std::uint8_t>(64 * 1024);
    RdpHiddenBits hidden;
    std::uint32_t origin = 0, width = 8;

    Framebuffer view(bool rgba32 = false, bool use_coverage = true) const {
        Framebuffer fb;
        fb.rdram = rdram;
        fb.hidden = &hidden;
        fb.origin = origin;
        fb.width = width;
        fb.rgba32 = rgba32;
        fb.use_coverage = use_coverage;
        return fb;
    }
    // A 16-bit pixel from 5-bit channels; coverage bit 2 goes to pixel bit 0, bits 1..0 to the hidden bits.
    void put16(int x, int y, unsigned r5, unsigned g5, unsigned b5, unsigned coverage) {
        const std::uint32_t address = origin + (static_cast<std::uint32_t>(y) * width + static_cast<std::uint32_t>(x)) * 2;
        const unsigned pixel = (r5 << 11) | (g5 << 6) | (b5 << 1) | ((coverage >> 2) & 1);
        rdram[address] = static_cast<std::uint8_t>(pixel >> 8);
        rdram[address + 1] = static_cast<std::uint8_t>(pixel);
        hidden.write(address >> 1, coverage & 3);
    }
};

} // namespace

TEST_CASE("VI fetch expands 16-bit pixels and reads coverage from bit 0 and the hidden bits") {
    Image img;
    img.origin = 0x100;
    img.width = 4;
    // (1,0): R=31, G=0, B=16, bit 0 = 1, hidden = 1 -> coverage (1 << 2) | 1 = 5.
    img.put16(1, 0, 31, 0, 16, 5);
    const Color c = fetch(img.view(), 1, 0);
    CHECK(c.r == (0x1F << 3));   // 31 << 3 = 0xF8
    CHECK(c.g == 0);
    CHECK(c.b == (16 << 3));     // 0x80
    CHECK(c.coverage == 5);
    // A 32-bit pixel (0,2): bytes R, G, B, A; coverage (0xA0 >> 5) & 7 = 5.
    const std::uint32_t at = 0x100 + 4 * 2 * 4;
    img.rdram[at] = 10; img.rdram[at + 1] = 20; img.rdram[at + 2] = 30; img.rdram[at + 3] = 0xA0;
    const Color d = fetch(img.view(true), 0, 2);
    CHECK(d.r == 10); CHECK(d.g == 20); CHECK(d.b == 30);
    CHECK(d.coverage == 5);
}

TEST_CASE("VI fetch ignores coverage outside AA modes 0 and 1") {
    Image img;
    img.put16(2, 1, 1, 2, 3, 2);
    CHECK(fetch(img.view(false, true), 2, 1).coverage == 2);
    CHECK(fetch(img.view(false, false), 2, 1).coverage == 7);
    CHECK(fetch(img.view(true, false), 0, 0).coverage == 7);
}

TEST_CASE("VI fetch outside RDRAM is black with full coverage") {
    Image img;
    const Color past = fetch(img.view(), 100000, 0);   // 200000 bytes in a 64 KiB image
    CHECK(past.coverage == 7); CHECK(past.r == 0); CHECK(past.g == 0); CHECK(past.b == 0);
    const Color before = fetch(img.view(), 0, -1);     // negative address
    CHECK(before.coverage == 7); CHECK(before.r == 0);
}

TEST_CASE("VI antialias blends a partly covered pixel toward its penultimate neighbours") {
    Image img;
    // Centre (3,1): R = 0, coverage 3. Six full-coverage neighbours with R 5-bit 2,4,6,8,10,16,
    // i.e. 0x10,0x20,0x30,0x40,0x50,0x80 after << 3 (the outlier tells second-from-the-end from plain min/max).
    img.put16(3, 1, 0, 0, 0, 3);
    img.put16(2, 0, 2, 0, 0, 7);
    img.put16(4, 0, 4, 0, 0, 7);
    img.put16(1, 1, 6, 0, 0, 7);
    img.put16(5, 1, 8, 0, 0, 7);
    img.put16(2, 2, 10, 0, 0, 7);
    img.put16(4, 2, 16, 0, 0, 7);
    const Color c = fetch_filtered(img.view(), 3, 1, false);
    // Set {0x00,0x10,0x20,0x30,0x40,0x50,0x80}: penmin 0x10, penmax 0x50 (second from each end);
    // ((0x10 + 0x50 - 0) * (7 - 3) + 4) >> 3 = (0x60 * 4 + 4) >> 3 = 388 >> 3 = 48 = 0x30.
    // (Plain min/max would give (0x80 * 4 + 4) >> 3 = 64.)
    CHECK(c.r == 0x30);
    CHECK(c.g == 0);   // all-zero channel stays zero
    CHECK(c.coverage == 3);
}

TEST_CASE("VI antialias leaves a pixel alone with fewer than two full neighbours") {
    Image img;
    img.put16(3, 1, 5, 6, 7, 3);
    img.put16(2, 0, 20, 20, 20, 7);   // the only full-coverage neighbour; the rest read coverage 0
    const Color centre = fetch(img.view(), 3, 1);
    const Color c = fetch_filtered(img.view(), 3, 1, false);
    // penmin = penmax = centre -> (0 * 4 + 4) >> 3 = 0, so the result is the centre.
    CHECK(c.r == centre.r); CHECK(c.g == centre.g); CHECK(c.b == centre.b);
}

TEST_CASE("VI dither filter nudges each channel toward its neighbours by up to 8") {
    Image img;
    const int offsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    img.put16(3, 1, 10, 10, 10, 7);
    for (const auto& o : offsets) img.put16(3 + o[0], 1 + o[1], 20, 5, 10, 7);
    const Color centre = fetch(img.view(), 3, 1);   // 10 << 3 = 80 per channel
    const Color c = fetch_filtered(img.view(), 3, 1, true);
    CHECK(c.r == centre.r + 8);   // all 8 neighbours brighter in 5 bits: 80 + 8 = 88
    CHECK(c.g == centre.g - 8);   // all 8 darker: 80 - 8 = 72
    CHECK(c.b == centre.b);       // equal: +0
    // Filter off: untouched.
    CHECK(fetch_filtered(img.view(), 3, 1, false).r == centre.r);
    // Mixed: 3 brighter, 2 darker, 3 equal in R -> 80 + 3 - 2 = 81 (one neighbour is 31 > 10 too:
    // brighter are 11, 12, 31 = 3; darker are 9, 0 = 2; equal are the three 10s).
    Image mix;
    mix.put16(3, 1, 10, 0, 0, 7);
    mix.put16(2, 0, 11, 0, 0, 7); mix.put16(3, 0, 12, 0, 0, 7); mix.put16(4, 0, 31, 0, 0, 7);
    mix.put16(2, 1, 9, 0, 0, 7);  mix.put16(4, 1, 0, 0, 0, 7);
    mix.put16(2, 2, 10, 0, 0, 7); mix.put16(3, 2, 10, 0, 0, 7); mix.put16(4, 2, 10, 0, 0, 7);
    CHECK(fetch_filtered(mix.view(), 3, 1, true).r == 81);
}

TEST_CASE("VI dither filter compares only the top five bits of each channel") {
    Image img;
    img.width = 8;
    auto put32 = [&](int x, int y, unsigned r) {
        const std::uint32_t a = (static_cast<std::uint32_t>(y) * 8 + static_cast<std::uint32_t>(x)) * 4;
        img.rdram[a] = static_cast<std::uint8_t>(r); img.rdram[a + 1] = 0; img.rdram[a + 2] = 0; img.rdram[a + 3] = 0xE0;
    };
    // Centre R = 0x87 (5 bits: 0x10); every neighbour R = 0x80 (5 bits: 0x10): equal in 5 bits although
    // 8 bits differ, so no nudge: result 0x87. (An 8-bit comparison would subtract 8 -> 0x7F.)
    put32(3, 1, 0x87);
    const int offsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    for (const auto& o : offsets) put32(3 + o[0], 1 + o[1], 0x80);
    CHECK(fetch_filtered(img.view(true), 3, 1, true).r == 0x87);
}

TEST_CASE("VI dither filter is skipped on partly covered pixels, which use antialiasing instead") {
    Image img;
    img.put16(3, 1, 10, 0, 0, 6);
    for (int dx = -1; dx <= 1; ++dx) { img.put16(3 + dx, 0, 20, 0, 0, 7); img.put16(3 + dx, 2, 20, 0, 0, 7); }
    // Full neighbours: (2,0),(4,0),(2,2),(4,2) = 4, all 0xA0; centre 0x50, coverage 6.
    // Set {0x50, 0xA0 x4}: penmin = penmax = 0xA0: ((0x140 - 0xA0) * 1 + 4) >> 3 = (0xA0 + 4) >> 3 = 20;
    // 0x50 + 20 = 100.
    CHECK(fetch_filtered(img.view(), 3, 1, true).r == 100);
}

TEST_CASE("VI divot takes the median only on edges") {
    Color centre{0x50, 0x50, 0x50, 7}, left{0x20, 0x90, 0x50, 7}, right{0x10, 0xA0, 0x60, 7};
    CHECK(divot(centre, left, right).r == centre.r);   // all coverage 7: unchanged
    centre.coverage = 3;
    const Color m = divot(centre, left, right);
    CHECK(m.r == 0x20);   // median of {0x50, 0x20, 0x10}
    CHECK(m.g == 0x90);   // {0x50, 0x90, 0xA0}
    CHECK(m.b == 0x50);   // {0x50, 0x50, 0x60}
}

TEST_CASE("VI lerp uses five-bit fractions with rounding") {
    const Color zero{0, 0, 0, 7}, full{0xFF, 0xFF, 0xFF, 7};
    // ((0xFF - 0) * 16 + 16) >> 5 = 4096 >> 5 = 128 = 0x80.
    CHECK(lerp(zero, full, 16).r == 0x80);
    // Descending: ((0 - 255) * 16 + 16) >> 5 = -4064 >> 5 = -127; + 255 = 128.
    CHECK(lerp(full, zero, 16).g == 128);
    const Color a{12, 34, 56, 7};
    CHECK(lerp(a, full, 0).r == a.r);
    CHECK(lerp(a, full, 0).b == a.b);
    // Fraction 31 of 0 -> 0xFF: (255 * 31 + 16) >> 5 = 7921 >> 5 = 247.
    CHECK(lerp(zero, full, 31).r == 247);
}

TEST_CASE("VI gamma follows twice the integer square root") {
    const Color c{0x80, 0x00, 0xFF, 7};
    // isqrt(0x80 << 6 = 8192) = 90 -> 180; isqrt(0) = 0; isqrt(255 << 6 = 16320) = 127 -> 254.
    const Color g = gamma(c, true, false, 0);
    CHECK(g.r == 2 * 90); CHECK(g.g == 0); CHECK(g.b == 254);
    // No gamma, no dither: identity.
    CHECK(gamma(c, false, false, 7).r == 0x80);
    // Dither only: noise bits 0,1,2 add 1 to R,G,B (not to 255). noise = 0b101: R +1, G +0, B stays 255.
    const Color d = gamma(Color{10, 20, 255, 7}, false, true, 5);
    CHECK(d.r == 11); CHECK(d.g == 20); CHECK(d.b == 255);
    // Gamma + dither with c = 0, noise = 0xFFF: R d = noise & 0x3F = 63 -> 2 * isqrt(63) = 14;
    // G d = (noise >> 6) & 0x3F = 63 -> 14; B d = ((noise >> 9) & 0x38) | (noise & 7) = 0 | 7 = 7 -> 2 * isqrt(7) = 4.
    const Color e = gamma(Color{0, 0, 0, 7}, true, true, 0xFFF);
    CHECK(e.r == 14); CHECK(e.g == 14); CHECK(e.b == 4);
}

TEST_CASE("VI output is 640x480 black when the VI is off") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x0200);   // type 0: blank
    put16(bus, kFb, 320, 0, 0, 31, 31, 31);
    const Vi::Image off = n64->output();
    CHECK(off.width == 640);
    CHECK(off.height == 480);
    CHECK(off.argb.size() == 640u * 480u);
    CHECK(all_black(off));
    vi_write(bus, kCtrl, 0x0201);   // type 1: reserved, also blank
    CHECK(all_black(n64->output()));
    // Type 2 with an empty window (H_END <= H_START): black too.
    vi_write(bus, kCtrl, 0x0202);
    vi_write(bus, kHVideo, 0x02EC006C);
    const Vi::Image empty = n64->output();
    CHECK(empty.height == 480);
    CHECK(all_black(empty));
    // A PAL console: 640x576.
    const auto pal = make_n64('P');
    REQUIRE(pal);
    const Vi::Image pal_off = pal->output();
    CHECK(pal_off.width == 640);
    CHECK(pal_off.height == 576);
    CHECK(all_black(pal_off));
}

TEST_CASE("VI output places the visible window and blacks out the rest") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    // Type 2, AA mode 2 (resample only, no coverage), no divot, no gamma.
    program_320x240(bus, 0x0202);
    put16(bus, kFb, 320, 10, 5, 31, 8, 2);
    put16(bus, kFb, 320, 0, 236, 31, 31, 31);
    put16(bus, kFb, 320, 0, 237, 31, 31, 31);
    const Vi::Image img = n64->output();
    REQUIRE(img.width == 640);
    REQUIRE(img.height == 480);
    // V_VIDEO 0x25..0x1FF: first field line (37 - 34) / 2 = 1, vres (511 - 37) >> 1 = 237, so
    // field lines 1..237 = output rows 2..475; rows 0, 1 and 476.. are black.
    CHECK(pixel(img, 0, 0) == kBlack);
    CHECK(pixel(img, 0, 1) == kBlack);
    // Framebuffer (10, 5): field line 1 + 5 = 6 -> rows 12, 13; xoffs = 20 * 512 -> col 10, xfrac 0.
    // R 31 << 3 = 0xF8, G 8 << 3 = 0x40, B 2 << 3 = 0x10.
    CHECK(pixel(img, 20, 12) == 0xFFF84010);
    CHECK(pixel(img, 20, 13) == 0xFFF84010);
    CHECK(pixel(img, 20, 11) == kBlack);   // framebuffer row 4 is zero
    // Framebuffer row 236 is field line 237 (rows 474, 475), the last inside; row 237 would be line 238.
    CHECK(pixel(img, 0, 474) == argb16(31, 31, 31));
    CHECK(pixel(img, 0, 476) == kBlack);

    // A window starting at H 128: screen column 128 - 108 = 20, so columns 0..19 are black.
    vi_write(bus, kHVideo, 0x008002EC);
    const Vi::Image right = n64->output();
    CHECK(pixel(right, 19, 12) == kBlack);
    // Column 20 + 20 = 40 reads xoffs 20 * 512 -> framebuffer col 10.
    CHECK(pixel(right, 40, 12) == 0xFFF84010);
    CHECK(pixel(right, 20, 474) == argb16(31, 31, 31));

    // A window starting at H 96: column 96 - 108 = -12 is clipped and x_start += 512 * 12 = 6144,
    // so screen column 0 reads xoffs 6144 -> col 6 and column 8 reads 6144 + 8 * 512 = 10240 -> col 10.
    vi_write(bus, kHVideo, 0x006002EC);
    const Vi::Image left = n64->output();
    CHECK(pixel(left, 8, 12) == 0xFFF84010);
    CHECK(pixel(left, 6, 12) == kBlack);   // xoffs 6144 + 3072 = 9216 -> col 9, zero
}

TEST_CASE("VI output doubles lines without interlace and weaves fields with it") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x0202);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) put16(bus, kFb, 320, x, y, static_cast<unsigned>(x + y), 3, 7);
    const Vi::Image progressive = n64->output();
    // Without interlace each field line goes to rows 2y and 2y + 1.
    bool doubled = true;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 640; ++x) doubled = doubled && pixel(progressive, x, 2 * y) == pixel(progressive, x, 2 * y + 1);
    CHECK(doubled);
    CHECK(pixel(progressive, 4, 4) == argb16(3, 3, 7));   // field line 2 = fb row 1, col 2: R 2 + 1 = 3

    // Interlaced (CTRL bit 6), field 0: field line y goes to row 2y; with no earlier field the other rows are black.
    vi_write(bus, kCtrl, 0x0242);
    REQUIRE((bus.read32(0x04400010) & 1) == 0);
    const Vi::Image field0 = n64->output();
    CHECK(pixel(field0, 4, 4) == argb16(3, 3, 7));
    CHECK(pixel(field0, 4, 5) == kBlack);

    // Next field (the VI toggles the field at the end of each interlaced field), with a new framebuffer
    // content: field 1 goes to rows 2y + 1 and the rows of field 0 are kept.
    n64->run_frame();
    REQUIRE((bus.read32(0x04400010) & 1) == 1);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) put16(bus, kFb, 320, x, y, 20, static_cast<unsigned>(x), 0);
    const Vi::Image field1 = n64->output();
    CHECK(pixel(field1, 4, 4) == argb16(3, 3, 7));    // the kept field 0 row 4
    CHECK(pixel(field1, 4, 5) == argb16(20, 2, 0));   // field line 2, col 2: G = 2
    CHECK(pixel(field1, 0, 0) == kBlack);              // field line 0 is outside the window in both fields
}

TEST_CASE("VI output interpolates a 320-wide image to 640") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x0202);   // AA mode 2: resample
    put16(bus, kFb, 320, 0, 0, 4, 0, 0);    // a: R 4 << 3 = 32
    put16(bus, kFb, 320, 1, 0, 20, 0, 0);   // b: R 20 << 3 = 160
    put16(bus, kFb, 320, 0, 1, 8, 0, 0);    // c: R 8 << 3 = 64
    const Vi::Image img = n64->output();
    // Column 1: xoffs 512 -> col 0, xfrac (512 >> 5) & 31 = 16; yfrac 0.
    // ((160 - 32) * 16 + 16) >> 5 = 2064 >> 5 = 64; 32 + 64 = 96 = (32 + 160 + 1) / 2.
    CHECK(((pixel(img, 1, 2) >> 16) & 0xFF) == (32 + 160 + 1) / 2);
    CHECK(pixel(img, 0, 2) == argb16(4, 0, 0));    // xfrac 0: a itself
    CHECK(pixel(img, 2, 2) == argb16(20, 0, 0));   // xoffs 1024: b itself

    // Y_SCALE 0.5: field line 2 is window line 1: curry 512 -> row 0, yfrac 16. Column 0 (xfrac 0):
    // vertical lerp of a and c: ((64 - 32) * 16 + 16) >> 5 = 528 >> 5 = 16; 32 + 16 = 48.
    vi_write(bus, kYScale, 0x200);
    const Vi::Image half = n64->output();
    CHECK(pixel(half, 0, 4) == (0xFF000000u | (48u << 16)));
    // Column 1, both fractions 16: left = 48, right = lerp(160, 0, 16) = ((0 - 160) * 16 + 16) >> 5 = -2544 >> 5
    // = -80 -> 80; horizontal ((80 - 48) * 16 + 16) >> 5 = 528 >> 5 = 16 -> 64.
    CHECK(pixel(half, 1, 4) == (0xFF000000u | (64u << 16)));
}

TEST_CASE("VI output replicates without interpolation in AA mode 3") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x0302);   // AA mode 3
    put16(bus, kFb, 320, 0, 0, 4, 0, 0);
    put16(bus, kFb, 320, 1, 0, 20, 0, 0);
    const Vi::Image img = n64->output();
    // Column 1 has xfrac 16, but mode 3 takes (col 0, row 0) as is.
    CHECK(pixel(img, 1, 2) == pixel(img, 0, 2));
    CHECK(pixel(img, 1, 2) == argb16(4, 0, 0));
    CHECK(pixel(img, 2, 2) == argb16(20, 0, 0));
}

TEST_CASE("VI output applies gamma when CTRL bit 3 is set") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x020A);   // type 2, gamma, AA mode 2
    put16(bus, kFb, 320, 0, 0, 16, 0, 31);
    // R 16 << 3 = 128: 2 * isqrt(128 << 6 = 8192) = 2 * 90 = 180 = 0xB4; G 0 -> 0;
    // B 31 << 3 = 248: 2 * isqrt(248 << 6 = 15872) = 2 * 125 = 250 = 0xFA (125^2 = 15625, 126^2 = 15876).
    CHECK(pixel(n64->output(), 0, 2) == 0xFFB400FA);
}

TEST_CASE("VI output never reads outside RDRAM") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    // 32-bit, AA mode 0 (coverage, antialias), divot, dither filter: every stage reads neighbours.
    vi_write(bus, kCtrl, 0x00010013);
    vi_write(bus, kOrigin, 0x7FFF00);
    vi_write(bus, kWidth, 640);
    vi_write(bus, kHVideo, 0x006C02EC);
    vi_write(bus, kVVideo, 0x002501FF);
    vi_write(bus, kXScale, 0x400);
    vi_write(bus, kYScale, 0x400);
    Vi::Image img;
    CHECK_NOTHROW(img = n64->output());
    REQUIRE(img.argb.size() == 640u * 480u);
    // Framebuffer row 0 holds (0x800000 - 0x7FFF00) / 4 = 64 pixels inside RDRAM; column 100 is past the end:
    // black (coverage 7), its row neighbours too, and the row above (inside RDRAM, zero) does not nudge it.
    CHECK(pixel(img, 100, 2) == kBlack);
    CHECK(pixel(img, 639, 475) == kBlack);
}

TEST_CASE("VI output follows a mode change on the next frame") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x0202);
    put16(bus, kFb, 320, 0, 0, 31, 31, 31);
    CHECK(pixel(n64->output(), 0, 2) == argb16(31, 31, 31));

    // 32-bit at another address with a window starting at screen column 20.
    constexpr std::uint32_t kFb32 = 0x200000;
    vi_write(bus, kCtrl, 0x0203);
    vi_write(bus, kOrigin, kFb32);
    vi_write(bus, kHVideo, 0x008002EC);
    bus.write32(kFb32, 0x11223300);   // R 0x11, G 0x22, B 0x33
    const Vi::Image next = n64->output();
    CHECK(next.width == 640);
    CHECK(next.height == 480);
    CHECK(pixel(next, 0, 2) == kBlack);
    CHECK(pixel(next, 20, 2) == 0xFF112233);

    // Interlaced 32-bit, field 0, then a change back to 16 bits for field 1: the woven rows of the old
    // mode are dropped, so the field 0 rows (2y) are black instead of the 32-bit picture.
    vi_write(bus, kCtrl, 0x0243);
    CHECK(pixel(n64->output(), 20, 2) == 0xFF112233);
    n64->run_frame();
    REQUIRE((bus.read32(0x04400010) & 1) == 1);
    vi_write(bus, kCtrl, 0x0242);
    vi_write(bus, kOrigin, kFb);
    const Vi::Image woven = n64->output();
    CHECK(pixel(woven, 20, 2) == kBlack);                // dropped
    CHECK(pixel(woven, 20, 3) == argb16(31, 31, 31));    // field 1: fb (0, 0)
}

TEST_CASE("VI output takes the divot median of a partly covered pixel") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    // 32-bit, AA mode 0, divot (CTRL bit 4), 640 wide at scale 1.0: screen column x is framebuffer column x.
    vi_write(bus, kCtrl, 0x0013);
    vi_write(bus, kOrigin, kFb);
    vi_write(bus, kWidth, 640);
    vi_write(bus, kHVideo, 0x006C02EC);
    vi_write(bus, kVVideo, 0x002501FF);
    vi_write(bus, kXScale, 0x400);
    vi_write(bus, kYScale, 0x400);
    // Row 0: centre (10, 0) R 0x80 with coverage 3 (alpha 0x60) between R 0x20 and R 0x10 at coverage 7.
    // Every other pixel is zero (coverage 0), so the centre has no full neighbour for antialiasing and
    // keeps R 0x80 after it; divot then takes median(0x20, 0x80, 0x10) = 0x20.
    bus.write32(kFb + 9 * 4, 0x200000E0);
    bus.write32(kFb + 10 * 4, 0x80000060);
    bus.write32(kFb + 11 * 4, 0x100000E0);
    // V_VIDEO 0x25: framebuffer row 0 is field line 1, output rows 2 and 3.
    CHECK(pixel(n64->output(), 10, 2) == 0xFF200000);
    vi_write(bus, kCtrl, 0x0003);   // divot off: the centre as it is
    CHECK(pixel(n64->output(), 10, 2) == 0xFF800000);
}

TEST_CASE("VI output clips a window larger than the screen") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    // 16-bit, AA mode 0, divot: H_VIDEO 108..1023 is 915 pixels (the screen has 640), V_VIDEO 37..1000
    // is 481 field lines (a field has 240).
    vi_write(bus, kCtrl, 0x0012);
    vi_write(bus, kOrigin, kFb);
    vi_write(bus, kWidth, 1024);
    vi_write(bus, kHVideo, 0x006C03FF);
    vi_write(bus, kVVideo, 0x002503E8);
    vi_write(bus, kXScale, 0x400);
    vi_write(bus, kYScale, 0x400);
    put16(bus, kFb, 1024, 639, 238, 31, 0, 0);
    Vi::Image img;
    CHECK_NOTHROW(img = n64->output());
    REQUIRE(img.width == 640);
    REQUIRE(img.height == 480);
    REQUIRE(img.argb.size() == 640u * 480u);
    // Without the filters (AA mode 2), framebuffer (639, 238) shows: field line 1 + 238 = 239, the last
    // one, rows 478 and 479. (With divot its zero neighbours make it black.)
    vi_write(bus, kCtrl, 0x0202);
    CHECK_NOTHROW(img = n64->output());
    CHECK(pixel(img, 639, 479) == argb16(31, 0, 0));
}

TEST_CASE("VI output does not weave a field from before a load_state") {
    const auto n64 = make_n64();
    REQUIRE(n64);
    Bus& bus = n64->bus();
    program_320x240(bus, 0x0242);   // interlaced 16-bit, AA mode 2
    put16(bus, kFb, 320, 0, 0, 31, 31, 31);
    REQUIRE((bus.read32(0x04400010) & 1) == 0);
    const std::vector<std::uint8_t> state = n64->save_state();
    // Field 0 fills the weave cache: row 2 is fb (0, 0).
    CHECK(pixel(n64->output(), 0, 2) == argb16(31, 31, 31));
    REQUIRE(n64->load_state(state));
    n64->run_frame();
    REQUIRE((bus.read32(0x04400010) & 1) == 1);
    const Vi::Image field1 = n64->output();
    CHECK(pixel(field1, 0, 3) == argb16(31, 31, 31));   // field 1: fb (0, 0)
    CHECK(pixel(field1, 0, 2) == kBlack);               // field 0's rows came from before the load: dropped
}
