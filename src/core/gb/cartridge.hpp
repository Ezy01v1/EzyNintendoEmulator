#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ezy::gb {

// Cartridge header layout (Pan Docs, "The Cartridge Header").
inline constexpr std::size_t kTitleStart = 0x134;
inline constexpr std::size_t kTitleEnd = 0x143;  // inclusive
inline constexpr std::size_t kCartTypeAddr = 0x147;
inline constexpr std::size_t kRomSizeAddr = 0x148;
inline constexpr std::size_t kRamSizeAddr = 0x149;
inline constexpr std::size_t kHeaderChecksumAddr = 0x14D;
inline constexpr std::size_t kHeaderEnd = 0x150;  // a ROM must be at least this big

struct CartridgeHeader {
    std::string title;
    std::uint8_t cart_type = 0;
    std::size_t rom_size = 0;  // bytes, as declared by the header
    std::size_t ram_size = 0;  // bytes, as declared by the header
    std::uint8_t header_checksum = 0;
    bool header_checksum_ok = false;
};

// Parses the header. Returns nullopt if the ROM is too small or declares an unknown size.
[[nodiscard]] std::optional<CartridgeHeader> parse_header(std::span<const std::uint8_t> rom);

// Checksum over 0x134..0x14C, as computed by the boot ROM.
[[nodiscard]] std::uint8_t compute_header_checksum(std::span<const std::uint8_t> rom);

[[nodiscard]] std::string_view cart_type_name(std::uint8_t type);

class Cartridge {
public:
    // Returns nullopt if the header can't be parsed.
    [[nodiscard]] static std::optional<Cartridge> load(std::span<const std::uint8_t> rom);

    [[nodiscard]] const CartridgeHeader& header() const { return header_; }

    // 0x0000-0x7FFF, through the MBC's current banks.
    [[nodiscard]] std::uint8_t read_rom(std::uint16_t addr) const;

    // Writes to 0x0000-0x7FFF configure the MBC. Only MBC1 ROM banking so far;
    // external RAM and the other MBCs come later.
    void write_rom(std::uint16_t addr, std::uint8_t value);

private:
    Cartridge(std::vector<std::uint8_t> rom, CartridgeHeader header)
        : rom_(std::move(rom)), header_(std::move(header)) {}

    [[nodiscard]] bool is_mbc1() const { return header_.cart_type >= 0x01 && header_.cart_type <= 0x03; }

    std::vector<std::uint8_t> rom_;
    CartridgeHeader header_;

    // MBC1 state.
    std::uint8_t bank_low_ = 1;   // 5 bits, 0 is treated as 1
    std::uint8_t bank_high_ = 0;  // 2 bits
    bool advanced_mode_ = false;  // also applies bank_high_ to 0x0000-0x3FFF
};

}  // namespace ezy::gb
