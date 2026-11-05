#include "gb/cartridge.hpp"

#include <algorithm>
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
    std::size_t offset = addr;
    if (is_mbc1()) {
        constexpr std::size_t kBankSize = 0x4000;
        const std::size_t bank_count = std::max<std::size_t>(1, rom_.size() / kBankSize);
        std::size_t bank = 0;
        if (addr < 0x4000) {
            bank = advanced_mode_ ? std::size_t{bank_high_} << 5 : 0;
        } else {
            bank = (std::size_t{bank_high_} << 5) | bank_low_;
        }
        offset = (bank % bank_count) * kBankSize + (addr & 0x3FFF);
    }
    // Reads past the end of a short ROM return open-bus 0xFF.
    return offset < rom_.size() ? rom_[offset] : 0xFF;
}

void Cartridge::write_rom(std::uint16_t addr, std::uint8_t value) {
    if (!is_mbc1()) return;  // ROM ONLY: writes are ignored
    if (addr < 0x2000) {
        ram_enabled_ = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
        bank_low_ = value & 0x1F;
        if (bank_low_ == 0) bank_low_ = 1;
    } else if (addr < 0x6000) {
        bank_high_ = value & 0x03;
    } else {
        advanced_mode_ = (value & 0x01) != 0;
    }
}

bool Cartridge::has_battery() const {
    switch (header_.cart_type) {
        case 0x03: case 0x06: case 0x09: case 0x0D: case 0x0F:
        case 0x10: case 0x13: case 0x1B: case 0x1E: case 0x22:
            return true;
        default:
            return false;
    }
}

std::optional<std::size_t> Cartridge::ram_offset(std::uint16_t addr) const {
    if (!is_mbc1() || !ram_enabled_ || ram_.empty()) return std::nullopt;
    constexpr std::size_t kBankSize = 0x2000;
    const std::size_t local = addr & 0x1FFF;
    // RAM smaller than one bank (2 KiB) mirrors inside the window.
    if (ram_.size() < kBankSize) return local % ram_.size();
    const std::size_t bank_count = ram_.size() / kBankSize;
    const std::size_t bank = advanced_mode_ ? bank_high_ % bank_count : 0;
    return bank * kBankSize + local;
}

std::uint8_t Cartridge::read_ram(std::uint16_t addr) const {
    const auto offset = ram_offset(addr);
    return offset ? ram_[*offset] : 0xFF;
}

void Cartridge::write_ram(std::uint16_t addr, std::uint8_t value) {
    const auto offset = ram_offset(addr);
    if (!offset) return;
    ram_[*offset] = value;
    save_dirty_ = true;
}

bool Cartridge::load_save_data(std::span<const std::uint8_t> data) {
    const std::size_t n = std::min(data.size(), ram_.size());
    std::copy_n(data.begin(), n, ram_.begin());
    return data.size() == ram_.size();
}

}  // namespace ezy::gb
