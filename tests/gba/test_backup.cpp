#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "gba/backup.hpp"
#include "gba/bus.hpp"

using namespace ezy::gba;

namespace {

// A 4 KiB ROM with a backup ID string at `at`.
std::vector<std::uint8_t> rom_with_id(std::string_view id, std::size_t at = 0x400) {
    std::vector<std::uint8_t> rom(4096);
    std::copy(id.begin(), id.end(), rom.begin() + static_cast<std::ptrdiff_t>(at));
    return rom;
}

// The flash unlock sequence followed by a command at 0x5555.
void flash_command(Bus& bus, std::uint8_t command) {
    bus.write8(0x0E005555, 0xAA);
    bus.write8(0x0E002AAA, 0x55);
    bus.write8(0x0E005555, command);
}

constexpr std::uint32_t kIwram = 0x03000000;
constexpr std::uint32_t kDma3 = 0x040000D4;  // SAD, DAD, CNT_L, CNT_H
constexpr std::uint32_t kEeprom = 0x0D000000;

void push_bits(std::vector<std::uint16_t>& bits, std::uint64_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bits.push_back(static_cast<std::uint16_t>((value >> i) & 1U));
}

// "10" + address + 64 data bits + stop bit.
std::vector<std::uint16_t> eeprom_write_request(std::uint32_t block, int address_bits, std::uint64_t data) {
    std::vector<std::uint16_t> bits;
    push_bits(bits, 0b10, 2);
    push_bits(bits, block, address_bits);
    push_bits(bits, data, 64);
    bits.push_back(0);
    return bits;
}

// "11" + address + stop bit.
std::vector<std::uint16_t> eeprom_read_request(std::uint32_t block, int address_bits) {
    std::vector<std::uint16_t> bits;
    push_bits(bits, 0b11, 2);
    push_bits(bits, block, address_bits);
    bits.push_back(0);
    return bits;
}

// Runs DMA 3 immediately with halfword units, as games talk to the EEPROM.
void dma3(Bus& bus, std::uint32_t src, std::uint32_t dst, std::uint16_t count) {
    bus.write32(kDma3, src);
    bus.write32(kDma3 + 4, dst);
    bus.write16(kDma3 + 8, count);
    bus.write16(kDma3 + 10, 0x8000);  // enable, 16-bit, immediate, both addresses increment
}

void send_to_eeprom(Bus& bus, const std::vector<std::uint16_t>& bits) {
    for (std::size_t i = 0; i < bits.size(); ++i) bus.write16(kIwram + static_cast<std::uint32_t>(i) * 2, bits[i]);
    dma3(bus, kIwram, kEeprom, static_cast<std::uint16_t>(bits.size()));
}

// Reads the 68-bit answer to a read request and returns its 64 data bits.
std::uint64_t receive_from_eeprom(Bus& bus) {
    dma3(bus, kEeprom, kIwram + 0x400, 68);
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < 68; ++i) {
        const std::uint16_t bit = bus.read16(kIwram + 0x400 + i * 2);
        if (i < 4) {
            CHECK(bit == 0);  // dummy bits
        } else {
            value = (value << 1) | (bit & 1U);
        }
    }
    return value;
}

}  // namespace

TEST_CASE("Backup type comes from the word-aligned ID string in the ROM") {
    CHECK(detect_backup_type(rom_with_id("SRAM_V113")) == BackupType::Sram);
    CHECK(detect_backup_type(rom_with_id("SRAM_F_V100")) == BackupType::Sram);
    CHECK(detect_backup_type(rom_with_id("EEPROM_V124")) == BackupType::Eeprom);
    CHECK(detect_backup_type(rom_with_id("FLASH_V126")) == BackupType::Flash64);
    CHECK(detect_backup_type(rom_with_id("FLASH512_V131")) == BackupType::Flash64);
    CHECK(detect_backup_type(rom_with_id("FLASH1M_V103")) == BackupType::Flash128);
    CHECK(detect_backup_type(rom_with_id("SRAM_V113", 0x402)) == BackupType::None);  // not aligned
    CHECK(detect_backup_type(std::vector<std::uint8_t>(4096)) == BackupType::None);
}

TEST_CASE("SRAM: 32 KiB, mirrored, 8-bit bus, marks the save dirty") {
    Bus bus(rom_with_id("SRAM_V113"));
    CHECK(bus.backup().type() == BackupType::Sram);
    CHECK(bus.read8(0x0E000000) == 0xFF);
    CHECK_FALSE(bus.backup().dirty());
    bus.write16(0x0E000011, 0x3412);  // only the low byte reaches the chip
    CHECK(bus.read8(0x0E000010) == 0x12);
    CHECK(bus.read8(0x0E008010) == 0x12);  // 32 KiB mirror
    CHECK(bus.backup().dirty());
    CHECK(bus.backup().data().size() == 32 * 1024);
}

TEST_CASE("No ID string: works as SRAM, but only persists once written") {
    Bus bus(std::vector<std::uint8_t>(4096));
    CHECK(bus.backup().type() == BackupType::None);
    CHECK_FALSE(bus.backup().persists());
    bus.write8(0x0E000000, 1);
    CHECK(bus.backup().persists());
}

TEST_CASE("Flash: ID mode reports the chip, F0 leaves it") {
    Bus bus(rom_with_id("FLASH512_V131"));
    flash_command(bus, 0x90);
    CHECK(bus.read8(0x0E000000) == kFlash64Maker);
    CHECK(bus.read8(0x0E000001) == kFlash64Device);
    flash_command(bus, 0xF0);
    CHECK(bus.read8(0x0E000000) == 0xFF);

    Bus big(rom_with_id("FLASH1M_V103"));
    flash_command(big, 0x90);
    CHECK(big.read8(0x0E000000) == kFlash128Maker);
    CHECK(big.read8(0x0E000001) == kFlash128Device);
}

