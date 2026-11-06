#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "gb/compat_palettes.hpp"
#include "test_helpers.hpp"

using namespace ezy::gb;

namespace {

using Colors = std::array<std::uint16_t, 4>;

CartridgeHeader nintendo_header(std::string_view title) {
    auto rom = test::make_rom(title);
    rom[kOldLicenseeAddr] = 0x01;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return *parse_header(rom);
}

}  // namespace

TEST_CASE("Nintendo title with a known checksum gets its palette") {
    const CompatPalette p = select_compat_palette(nintendo_header("POKEMON RED"), {});
    CHECK(p.obj0 == Colors{0x7FFF, 0x1BEF, 0x0200, 0x0000});
    CHECK(p.obj1 == Colors{0x7FFF, 0x421F, 0x1CF2, 0x0000});
    CHECK(p.bg == Colors{0x7FFF, 0x421F, 0x1CF2, 0x0000});
}

TEST_CASE("a shared checksum is told apart by the 4th letter") {
    const CompatPalette blue = select_compat_palette(nintendo_header("POKEMON BLUE"), {});
    CHECK(blue.bg == Colors{0x7FFF, 0x7E8C, 0x7C00, 0x0000});
    const CompatPalette vegas = select_compat_palette(nintendo_header("VEGAS STAKES"), {});
    CHECK(vegas.bg == Colors{0x7FFF, 0x1BEF, 0x0200, 0x0000});
}

TEST_CASE("non-Nintendo licensee and unknown titles get the default palette") {
    const CompatPalette expected{{0x7FFF, 0x1BEF, 0x6180, 0x0000},
                                 {0x7FFF, 0x421F, 0x1CF2, 0x0000},
                                 {0x7FFF, 0x421F, 0x1CF2, 0x0000}};
    auto rom = test::make_rom("POKEMON RED");  // licensee 0x00
    const CompatPalette other = select_compat_palette(*parse_header(rom), {});
    CHECK(other.bg == expected.bg);
    CHECK(other.obj0 == expected.obj0);
    const CompatPalette unknown = select_compat_palette(nintendo_header("NOT A REAL GAME"), {});
    CHECK(unknown.bg == expected.bg);
}

TEST_CASE("Left + A at power-on overrides the choice") {
    const CompatPalette p = select_compat_palette(nintendo_header("POKEMON RED"), {.left = true, .a = true});
    CHECK(p.obj0 == Colors{0x7FFF, 0x421F, 0x1CF2, 0x0000});
    CHECK(p.obj1 == Colors{0x7FFF, 0x32BF, 0x00D0, 0x0000});
    CHECK(p.bg == Colors{0x7FFF, 0x6E31, 0x454A, 0x0000});
}

TEST_CASE("buttons without a direction do not override") {
    const CompatPalette p = select_compat_palette(nintendo_header("POKEMON RED"), {.a = true, .b = true});
    CHECK(p.bg == Colors{0x7FFF, 0x421F, 0x1CF2, 0x0000});
}
