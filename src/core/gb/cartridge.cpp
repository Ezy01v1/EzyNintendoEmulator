#include "gb/cartridge.hpp"

#include <utility>

namespace ezy::gb {

namespace {

// 0x148: 32 KiB << code, for codes 0x00-0x08.
std::optional<std::size_t> decode_rom_size(std::uint8_t code) {
    if (code > 0x08) return std::nullopt;
    return std::size_t{32 * 1024} << code;
}

// 0x149: fixed table (0x01 is unofficial, 2 KiB).
std::optional<std::size_t> decode_ram_size(std::uint8_t code) {
    switch (code) {
        case 0x00: return 0;
        case 0x01: return 2 * 1024;
        case 0x02: return 8 * 1024;
        case 0x03: return 32 * 1024;
        case 0x04: return 128 * 1024;
        case 0x05: return 64 * 1024;
        default: return std::nullopt;
    }
}

}  // namespace

std::uint8_t compute_header_checksum(std::span<const std::uint8_t> rom) {
    std::uint8_t x = 0;
    for (std::size_t addr = kTitleStart; addr < kHeaderChecksumAddr; ++addr) {
        x = static_cast<std::uint8_t>(x - rom[addr] - 1);
    }
    return x;
}

std::optional<CartridgeHeader> parse_header(std::span<const std::uint8_t> rom) {
    if (rom.size() < kHeaderEnd) {
        return std::nullopt;
    }
    const auto rom_size = decode_rom_size(rom[kRomSizeAddr]);
    const auto ram_size = decode_ram_size(rom[kRamSizeAddr]);
    if (!rom_size || !ram_size) {
        return std::nullopt;
    }

    CartridgeHeader header;
    // The title is padded with zeros; stop at the first one.
    for (std::size_t addr = kTitleStart; addr <= kTitleEnd && rom[addr] != 0; ++addr) {
        header.title.push_back(static_cast<char>(rom[addr]));
    }
    header.cart_type = rom[kCartTypeAddr];
    header.rom_size = *rom_size;
    header.ram_size = *ram_size;
    header.header_checksum = rom[kHeaderChecksumAddr];
    header.header_checksum_ok = compute_header_checksum(rom) == header.header_checksum;
    return header;
}

std::string_view cart_type_name(std::uint8_t type) {
    switch (type) {
        case 0x00: return "ROM ONLY";
        case 0x01: return "MBC1";
        case 0x02: return "MBC1+RAM";
        case 0x03: return "MBC1+RAM+BATTERY";
        case 0x05: return "MBC2";
        case 0x06: return "MBC2+BATTERY";
        case 0x0F: return "MBC3+TIMER+BATTERY";
        case 0x10: return "MBC3+TIMER+RAM+BATTERY";
        case 0x11: return "MBC3";
        case 0x12: return "MBC3+RAM";
        case 0x13: return "MBC3+RAM+BATTERY";
        case 0x19: return "MBC5";
        case 0x1A: return "MBC5+RAM";
        case 0x1B: return "MBC5+RAM+BATTERY";
        default: return "UNKNOWN";
    }
}

std::optional<Cartridge> Cartridge::load(std::span<const std::uint8_t> rom) {
    auto header = parse_header(rom);
    if (!header) {
        return std::nullopt;
    }
    return Cartridge(std::vector<std::uint8_t>(rom.begin(), rom.end()), std::move(*header));
}

std::uint8_t Cartridge::read_rom(std::uint16_t addr) const {
    // Reads past the end of a short ROM return open-bus 0xFF.
    return addr < rom_.size() ? rom_[addr] : 0xFF;
}

}  // namespace ezy::gb
