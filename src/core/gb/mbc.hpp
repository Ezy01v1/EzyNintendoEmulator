#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ezy::gb {

// Memory bank controllers. Each one is a small value type with the same shape:
//
//   rom_offset(addr, rom_size)   byte offset into the ROM for a read at 0x0000-0x7FFF
//   write_control(addr, value)   writes to 0x0000-0x7FFF configure the MBC
//   read_ram(addr, ram)          read at 0xA000-0xBFFF (0xFF when not accessible)
//   write_ram(addr, value, ram)  write at 0xA000-0xBFFF; returns true if it changed save state
//
// They are held in a std::variant (see Cartridge), so adding one means adding a type here
// and std::visit fails to compile anywhere a case is missing.

inline constexpr std::size_t kRomBankSize = 0x4000;
inline constexpr std::size_t kRamBankSize = 0x2000;

// ROM ONLY: 32 KiB mapped flat, no RAM, writes ignored.
struct NoMbc {
    [[nodiscard]] std::size_t rom_offset(std::uint16_t addr, std::size_t /*rom_size*/) const { return addr; }
    void write_control(std::uint16_t /*addr*/, std::uint8_t /*value*/) {}
    [[nodiscard]] std::uint8_t read_ram(std::uint16_t /*addr*/, std::span<const std::uint8_t> /*ram*/) const {
        return 0xFF;
    }
    bool write_ram(std::uint16_t /*addr*/, std::uint8_t /*value*/, std::span<std::uint8_t> /*ram*/) { return false; }
};

struct Mbc1 {
    [[nodiscard]] std::size_t rom_offset(std::uint16_t addr, std::size_t rom_size) const;
    void write_control(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const;
    bool write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram);

    std::uint8_t bank_low = 1;   // 5 bits, 0 is treated as 1
    std::uint8_t bank_high = 0;  // 2 bits
    bool advanced_mode = false;  // also applies bank_high to 0x0000-0x3FFF and RAM
    bool ram_enabled = false;

private:
    [[nodiscard]] std::size_t ram_offset(std::uint16_t addr, std::size_t ram_size) const;
};

using Mbc = std::variant<NoMbc, Mbc1>;

}  // namespace ezy::gb