TEST_CASE("Flash: writes need the program command; erase restores 0xFF") {
    Bus bus(rom_with_id("FLASH_V126"));
    bus.write8(0x0E000100, 0x42);  // a plain write does nothing
    CHECK(bus.read8(0x0E000100) == 0xFF);
    flash_command(bus, 0xA0);
    bus.write8(0x0E000100, 0x42);
    CHECK(bus.read8(0x0E000100) == 0x42);
    bus.write8(0x0E000100, 0x43);  // the command covers one byte only
    CHECK(bus.read8(0x0E000100) == 0x42);
    flash_command(bus, 0xA0);
    bus.write8(0x0E001100, 0x99);  // another 4 KiB sector

    // Sector erase: AA 55 80, AA 55, then 30 at the sector.
    flash_command(bus, 0x80);
    bus.write8(0x0E005555, 0xAA);
    bus.write8(0x0E002AAA, 0x55);
    bus.write8(0x0E000000, 0x30);
    CHECK(bus.read8(0x0E000100) == 0xFF);
    CHECK(bus.read8(0x0E001100) == 0x99);

    // Chip erase: AA 55 80, AA 55 10.
    flash_command(bus, 0x80);
    flash_command(bus, 0x10);
    CHECK(bus.read8(0x0E001100) == 0xFF);
    CHECK(bus.backup().dirty());
}

TEST_CASE("Flash 128 KiB: B0 then a write to 0 switches the bank") {
    Bus bus(rom_with_id("FLASH1M_V103"));
    flash_command(bus, 0xB0);
    bus.write8(0x0E000000, 1);
    flash_command(bus, 0xA0);
    bus.write8(0x0E000010, 0x77);
    CHECK(bus.read8(0x0E000010) == 0x77);
    CHECK(bus.backup().data()[0x10010] == 0x77);
    flash_command(bus, 0xB0);
    bus.write8(0x0E000000, 0);
    CHECK(bus.read8(0x0E000010) == 0xFF);
}

TEST_CASE("EEPROM 8 KiB through DMA 3: write a block, read it back") {
    Bus bus(rom_with_id("EEPROM_V124"));
    CHECK(bus.backup().is_eeprom());
    CHECK(bus.read16(0x0D000000) == 1);  // idle: ready

    const std::uint64_t value = 0x0123456789ABCDEFULL;
    send_to_eeprom(bus, eeprom_write_request(0x3FF, 14, value));  // 81 bits: an 8 KiB chip
    CHECK(bus.backup().data().size() == 8 * 1024);
    CHECK(bus.backup().data()[0x3FF * 8] == 0x01);      // first bit = MSB of the first byte
    CHECK(bus.backup().data()[0x3FF * 8 + 7] == 0xEF);
    CHECK(bus.backup().dirty());

    send_to_eeprom(bus, eeprom_read_request(0x3FF, 14));
    CHECK(receive_from_eeprom(bus) == value);
    CHECK(bus.read16(0x0D000000, Access{}) == 1);  // back to idle after 68 bits
}

TEST_CASE("EEPROM 512 B: 6-bit addresses, learnt from a 73-bit write") {
    Bus bus(rom_with_id("EEPROM_V124"));
    send_to_eeprom(bus, eeprom_write_request(5, 6, 0xFFFF0000FFFF0000ULL));
    CHECK(bus.backup().data().size() == 512);
    send_to_eeprom(bus, eeprom_read_request(5, 6));
    CHECK(receive_from_eeprom(bus) == 0xFFFF0000FFFF0000ULL);
}

TEST_CASE("Untimed reads only peek at the EEPROM") {
    Bus bus(rom_with_id("EEPROM_V124"));
    send_to_eeprom(bus, eeprom_write_request(0, 6, 0x8000000000000000ULL));
    send_to_eeprom(bus, eeprom_read_request(0, 6));
    for (int i = 0; i < 10; ++i) CHECK(bus.read16(kEeprom) == 0);  // stays on dummy bit 0
    CHECK(receive_from_eeprom(bus) == 0x8000000000000000ULL);
}

TEST_CASE("Loading a .sav: accepted sizes and what they decide") {
    Backup sram(BackupType::Sram);
    CHECK(sram.load(std::vector<std::uint8_t>(32 * 1024, 7)));
    CHECK(sram.read8(0) == 7);
    CHECK(sram.load(std::vector<std::uint8_t>(64 * 1024, 8)));  // other emulators' 64 KiB files
    CHECK_FALSE(sram.load(std::vector<std::uint8_t>(100)));
    CHECK_FALSE(sram.dirty());

    Backup flash(BackupType::Flash64);
    CHECK(flash.load(std::vector<std::uint8_t>(128 * 1024, 0)));
    CHECK(flash.type() == BackupType::Flash128);  // the file knows the chip size

    Backup eeprom(BackupType::Eeprom);
    CHECK(eeprom.load(std::vector<std::uint8_t>(8 * 1024, 0)));
    CHECK(eeprom.data().size() == 8 * 1024);
    CHECK_FALSE(eeprom.load(std::vector<std::uint8_t>(1024)));

    Backup none(BackupType::None);
    CHECK(none.load(std::vector<std::uint8_t>(32 * 1024, 0)));
    CHECK(none.persists());
}
