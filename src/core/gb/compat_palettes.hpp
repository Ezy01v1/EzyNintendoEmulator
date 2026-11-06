#pragma once

#include <array>
#include <cstdint>

#include "gb/cartridge.hpp"
#include "gb/model.hpp"

namespace ezy::gb {

// The colours a CGB gives a DMG game (DMG compatibility mode), RGB555.
struct CompatPalette {
    std::array<std::uint16_t, 4> bg;
    std::array<std::uint16_t, 4> obj0;
    std::array<std::uint16_t, 4> obj1;
};

// Picks the palette the CGB boot ROM would: by title checksum for Nintendo-published games
// (with the 4th title letter breaking ties), the default palette otherwise, unless a
// button combination overrides it. The tables come from SameBoy's free reimplementation
// of the CGB boot ROM (BootROMs/cgb_boot.asm, MIT licence), see Pan Docs, "Power Up
// Sequence -> Compatibility palettes".
[[nodiscard]] CompatPalette select_compat_palette(const CartridgeHeader& header, HeldButtons held);

}  // namespace ezy::gb
