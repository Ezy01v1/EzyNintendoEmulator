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

// --- MBC3 RTC ---

// Counters roll over at their bit width (seconds/minutes 6 bits, hours 5 bits) and only
// carry into the next one when they pass the real limit, so values a game writes out of
// range (e.g. 61 seconds) count up to the bit limit and wrap without carrying.
void Rtc::step_second() {
    auto& [s, m, h, dl, dh] = regs;
    if (s != 59) { s = (s + 1) & 0x3F; return; }
    s = 0;
    if (m != 59) { m = (m + 1) & 0x3F; return; }
    m = 0;
    if (h != 23) { h = (h + 1) & 0x1F; return; }
    h = 0;
    if (++dl != 0) return;
    if ((dh & kDayHighBit8) == 0) {
        dh |= kDayHighBit8;
    } else {
        dh = static_cast<std::uint8_t>((dh & ~kDayHighBit8) | kDayCarry);  // day 511 -> 0
    }
}

void Rtc::tick(std::uint32_t t_cycles) {
    if (halted()) return;
    subsecond_cycles += t_cycles;
    while (subsecond_cycles >= kCyclesPerSecond) {
        subsecond_cycles -= kCyclesPerSecond;
        step_second();
    }
}

void Rtc::advance_seconds(std::uint64_t seconds) {
    if (halted()) return;
    // Step one at a time until every counter is in its normal range, then jump.
    while (seconds > 0 && (regs[0] > 59 || regs[1] > 59 || regs[2] > 23)) {
        step_second();
        --seconds;
    }
    if (seconds == 0) return;
    std::uint64_t days = std::uint64_t{regs[3]} | (static_cast<std::uint64_t>(regs[4] & kDayHighBit8) << 8);
    std::uint64_t total = regs[0] + regs[1] * 60ULL + regs[2] * 3600ULL + seconds;
    days += total / 86400;
    total %= 86400;
    regs[0] = static_cast<std::uint8_t>(total % 60);
    regs[1] = static_cast<std::uint8_t>(total / 60 % 60);
    regs[2] = static_cast<std::uint8_t>(total / 3600);
    std::uint8_t dh = regs[4] & static_cast<std::uint8_t>(~kDayHighBit8);
    if (days > 511) dh |= kDayCarry;
    days %= 512;
    regs[3] = static_cast<std::uint8_t>(days & 0xFF);
    regs[4] = static_cast<std::uint8_t>(dh | (days >> 8));
}

std::uint8_t Rtc::mask(std::uint8_t reg, std::uint8_t value) {
    switch (reg) {
        case 0x08: case 0x09: return value & 0x3F;
        case 0x0A: return value & 0x1F;
        case 0x0B: return value;
        default: return value & (kDayHighBit8 | kHalt | kDayCarry);
    }
}

void Rtc::write(std::uint8_t reg, std::uint8_t value) {
    regs[reg - 0x08] = mask(reg, value);
    if (reg == 0x08) subsecond_cycles = 0;  // writing seconds resets the sub-second divider
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
    } else {
        // Latch: writing 0x00 then 0x01 copies the live clock into the readable registers.
        if (has_rtc && last_latch_write == 0x00 && value == 0x01) rtc.latch();
        last_latch_write = value;
    }
}

std::uint8_t Mbc3::read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const {
    if (!ram_enabled) return 0xFF;
    if (ram_select >= 0x08 && ram_select <= 0x0C) return has_rtc ? rtc.read(ram_select) : 0xFF;
    if (ram_select > 0x03 || ram.empty()) return 0xFF;
    return ram[ram_bank_offset(ram_select, addr, ram.size())];
}

bool Mbc3::write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram) {
    if (!ram_enabled) return false;
    if (ram_select >= 0x08 && ram_select <= 0x0C) {
        if (!has_rtc) return false;
        rtc.write(ram_select, value);
        return true;
    }
    if (ram_select > 0x03 || ram.empty()) return false;
    ram[ram_bank_offset(ram_select, addr, ram.size())] = value;
    return true;
}

// --- MBC5 ---

std::size_t Mbc5::rom_offset(std::uint16_t addr, std::size_t rom_size) const {
    if (addr < 0x4000) return addr;  // always bank 0
    const std::size_t bank_count = std::max<std::size_t>(1, rom_size / kRomBankSize);
    return (std::size_t{rom_bank} % bank_count) * kRomBankSize + (addr & 0x3FFF);
}

void Mbc5::write_control(std::uint16_t addr, std::uint8_t value) {
    if (addr < 0x2000) {
        ram_enabled = value == 0x0A;  // MBC5 compares all 8 bits, not just the low nibble
    } else if (addr < 0x3000) {
        rom_bank = static_cast<std::uint16_t>((rom_bank & 0x100) | value);
    } else if (addr < 0x4000) {
        rom_bank = static_cast<std::uint16_t>((rom_bank & 0xFF) | ((value & 0x01) << 8));
    } else if (addr < 0x6000) {
        if (has_rumble) {
            rumble_on = (value & 0x08) != 0;
            ram_bank = value & 0x07;
        } else {
            ram_bank = value & 0x0F;
        }
    }
    // 0x6000-0x7FFF: unused.
}

std::uint8_t Mbc5::read_ram(std::uint16_t addr, std::span<const std::uint8_t> ram) const {
    if (!ram_enabled || ram.empty()) return 0xFF;
    return ram[ram_bank_offset(ram_bank, addr, ram.size())];
}

bool Mbc5::write_ram(std::uint16_t addr, std::uint8_t value, std::span<std::uint8_t> ram) {
    if (!ram_enabled || ram.empty()) return false;
    ram[ram_bank_offset(ram_bank, addr, ram.size())] = value;
    return true;
}

}  // namespace ezy::gb
