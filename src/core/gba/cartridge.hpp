#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ezy::gba {

// Cartridge header layout (GBATEK, "GBA Cartridge Header").
inline constexpr std::size_t kTitleAddr = 0xA0;     // 12 bytes, uppercase ASCII
inline constexpr std::size_t kGameCodeAddr = 0xAC;  // 4 bytes, e.g. "AXVE"
inline constexpr std::size_t kMakerCodeAddr = 0xB0; // 2 bytes
inline constexpr std::size_t kFixedValueAddr = 0xB2;  // must be 0x96
inline constexpr std::size_t kVersionAddr = 0xBC;
inline constexpr std::size_t kHeaderChecksumAddr = 0xBD;
inline constexpr std::size_t kHeaderEnd = 0xC0;  // a ROM must be at least this big
inline constexpr std::size_t kMaxRomSize = 32 * 1024 * 1024;

struct CartridgeHeader {
    std::string title;
    std::string game_code;
    std::string maker_code;
    std::uint8_t version = 0;
    std::uint8_t header_checksum = 0;
    bool header_checksum_ok = false;
    bool fixed_value_ok = false;  // 0x96 at 0xB2
};

// Complement check over 0xA0-0xBC, as verified by the BIOS before booting.
[[nodiscard]] std::uint8_t compute_header_checksum(std::span<const std::uint8_t> rom);

// Returns nullopt if the ROM is smaller than the header or larger than 32 MiB.
// A bad checksum is reported, not rejected (homebrew and test ROMs often skip it).
[[nodiscard]] std::optional<CartridgeHeader> parse_header(std::span<const std::uint8_t> rom);

}  // namespace ezy::gba
