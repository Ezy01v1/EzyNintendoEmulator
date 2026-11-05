#include "gb/mbc.hpp"

#include <algorithm>

namespace ezy::gb {

namespace {

// Offset for RAM smaller than a bank (2 KiB mirrors inside the 8 KiB window).
std::size_t ram_bank_offset(std::size_t bank, std::uint16_t addr, std::size_t ram_size) {
    const std::size_t local = addr & 0x1FFF;
    if (ram_size < kRamBankSize) return local % ram_size;
    const std::size_t bank_count = ram_size / kRamBankSize;
    return (bank % bank_count) * kRamBankSize + local;
}

}  // namespace

// --- MBC1 ---

std::size_t Mbc1::rom_offset(std::uint16_t addr, std::size_t rom_size) const {
    const std::size_t bank_count = std::max<std::size_t>(1, rom_size / kRomBankSize);
    std::size_t bank = 0;
    if (addr < 0x4000) {
        bank = advanced_mode ? std::size_t{bank_high} << 5 : 0;
    } else {
        bank = (std::size_t{bank_high} << 5) | bank_low;
    }
    return (bank % bank_count) * kRomBankSize + (addr & 0x3FFF);
}

void Mbc1::write_control(std::uint16_t addr, std::uint8_t value) {
    if (addr < 0x2000) {
        ram_enabled = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
        bank_low = value & 0x1F;
        if (bank_low == 0) bank_low = 1;
    } else if (addr < 0x6000) {
        bank_high = value & 0x03;
    } else {
        advanced_mode = (value & 0x01) != 0;
    }
}

std::size_t Mbc1::ram_offset(std::uint16_t addr, std::size_t ram_size) const {
    return ram_bank_offset(advanced_mode ? bank_high : 0, addr, ram_size);
}

std::uint8_t Mbc1::read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const {
    if (!ram_enabled || ram.empty()) return 0xFF;
    return ram[ram_offset(addr, ram.size())];
}

bool Mbc1::write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram) {
    if (!ram_enabled || ram.empty()) return false;
    ram[ram_offset(addr, ram.size())] = value;
    return true;
}

// --- MBC3 ---

std::size_t Mbc3::rom_offset(std::uint16_t addr, std::size_t rom_size) const {
    if (addr < 0x4000) return addr;  // always bank 0
    const std::size_t bank_count = std::max<std::size_t>(1, rom_size / kRomBankSize);
    return (std::size_t{rom_bank} % bank_count) * kRomBankSize + (addr & 0x3FFF);
}

void Mbc3::write_control(std::uint16_t addr, std::uint8_t value) {
    if (addr < 0x2000) {
        ram_enabled = (value & 0x0F) == 0x0A;
    } else if (addr < 0x4000) {
        rom_bank = value & 0x7F;
        if (rom_bank == 0) rom_bank = 1;
    } else if (addr < 0x6000) {
        ram_select = value & 0x0F;
    }
    // 0x6000-0x7FFF: RTC latch (no RTC yet).
}

std::uint8_t Mbc3::read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const {
    if (!ram_enabled || ram_select > 0x03 || ram.empty()) return 0xFF;
    return ram[ram_bank_offset(ram_select, addr, ram.size())];
}

bool Mbc3::write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram) {
    if (!ram_enabled || ram_select > 0x03 || ram.empty()) return false;
    ram[ram_bank_offset(ram_select, addr, ram.size())] = value;
    return true;
}

}  // namespace ezy::gb
