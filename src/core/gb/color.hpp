#pragma once

#include <cstdint>

namespace ezy::gb {

// RGB555 (bits 0-4 red, 5-9 green, 10-14 blue) to 0xAARRGGBB, scaling each 5-bit channel
// to 8 bits by repeating its top bits (so 31 becomes 255 exactly).
[[nodiscard]] constexpr std::uint32_t rgb555_to_argb(std::uint16_t color) {
    const auto expand = [](std::uint32_t c) { return (c << 3) | (c >> 2); };
    return 0xFF000000U | (expand(color & 0x1FU) << 16) | (expand((color >> 5) & 0x1FU) << 8) |
           expand((color >> 10) & 0x1FU);
}

// What a colour looks like on the Game Boy Color's screen. Its LCD mixes the channels a
// little and is less saturated than a modern monitor, and games were tuned for that, so
// raw RGB555 looks garish. This is the channel-mixing matrix from byuu/Near's higan:
//   R = 26r + 4g + 2b,  G = 24g + 8b,  B = 6r + 4g + 22b   (each row sums to 32)
// divided by 32 instead of clamped, so white stays white and black stays black.
[[nodiscard]] constexpr std::uint32_t correct_cgb_color(std::uint16_t color) {
    const std::uint32_t r = color & 0x1FU;
    const std::uint32_t g = (color >> 5) & 0x1FU;
    const std::uint32_t b = (color >> 10) & 0x1FU;
    // Mixed values are in 0..31*32; scale to 0..255 with rounding.
    const auto scale = [](std::uint32_t mixed) { return (mixed * 255 + (31 * 32) / 2) / (31 * 32); };
    return 0xFF000000U | (scale(26 * r + 4 * g + 2 * b) << 16) | (scale(24 * g + 8 * b) << 8) |
           scale(6 * r + 4 * g + 22 * b);
}

}  // namespace ezy::gb
