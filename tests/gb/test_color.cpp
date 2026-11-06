#include <doctest/doctest.h>

#include <cstdint>

#include "gb/color.hpp"

using ezy::gb::correct_cgb_color;
using ezy::gb::rgb555_to_argb;

TEST_CASE("colour correction keeps white and black") {
    CHECK(correct_cgb_color(0x7FFF) == 0xFFFFFFFF);
    CHECK(correct_cgb_color(0x0000) == 0xFF000000);
}

TEST_CASE("colour correction desaturates pure red like the GBC screen") {
    const std::uint32_t red = correct_cgb_color(0x001F);
    const auto r = (red >> 16) & 0xFF;
    const auto g = (red >> 8) & 0xFF;
    const auto b = red & 0xFF;
    CHECK(r < 255);
    CHECK(g == 0);  // green only mixes in green and blue
    CHECK(b > 0);
    CHECK(r > g);
    CHECK(r > b);
}

TEST_CASE("plain conversion repeats the top bits") {
    CHECK(rgb555_to_argb(0x7FFF) == 0xFFFFFFFF);
    CHECK(rgb555_to_argb(0x001F) == 0xFFFF0000);
    CHECK(rgb555_to_argb(0x7C00) == 0xFF0000FF);
}
