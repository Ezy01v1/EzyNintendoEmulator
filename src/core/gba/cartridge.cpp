#include "gba/cartridge.hpp"

namespace ezy::gba {

namespace {

// Header strings are padded with zeros; stop at the first one.
std::string read_string(std::span<const std::uint8_t> rom, std::size_t addr, std::size_t length) {
    std::string out;
    for (std::size_t i = addr; i < addr + length && rom[i] != 0; ++i) out.push_back(static_cast<char>(rom[i]));
    return out;
}

}  // namespace

std::uint8_t compute_header_checksum(std::span<const std::uint8_t> rom) {
    std::uint8_t sum = 0;
    for (std::size_t addr = kTitleAddr; addr < kHeaderChecksumAddr; ++addr) {
        sum = static_cast<std::uint8_t>(sum - rom[addr]);
    }
    return static_cast<std::uint8_t>(sum - 0x19);
}

std::optional<CartridgeHeader> parse_header(std::span<const std::uint8_t> rom) {
    if (rom.size() < kHeaderEnd || rom.size() > kMaxRomSize) return std::nullopt;
    CartridgeHeader header;
    header.title = read_string(rom, kTitleAddr, 12);
    header.game_code = read_string(rom, kGameCodeAddr, 4);
    header.maker_code = read_string(rom, kMakerCodeAddr, 2);
    header.version = rom[kVersionAddr];
    header.header_checksum = rom[kHeaderChecksumAddr];
    header.header_checksum_ok = compute_header_checksum(rom) == header.header_checksum;
    header.fixed_value_ok = rom[kFixedValueAddr] == 0x96;
    return header;
}

}  // namespace ezy::gba
