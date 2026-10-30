#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "gb/cartridge.hpp"

namespace ezy::gb::test {

// Builds a synthetic 32 KiB ROM with a valid header.
inline std::vector<std::uint8_t> make_rom(std::string_view title = "TEST", std::uint8_t type = 0x00,
                                          std::uint8_t rom_code = 0x00, std::uint8_t ram_code = 0x00) {
    std::vector<std::uint8_t> rom(32 * 1024, 0);
    for (std::size_t i = 0; i < title.size(); ++i) {
        rom[kTitleStart + i] = static_cast<std::uint8_t>(title[i]);
    }
    rom[kCartTypeAddr] = type;
    rom[kRomSizeAddr] = rom_code;
    rom[kRamSizeAddr] = ram_code;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return rom;
}

inline Cartridge make_cartridge(std::vector<std::uint8_t> rom = make_rom()) {
    return *Cartridge::load(rom);
}

}  // namespace ezy::gb::test
