#pragma once

#include <array>
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
//   tick(t_cycles)               optional: only MBCs with a clock have it
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

    template <class Ar>
    void serialize(Ar& /*ar*/) {}
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

    template <class Ar>
    void serialize(Ar& ar) {
        ar(bank_low), ar(bank_high), ar(advanced_mode), ar(ram_enabled);
    }

private:
    [[nodiscard]] std::size_t ram_offset(std::uint16_t addr, std::size_t ram_size) const;
};

// MBC3 real-time clock. Counts emulated T-cycles while running; elapsed wall-clock time
// while the emulator was closed is added by the caller through advance_seconds().
struct Rtc {
    static constexpr std::uint32_t kCyclesPerSecond = 4'194'304;
    static constexpr std::uint8_t kDayHighBit8 = 0x01;
    static constexpr std::uint8_t kHalt = 0x40;
    static constexpr std::uint8_t kDayCarry = 0x80;

    // Live counters, then the copy the game reads after a latch. Index = register - 0x08.
    std::array<std::uint8_t, 5> regs{};     // S, M, H, DL, DH
    std::array<std::uint8_t, 5> latched{};
    std::uint32_t subsecond_cycles = 0;

    [[nodiscard]] bool halted() const { return (regs[4] & kHalt) != 0; }
    void tick(std::uint32_t t_cycles);
    void advance_seconds(std::uint64_t seconds);
    void latch() { latched = regs; }
    void write(std::uint8_t reg, std::uint8_t value);  // reg in 0x08-0x0C
    // Keeps only the bits the hardware stores for a register (0x08-0x0C).
    [[nodiscard]] static std::uint8_t mask(std::uint8_t reg, std::uint8_t value);
    [[nodiscard]] std::uint8_t read(std::uint8_t reg) const { return latched[reg - 0x08]; }

    template <class Ar>
    void serialize(Ar& ar) {
        ar(regs), ar(latched), ar(subsecond_cycles);
    }

private:
    void step_second();
};

struct Mbc3 {
    [[nodiscard]] std::size_t rom_offset(std::uint16_t addr, std::size_t rom_size) const;
    void write_control(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const;
    bool write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram);

    void tick(std::uint32_t t_cycles) {
        if (has_rtc) rtc.tick(t_cycles);
    }

    std::uint8_t rom_bank = 1;    // 7 bits, 0 is treated as 1
    std::uint8_t ram_select = 0;  // 0x00-0x03: RAM bank, 0x08-0x0C: RTC register
    bool ram_enabled = false;     // also gates the RTC registers
    std::uint8_t last_latch_write = 0xFF;
    bool has_rtc = false;
    Rtc rtc;

    template <class Ar>
    void serialize(Ar& ar) {
        ar(rom_bank), ar(ram_select), ar(ram_enabled), ar(last_latch_write), ar(rtc);
    }
};

struct Mbc5 {
    [[nodiscard]] std::size_t rom_offset(std::uint16_t addr, std::size_t rom_size) const;
    void write_control(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const;
    bool write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram);

    std::uint16_t rom_bank = 1;  // 9 bits; unlike MBC1/MBC3, bank 0 is a valid choice
    std::uint8_t ram_bank = 0;   // 4 bits (3 on rumble carts)
    bool ram_enabled = false;
    bool has_rumble = false;     // bit 3 of the RAM bank register drives the motor instead
    bool rumble_on = false;

    template <class Ar>
    void serialize(Ar& ar) {
        ar(rom_bank), ar(ram_bank), ar(ram_enabled), ar(rumble_on);
    }
};

using Mbc = std::variant<NoMbc, Mbc1, Mbc3, Mbc5>;

}  // namespace ezy::gb
