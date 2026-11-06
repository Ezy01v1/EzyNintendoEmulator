#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "gb/mbc.hpp"

namespace ezy::gb {

// Cartridge header layout (Pan Docs, "The Cartridge Header").
inline constexpr std::size_t kTitleStart = 0x134;
inline constexpr std::size_t kTitleEnd = 0x143;  // inclusive (0x142 on CGB cartridges)
inline constexpr std::size_t kCgbFlagAddr = 0x143;
inline constexpr std::size_t kNewLicenseeAddr = 0x144;  // 2 ASCII characters
inline constexpr std::size_t kOldLicenseeAddr = 0x14B;  // 0x33 = "use the new code"
inline constexpr std::size_t kCartTypeAddr = 0x147;
inline constexpr std::size_t kRomSizeAddr = 0x148;
inline constexpr std::size_t kRamSizeAddr = 0x149;
inline constexpr std::size_t kHeaderChecksumAddr = 0x14D;
inline constexpr std::size_t kHeaderEnd = 0x150;  // a ROM must be at least this big

struct CartridgeHeader {
    std::string title;
    // The 16 raw bytes 0x134-0x143. The CGB boot ROM sums them to pick a palette.
    std::array<std::uint8_t, 16> title_bytes{};
    // 0x80 = also runs on CGB with colour, 0xC0 = CGB only, anything else = DMG game.
    std::uint8_t cgb_flag = 0;
    std::uint8_t old_licensee = 0;
    std::array<char, 2> new_licensee{};
    std::uint8_t cart_type = 0;
    std::size_t rom_size = 0;  // bytes, as declared by the header
    std::size_t ram_size = 0;  // bytes, as declared by the header
    std::uint8_t header_checksum = 0;
    bool header_checksum_ok = false;

    [[nodiscard]] bool supports_cgb() const { return (cgb_flag & 0x80) != 0; }
    // Games published by Nintendo: the only ones the CGB boot ROM has palettes for.
    [[nodiscard]] bool nintendo_licensee() const {
        return old_licensee == 0x01 || (old_licensee == 0x33 && new_licensee == std::array<char, 2>{'0', '1'});
    }
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
    // Writes to 0x0000-0x7FFF configure the MBC.
    void write_rom(std::uint16_t addr, std::uint8_t value);

    // 0xA000-0xBFFF: external RAM. Reads 0xFF and ignores writes while disabled
    // or when the cartridge has no RAM.
    [[nodiscard]] std::uint8_t read_ram(std::uint16_t addr) const;
    void write_ram(std::uint16_t addr, std::uint8_t value);

    // Advances MBC hardware that runs on its own (the MBC3 clock).
    void tick(std::uint32_t t_cycles);

    [[nodiscard]] bool has_battery() const;
    [[nodiscard]] bool has_rtc() const;
    // MBC5+RUMBLE: whether the game currently has the motor on (the frontend may vibrate).
    [[nodiscard]] bool rumble_active() const;
    [[nodiscard]] std::span<const std::uint8_t> ram() const { return ram_; }

    // Battery-backed state persists between sessions as a .sav image: the raw RAM, then,
    // for cartridges with a clock, the 48-byte RTC footer used by BGB/VBA/mGBA (live and
    // latched registers as u32 LE, plus the unix time of the save as u64 LE).
    // The core never reads the clock itself: callers pass the current unix time.
    [[nodiscard]] std::vector<std::uint8_t> save_data(std::int64_t unix_now = 0) const;
    // Copies min(data.size(), RAM size) bytes of RAM; the rest keeps its contents. If an RTC
    // footer is present, restores the clock and adds the time elapsed since the save.
    // Returns false if the size is not one this cartridge would have written.
    bool load_save_data(std::span<const std::uint8_t> data, std::int64_t unix_now = 0);
    // Set by every write to external RAM or the RTC, until cleared by whoever persists it.
    [[nodiscard]] bool save_dirty() const { return save_dirty_; }

    // Save states: the MBC registers and the RAM (the ROM and header never change).
    template <class Ar>
    void serialize(Ar& ar) {
        std::visit([&](auto& mbc) { mbc.serialize(ar); }, mbc_);
        ar(ram_);
        if constexpr (Ar::kLoading) save_dirty_ = true;
    }
    void clear_save_dirty() { save_dirty_ = false; }

    [[nodiscard]] const Mbc& mbc() const { return mbc_; }

private:
    Cartridge(std::vector<std::uint8_t> rom, CartridgeHeader header, Mbc mbc)
        : rom_(std::move(rom)), header_(std::move(header)), mbc_(mbc), ram_(header_.ram_size, 0) {}

    std::vector<std::uint8_t> rom_;
    CartridgeHeader header_;
    Mbc mbc_;
    std::vector<std::uint8_t> ram_;  // header_.ram_size bytes; declared after header_
    bool save_dirty_ = false;
};

}  // namespace ezy::gb
