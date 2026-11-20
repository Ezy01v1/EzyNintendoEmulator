#include <doctest/doctest.h>
#include "n64/vi_filter.hpp"
#include <vector>

using namespace ezy::n64;
using namespace ezy::n64::vi;

namespace {

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
