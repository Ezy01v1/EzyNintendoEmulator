#include "gba/backup.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace ezy::gba {

namespace {

constexpr std::size_t kSramBytes = 32 * 1024;
constexpr std::size_t kFlashBank = 64 * 1024;
constexpr std::size_t kEepromSmall = 512;
constexpr std::size_t kEepromLarge = 8 * 1024;
constexpr std::uint8_t kErased = 0xFF;

// Flash command addresses and commands.
constexpr std::uint32_t kFlashCmdAddr1 = 0x5555;
constexpr std::uint32_t kFlashCmdAddr2 = 0x2AAA;
constexpr std::uint32_t kFlashSectorSize = 4 * 1024;

// EEPROM request bits.
constexpr std::uint32_t kEepromRead = 0b11;
constexpr std::uint32_t kEepromWrite = 0b10;
constexpr int kEepromDummyBits = 4;
constexpr int kEepromDataBits = 64;

}  // namespace

BackupType detect_backup_type(std::span<const std::uint8_t> rom) {
    // Longest first wouldn't matter (each ID ends in "_V"), but SRAM_F_V must map to SRAM.
    static constexpr std::array<std::pair<std::string_view, BackupType>, 6> kIds = {{
        {"EEPROM_V", BackupType::Eeprom},
        {"SRAM_V", BackupType::Sram},
        {"SRAM_F_V", BackupType::Sram},
        {"FLASH_V", BackupType::Flash64},
        {"FLASH512_V", BackupType::Flash64},
        {"FLASH1M_V", BackupType::Flash128},
    }};
    for (std::size_t at = 0; at + 4 <= rom.size(); at += 4) {
        if (rom[at] != 'E' && rom[at] != 'S' && rom[at] != 'F') continue;  // cheap first filter
        for (const auto& [id, type] : kIds) {
            if (at + id.size() > rom.size()) continue;
            if (std::equal(id.begin(), id.end(), rom.begin() + static_cast<std::ptrdiff_t>(at))) return type;
        }
    }
    return BackupType::None;
}

std::string_view backup_type_name(BackupType type) {
    switch (type) {
        case BackupType::None: return "none";
        case BackupType::Sram: return "SRAM 32 KiB";
        case BackupType::Flash64: return "Flash 64 KiB";
        case BackupType::Flash128: return "Flash 128 KiB";
        case BackupType::Eeprom: return "EEPROM";
    }
    return "?";
}

Backup::Backup(BackupType type) : type_(type) {
    switch (type) {
        case BackupType::None:
        case BackupType::Sram: data_.assign(kSramBytes, kErased); break;
        case BackupType::Flash64: data_.assign(kFlashBank, kErased); break;
        case BackupType::Flash128: data_.assign(2 * kFlashBank, kErased); break;
        case BackupType::Eeprom: eeprom_set_size(kEepromSmall); break;
    }
}

std::uint8_t Backup::read8(std::uint32_t offset) const {
    offset &= 0xFFFF;
    switch (type_) {
        case BackupType::None:
        case BackupType::Sram: return data_[offset & (kSramBytes - 1)];  // 32 KiB, mirrored
        case BackupType::Flash64:
        case BackupType::Flash128:
            if (id_mode_ && offset <= 1) {
                const bool big = type_ == BackupType::Flash128;
                if (offset == 0) return big ? kFlash128Maker : kFlash64Maker;
                return big ? kFlash128Device : kFlash64Device;
            }
            return data_[bank_ * kFlashBank + offset];
        case BackupType::Eeprom: return kErased;  // nothing at 0x0E000000
    }
    return kErased;
}

void Backup::write8(std::uint32_t offset, std::uint8_t value) {
    offset &= 0xFFFF;
    switch (type_) {
        case BackupType::None:
        case BackupType::Sram:
            data_[offset & (kSramBytes - 1)] = value;
            touch();
            break;
        case BackupType::Flash64:
        case BackupType::Flash128: flash_write(offset, value); break;
        case BackupType::Eeprom: break;
    }
}

void Backup::flash_write(std::uint32_t offset, std::uint8_t value) {
    // A pending single-write command takes the next write, whatever its address.
    if (flash_next_ == FlashNext::Program) {
        data_[bank_ * kFlashBank + offset] = value;
        flash_next_ = FlashNext::None;
        touch();
        return;
    }
    if (flash_next_ == FlashNext::Bank) {
        if (offset == 0) bank_ = value & 1U;
        flash_next_ = FlashNext::None;
        return;
    }

    switch (flash_state_) {
        case FlashState::Ready:
            if (offset == kFlashCmdAddr1 && value == 0xAA) flash_state_ = FlashState::Unlock1;
            break;
        case FlashState::Unlock1:
            flash_state_ = offset == kFlashCmdAddr2 && value == 0x55 ? FlashState::Unlock2 : FlashState::Ready;
            break;
        case FlashState::Unlock2:
            flash_state_ = FlashState::Ready;
            if (erase_prefix_ && value == 0x30) {
                // Sector erase: the address picks the 4 KiB sector.
                const std::size_t start = bank_ * kFlashBank + (offset & ~(kFlashSectorSize - 1));
                std::fill_n(data_.begin() + static_cast<std::ptrdiff_t>(start), kFlashSectorSize, kErased);
                erase_prefix_ = false;
                touch();
            } else if (offset == kFlashCmdAddr1) {
                flash_command(value);
            }
            break;
    }
}

