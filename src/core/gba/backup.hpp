#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ezy::gba {

// Cartridge save memory ("backup", GBATEK "GBA Cartridge Backup IDs").
enum class BackupType : std::uint8_t {
    None,      // no ID string found: behaves as SRAM, but only saved if the game writes it
    Sram,      // 32 KiB battery-backed SRAM at 0x0E000000
    Flash64,   // 64 KiB flash, command interface at 0x0E000000
    Flash128,  // 128 KiB flash: two 64 KiB banks
    Eeprom,    // 512 B or 8 KiB serial EEPROM in the top of the ROM area, used through DMA 3
};

// Nintendo's SDK puts an ID string in the ROM ("SRAM_V", "FLASH1M_V", "EEPROM_V", ...),
// word aligned. That's how emulators (and we) know which chip the cartridge has.
[[nodiscard]] BackupType detect_backup_type(std::span<const std::uint8_t> rom);
[[nodiscard]] std::string_view backup_type_name(BackupType type);

// Flash chip IDs reported in ID mode: the ones mGBA uses, which games accept.
inline constexpr std::uint8_t kFlash64Maker = 0x32;    // Panasonic
inline constexpr std::uint8_t kFlash64Device = 0x1B;
inline constexpr std::uint8_t kFlash128Maker = 0x62;   // Sanyo
inline constexpr std::uint8_t kFlash128Device = 0x13;

// The save chip: storage plus the protocol each kind speaks.
//
// SRAM: plain bytes. Flash: a command state machine (AA to 5555, 55 to 2AAA, then the
// command) for ID mode, chip/sector erase, byte program and bank switching. EEPROM: a
// bit-serial protocol, one bit per halfword access; the address width (6 bits for 512 B,
// 14 for 8 KiB) is learnt from the first DMA transfer length or from a loaded save.
//
// Everything completes instantly: erase/program report "ready" at once.
class Backup {
public:
    explicit Backup(BackupType type);

    [[nodiscard]] BackupType type() const { return type_; }

    // 0x0E000000 region (SRAM/flash), 8-bit bus. `offset` is the address within 64 KiB.
    [[nodiscard]] std::uint8_t read8(std::uint32_t offset) const;
    void write8(std::uint32_t offset, std::uint8_t value);

    // EEPROM, one bit at a time (bit 0 of a halfword access).
    [[nodiscard]] bool is_eeprom() const { return type_ == BackupType::Eeprom; }
    std::uint16_t eeprom_read();                       // advances the serial output
    [[nodiscard]] std::uint16_t eeprom_peek() const;   // same value, no side effects (debugger)
    void eeprom_write(std::uint16_t value);
    // DMA 3 is about to move `count` halfwords to the EEPROM: 9 or 73 means a 512 B chip,
    // 17 or 81 an 8 KiB one (read request / write request lengths).
    void eeprom_dma_hint(std::uint32_t count);

    // The bytes a .sav file holds. Empty for a chip that was never touched (None).
    [[nodiscard]] std::span<const std::uint8_t> data() const { return data_; }
    // Loads a .sav file. Accepts any size this chip kind can have (and resizes to it:
    // a 512 B EEPROM file fixes the address width). Returns false if the size doesn't fit.
    bool load(std::span<const std::uint8_t> data);
    // Whether this cartridge has something worth writing to a .sav file.
    [[nodiscard]] bool persists() const { return type_ != BackupType::None || written_; }
    [[nodiscard]] bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

private:
    enum class FlashState : std::uint8_t { Ready, Unlock1, Unlock2 };
    enum class FlashNext : std::uint8_t { None, EraseArmed, Program, Bank };
    enum class EepromState : std::uint8_t { Command, Address, WriteData, StopBit, Reading };

    void flash_write(std::uint32_t offset, std::uint8_t value);
    void flash_command(std::uint8_t command);
    void eeprom_set_size(std::size_t size);
    void touch() { dirty_ = written_ = true; }

    BackupType type_;
    std::vector<std::uint8_t> data_;

    // Flash
    FlashState flash_state_ = FlashState::Ready;
    FlashNext flash_next_ = FlashNext::None;
    bool erase_prefix_ = false;  // 0x80 received: the next command is an erase
    bool id_mode_ = false;
    std::uint32_t bank_ = 0;  // 0 or 1, 128 KiB flash only

    // EEPROM
    int address_bits_ = 6;
    bool size_known_ = false;
    EepromState eeprom_state_ = EepromState::Command;
    int bits_left_ = 2;
    std::uint32_t command_ = 0;
    std::uint32_t address_ = 0;
    std::uint64_t buffer_ = 0;
    int read_bit_ = 0;  // 0..67 while Reading: 4 dummy bits, then 64 data bits

    bool dirty_ = false;
    bool written_ = false;
};

}  // namespace ezy::gba
