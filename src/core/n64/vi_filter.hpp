#pragma once

#include "n64/rdp_hidden_bits.hpp"

#include <cstdint>
#include <span>

// The N64 VI's output filter stages as pure functions over an RDRAM image: coverage fetch,
// antialias, dither filter, divot, interpolation and gamma. Vi::output() chains them.
namespace ezy::n64::vi {

struct Color { std::uint8_t r = 0, g = 0, b = 0; std::uint8_t coverage = 7; };

// A view of the image VI_ORIGIN points at.
struct Framebuffer {
    std::span<const std::uint8_t> rdram;
    const RdpHiddenBits* hidden = nullptr; // nullptr: hidden bits read as 0
    std::uint32_t origin = 0, width = 0;   // VI_ORIGIN & 0xFFFFFF, VI_WIDTH & 0xFFF (pixels)
    bool rgba32 = false;
    bool use_coverage = true;              // AA_MODE 0 or 1; otherwise coverage reads as 7
};

// Pixel (x, y) with its coverage; outside RDRAM: black with coverage 7.
Color fetch(const Framebuffer& fb, int x, int y);
// Antialias (coverage < 7) or dither filter (coverage == 7 and dither_filter), as the hardware's fetch does.
Color fetch_filtered(const Framebuffer& fb, int x, int y, bool dither_filter);
Color divot(Color center, Color left, Color right);
Color lerp(Color a, Color b, unsigned fraction); // fraction 0..31
Color gamma(Color c, bool gamma, bool gamma_dither, std::uint32_t noise);

} // namespace ezy::n64::vi