void Backup::flash_command(std::uint8_t command) {
    const bool erase = std::exchange(erase_prefix_, false);
    switch (command) {
        case 0x90: id_mode_ = true; break;   // enter ID mode
        case 0xF0: id_mode_ = false; break;  // leave ID mode / reset
        case 0x80: erase_prefix_ = true; break;
        case 0x10:  // chip erase (after 0x80)
            if (erase) {
                std::fill(data_.begin(), data_.end(), kErased);
                touch();
            }
            break;
        case 0xA0: flash_next_ = FlashNext::Program; break;
        case 0xB0:
            if (type_ == BackupType::Flash128) flash_next_ = FlashNext::Bank;
            break;
        default: break;
    }
}

void Backup::eeprom_set_size(std::size_t size) {
    data_.resize(size, kErased);
    address_bits_ = size == kEepromSmall ? 6 : 14;
}

void Backup::eeprom_dma_hint(std::uint32_t count) {
    if (!is_eeprom() || size_known_ || eeprom_state_ != EepromState::Command) return;
    if (count == 9 || count == 73) {
        eeprom_set_size(kEepromSmall);
        size_known_ = true;
    } else if (count == 17 || count == 81) {
        eeprom_set_size(kEepromLarge);
        size_known_ = true;
    }
}

void Backup::eeprom_write(std::uint16_t value) {
    if (!is_eeprom()) return;
    const std::uint32_t bit = value & 1U;
    if (eeprom_state_ == EepromState::Reading) {
        // A new request abandons an unfinished read.
        eeprom_state_ = EepromState::Command;
        command_ = 0;
        bits_left_ = 2;
    }
    const std::uint32_t blocks = static_cast<std::uint32_t>(data_.size() / 8);  // 8-byte blocks
    switch (eeprom_state_) {
        case EepromState::Command:
            command_ = (command_ << 1) | bit;
            if (--bits_left_ > 0) return;
            if (command_ == kEepromRead || command_ == kEepromWrite) {
                eeprom_state_ = EepromState::Address;
                bits_left_ = address_bits_;
                address_ = 0;
            } else {
                command_ = 0;  // not a request: start over
                bits_left_ = 2;
            }
            break;
        case EepromState::Address:
            address_ = (address_ << 1) | bit;
            if (--bits_left_ > 0) return;
            address_ %= blocks;  // 8 KiB chips take 14 bits but only use 10
            if (command_ == kEepromWrite) {
                eeprom_state_ = EepromState::WriteData;
                bits_left_ = kEepromDataBits;
                buffer_ = 0;
            } else {
                eeprom_state_ = EepromState::StopBit;
            }
            break;
        case EepromState::WriteData:
            buffer_ = (buffer_ << 1) | bit;
            if (--bits_left_ == 0) eeprom_state_ = EepromState::StopBit;
            break;
        case EepromState::StopBit:
            if (command_ == kEepromWrite) {
                // 64 bits, first bit = most significant bit of the block's first byte.
                for (std::size_t i = 0; i < 8; ++i) {
                    data_[address_ * 8 + i] = static_cast<std::uint8_t>(buffer_ >> (56 - 8 * i));
                }
                touch();
                eeprom_state_ = EepromState::Command;
            } else {
                eeprom_state_ = EepromState::Reading;
                read_bit_ = 0;
            }
            command_ = 0;
            bits_left_ = 2;
            break;
        case EepromState::Reading: break;  // handled above
    }
}

std::uint16_t Backup::eeprom_peek() const {
    if (!is_eeprom() || eeprom_state_ != EepromState::Reading) return 1;  // idle/write done: "ready"
    if (read_bit_ < kEepromDummyBits) return 0;
    const auto k = static_cast<std::size_t>(read_bit_ - kEepromDummyBits);
    return static_cast<std::uint16_t>((data_[address_ * 8 + k / 8] >> (7 - k % 8)) & 1U);
}

std::uint16_t Backup::eeprom_read() {
    const std::uint16_t bit = eeprom_peek();
    if (is_eeprom() && eeprom_state_ == EepromState::Reading &&
        ++read_bit_ == kEepromDummyBits + kEepromDataBits) {
        eeprom_state_ = EepromState::Command;
    }
    return bit;
}

bool Backup::load(std::span<const std::uint8_t> data) {
    switch (type_) {
        case BackupType::None:
        case BackupType::Sram:
            // Some emulators write 64 KiB for SRAM games: the first 32 KiB are the chip.
            if (data.size() != kSramBytes && data.size() != 2 * kSramBytes) return false;
            std::copy_n(data.begin(), kSramBytes, data_.begin());
            written_ = true;  // a save exists: keep writing it back
            break;
        case BackupType::Flash64:
        case BackupType::Flash128:
            // Trust the file over the ID string: it says how big the chip was.
            if (data.size() == kFlashBank) {
                type_ = BackupType::Flash64;
            } else if (data.size() == 2 * kFlashBank) {
                type_ = BackupType::Flash128;
            } else {
                return false;
            }
            data_.assign(data.begin(), data.end());
            break;
        case BackupType::Eeprom:
            if (data.size() != kEepromSmall && data.size() != kEepromLarge) return false;
            eeprom_set_size(data.size());
            size_known_ = true;
            std::copy(data.begin(), data.end(), data_.begin());
            break;
    }
    dirty_ = false;
    return true;
}

}  // namespace ezy::gba
