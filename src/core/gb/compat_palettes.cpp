#include "gb/compat_palettes.hpp"

#include <cstddef>
#include <string_view>

namespace ezy::gb {

namespace {

// Title checksums the boot ROM recognises. From index 65 on, the checksum is shared
// by several games and the 4th title letter must also match.
constexpr std::array<std::uint8_t, 94> kTitleChecksums = {
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
    0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
    0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
    0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
    0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
    0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
    0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6,
    0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};
constexpr std::size_t kFirstChecksumWithDuplicate = 65;
constexpr std::string_view kFourthLetters = "BEFAARBEKEK R-URAR INAILICE R";
// Palette combination for each checksum entry.
constexpr std::array<std::uint8_t, 94> kPalettePerChecksum = {
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36, 7, 37, 30, 44,
    21, 32, 31, 20, 5, 33, 13, 14, 5, 29, 5, 18, 9, 3, 2, 26,
    25, 25, 41, 42, 26, 45, 42, 45, 36, 38, 26, 42, 30, 41, 34, 34,
    5, 42, 6, 5, 33, 25, 42, 42, 40, 2, 16, 25, 42, 42, 5, 0,
    39, 36, 22, 25, 6, 32, 12, 36, 11, 39, 18, 39, 24, 31, 50, 17,
    46, 6, 27, 0, 47, 41, 41, 0, 0, 19, 34, 23, 18, 29,
};
// Palette combinations: word offsets into kColors for OBJ0, OBJ1 and BG (4 colours each).
constexpr std::array<std::array<std::uint8_t, 3>, 51> kCombinations = {{
    {16, 16, 116},
    {72, 72, 72},
    {80, 80, 80},
    {96, 96, 96},
    {36, 36, 36},
    {0, 0, 0},
    {108, 108, 108},
    {20, 20, 20},
    {48, 48, 48},
    {104, 104, 104},
    {64, 32, 32},
    {16, 112, 112},
    {16, 8, 8},
    {12, 16, 16},
    {16, 116, 116},
    {112, 16, 112},
    {8, 68, 8},
    {64, 64, 32},
    {16, 16, 28},
    {16, 16, 72},
    {16, 16, 80},
    {76, 76, 36},
    {15, 15, 44},
    {68, 68, 8},
    {16, 16, 8},
    {16, 16, 12},
    {112, 112, 0},
    {12, 12, 0},
    {0, 0, 4},
    {72, 88, 72},
    {80, 88, 80},
    {96, 88, 96},
    {64, 88, 32},
    {68, 16, 52},
    {111, 0, 56},
    {111, 16, 60},
    {76, 91, 36},
    {64, 112, 40},
    {16, 92, 112},
    {68, 88, 8},
    {16, 0, 8},
    {16, 112, 12},
    {112, 12, 0},
    {12, 112, 16},
    {84, 112, 16},
    {12, 112, 0},
    {100, 12, 112},
    {0, 112, 32},
    {16, 12, 112},
    {112, 12, 24},
    {16, 112, 116},
}};
// The colour table (RGB555), 4 colours per row.
constexpr std::array<std::uint16_t, 120> kColors = {
    0x7FFF, 0x32BF, 0x00D0, 0x0000, 0x639F, 0x4279, 0x15B0, 0x04CB,
    0x7FFF, 0x6E31, 0x454A, 0x0000, 0x7FFF, 0x1BEF, 0x0200, 0x0000,
    0x7FFF, 0x421F, 0x1CF2, 0x0000, 0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x03FF, 0x012F, 0x0000, 0x7FFF, 0x03EF, 0x01D6, 0x0000,
    0x7FFF, 0x42B5, 0x3DC8, 0x0000, 0x7E74, 0x03FF, 0x0180, 0x0000,
    0x67FF, 0x77AC, 0x1A13, 0x2D6B, 0x7ED6, 0x4BFF, 0x2175, 0x0000,
    0x53FF, 0x4A5F, 0x7E52, 0x0000, 0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,
    0x03ED, 0x7FFF, 0x255F, 0x0000, 0x036A, 0x021F, 0x03FF, 0x7FFF,
    0x7FFF, 0x01DF, 0x0112, 0x0000, 0x231F, 0x035F, 0x00F2, 0x0009,
    0x7FFF, 0x03EA, 0x011F, 0x0000, 0x299F, 0x001A, 0x000C, 0x0000,
    0x7FFF, 0x027F, 0x001F, 0x0000, 0x7FFF, 0x03E0, 0x0206, 0x0120,
    0x7FFF, 0x7EEB, 0x001F, 0x7C00, 0x7FFF, 0x3FFF, 0x7E00, 0x001F,
    0x7FFF, 0x03FF, 0x001F, 0x0000, 0x03FF, 0x001F, 0x000C, 0x0000,
    0x7FFF, 0x033F, 0x0193, 0x0000, 0x0000, 0x4200, 0x037F, 0x7FFF,
    0x7FFF, 0x7E8C, 0x7C00, 0x0000, 0x7FFF, 0x1BEF, 0x6180, 0x0000,
};
// Combination chosen by a button combo: Right, Left, Up, Down, each alone, + A, + B.
constexpr std::array<std::uint8_t, 12> kKeyCombinations = {1, 48, 5, 8, 0, 40, 43, 3, 6, 7, 28, 49};

// Index into kCombinations from the title, or 0 (the default) if the game isn't known.
std::size_t combination_for(const CartridgeHeader& header) {
    if (!header.nintendo_licensee()) return 0;
    std::uint8_t sum = 0;
    for (const auto byte : header.title_bytes) sum = static_cast<std::uint8_t>(sum + byte);
    for (std::size_t i = 0; i < kTitleChecksums.size(); ++i) {
        if (kTitleChecksums[i] != sum) continue;
        if (i >= kFirstChecksumWithDuplicate &&
            static_cast<std::uint8_t>(kFourthLetters[i - kFirstChecksumWithDuplicate]) != header.title_bytes[3]) {
            continue;  // same checksum, different game
        }
        return kPalettePerChecksum[i];
    }
    return 0;
}

// The boot ROM numbers combos 1-12: direction (1 Right, 2 Left, 3 Up, 4 Down) + 4 if A
// is held, + 8 if B is. 0 = no direction held = no override.
int key_combo(HeldButtons held) {
    int direction = 0;
    if (held.right) {
        direction = 1;
    } else if (held.left) {
        direction = 2;
    } else if (held.up) {
        direction = 3;
    } else if (held.down) {
        direction = 4;
    }
    if (direction == 0) return 0;
    if (held.a && held.b) return 0;  // not one of the 12 combinations
    return direction + (held.a ? 4 : 0) + (held.b ? 8 : 0);
}

std::array<std::uint16_t, 4> colors_at(std::size_t offset) {
    return {kColors[offset], kColors[offset + 1], kColors[offset + 2], kColors[offset + 3]};
}

}  // namespace

CompatPalette select_compat_palette(const CartridgeHeader& header, HeldButtons held) {
    const int combo = key_combo(held);
    const std::size_t index =
        combo != 0 ? kKeyCombinations[static_cast<std::size_t>(combo - 1)] : combination_for(header);
    const auto& c = kCombinations[index];
    return {colors_at(c[2]), colors_at(c[0]), colors_at(c[1])};
}

}  // namespace ezy::gb
