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
        case 0x1C: return "MBC5+RUMBLE";
        case 0x1D: return "MBC5+RUMBLE+RAM";
        case 0x1E: return "MBC5+RUMBLE+RAM+BATTERY";
        default: return "UNKNOWN";
    }
}

namespace {

Mbc make_mbc(std::uint8_t cart_type) {
    switch (cart_type) {
        case 0x01: case 0x02: case 0x03: return Mbc1{};
        case 0x0F: case 0x10: return Mbc3{.has_rtc = true};
        case 0x11: case 0x12: case 0x13: return Mbc3{};
        case 0x19: case 0x1A: case 0x1B: return Mbc5{};
        case 0x1C: case 0x1D: case 0x1E: return Mbc5{.has_rumble = true};
        default: return NoMbc{};  // ROM ONLY, and MBCs not implemented yet
    }
}

constexpr std::size_t kRtcFooterSize = 48;
constexpr std::size_t kRtcFooterSize32 = 44;  // older variant with a 32-bit timestamp

void put_le(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint64_t get_le(std::span<const std::uint8_t> in, std::size_t pos, int bytes) {
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= std::uint64_t{in[pos + i]} << (8 * i);
    return value;
}

}  // namespace

std::optional<Cartridge> Cartridge::load(std::span<const std::uint8_t> rom) {
    auto header = parse_header(rom);
    if (!header) {
        return std::nullopt;
    }
    const Mbc mbc = make_mbc(header->cart_type);
    return Cartridge(std::vector<std::uint8_t>(rom.begin(), rom.end()), std::move(*header), mbc);
}

std::uint8_t Cartridge::read_rom(std::uint16_t addr) const {
    const std::size_t offset = std::visit([&](const auto& m) { return m.rom_offset(addr, rom_.size()); }, mbc_);
    // Reads past the end of a short ROM return open-bus 0xFF.
    return offset < rom_.size() ? rom_[offset] : 0xFF;
}

void Cartridge::write_rom(std::uint16_t addr, std::uint8_t value) {
    std::visit([&](auto& m) { m.write_control(addr, value); }, mbc_);
}

std::uint8_t Cartridge::read_ram(std::uint16_t addr) const {
    return std::visit([&](const auto& m) { return m.read_ram(addr, ram_); }, mbc_);
}

void Cartridge::write_ram(std::uint16_t addr, std::uint8_t value) {
    if (std::visit([&](auto& m) { return m.write_ram(addr, value, ram_); }, mbc_)) save_dirty_ = true;
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

void Cartridge::tick(std::uint32_t t_cycles) {
    std::visit(
        [&](auto& m) {
            if constexpr (requires { m.tick(t_cycles); }) m.tick(t_cycles);
        },
        mbc_);
}

bool Cartridge::rumble_active() const {
    const auto* mbc5 = std::get_if<Mbc5>(&mbc_);
    return mbc5 != nullptr && mbc5->rumble_on;
}

bool Cartridge::has_rtc() const {
    const auto* mbc3 = std::get_if<Mbc3>(&mbc_);
    return mbc3 != nullptr && mbc3->has_rtc;
}

std::vector<std::uint8_t> Cartridge::save_data(std::int64_t unix_now) const {
    std::vector<std::uint8_t> out(ram_.begin(), ram_.end());
    if (const auto* mbc3 = std::get_if<Mbc3>(&mbc_); mbc3 && mbc3->has_rtc) {
        for (const auto r : mbc3->rtc.regs) put_le(out, r, 4);
        for (const auto r : mbc3->rtc.latched) put_le(out, r, 4);
        put_le(out, static_cast<std::uint64_t>(unix_now), 8);
    }
    return out;
}

bool Cartridge::load_save_data(std::span<const std::uint8_t> data, std::int64_t unix_now) {
    const std::size_t n = std::min(data.size(), ram_.size());
    std::copy_n(data.begin(), n, ram_.begin());
    if (data.size() == ram_.size()) return true;

    auto* mbc3 = std::get_if<Mbc3>(&mbc_);
    const std::size_t footer = data.size() - n;
    if (!mbc3 || !mbc3->has_rtc || data.size() < ram_.size() ||
        (footer != kRtcFooterSize && footer != kRtcFooterSize32)) {
        return false;
    }
    const auto rtc_bytes = data.subspan(ram_.size());
    Rtc& rtc = mbc3->rtc;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto reg = static_cast<std::uint8_t>(0x08 + i);
        rtc.regs[i] = Rtc::mask(reg, static_cast<std::uint8_t>(get_le(rtc_bytes, i * 4, 4)));
        rtc.latched[i] = Rtc::mask(reg, static_cast<std::uint8_t>(get_le(rtc_bytes, 20 + i * 4, 4)));
    }
    const auto saved_at = static_cast<std::int64_t>(get_le(rtc_bytes, 40, footer == kRtcFooterSize ? 8 : 4));
    if (unix_now > saved_at) rtc.advance_seconds(static_cast<std::uint64_t>(unix_now - saved_at));
    return true;
}

}  // namespace ezy::gb
