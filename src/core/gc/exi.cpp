#include "gc/exi.hpp"

#include <cstdio>
#include <ctime>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Exi);
constexpr std::uint32_t kChannelStride = 0x14;

// CSR bits (Dolphin's UEXI_STATUS).
constexpr std::uint32_t kExiIntMask = 1u << 0, kExiInt = 1u << 1, kTcintMask = 1u << 2, kTcint = 1u << 3,
                        kClk = 7u << 4, kCs = 7u << 7, kExtIntMask = 1u << 10, kExtInt = 1u << 11,
                        kRomDis = 1u << 13;
constexpr std::uint32_t kIplSelect = 2u << 7;  // CS: device 1
// CR bits (UEXI_CONTROL).
constexpr std::uint32_t kTstart = 1u << 0, kDma = 1u << 1;

// The IPL chip's address space (Dolphin's EXI_DeviceIPL.h): the RTC then the SRAM.
constexpr std::uint32_t kRomEnd = 0x200000, kSramBase = 0x800000, kRtcSramSize = 0x44, kRtcSize = 4;

std::string hex32(std::uint32_t value) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%08X", value);
    return b;
}

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled external interface register at offset 0x" + b, kBase + offset);
}

[[noreturn]] void transfer_error(unsigned channel, const std::string& what) {
    throw BusError("EXI channel " + std::to_string(channel) + ": " + what,
                   kBase + channel * kChannelStride + 0x0C);
}

// Days from 1970-01-01 to the given civil date (Howard Hinnant's days_from_civil).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Dolphin's sram_dump (Core/HW/Sram.cpp) for the checksummed settings: rtc_bias 0, VI
// offset 0, NTD 0, language 0 (English), flags 0x2C (0x20 | OOBE done 0x08 | stereo 0x04).
// The extended part is zeros (no memory card flash IDs: their checksums 0x3A/0x3B are the
// complement of the IDs' byte sums, 0xFF), not Dolphin's "DOLPHINSLOTA/B". The checksum pair
// at 0x00/0x02 is FixSRAMChecksums': the 16-bit sum of the big-endian words 0x0C-0x13 and the
// sum of their complements.
std::array<std::uint8_t, Exi::kSramSize> default_sram() {
    std::array<std::uint8_t, Exi::kSramSize> s{};
    s[0x13] = 0x2C;
    s[0x3A] = s[0x3B] = 0xFF;
    std::uint16_t sum = 0, inv = 0;
    for (std::size_t at = 0x0C; at < 0x14; at += 2) {
        const auto word = static_cast<std::uint16_t>((s[at] << 8) | s[at + 1]);
        sum = static_cast<std::uint16_t>(sum + word);
        inv = static_cast<std::uint16_t>(inv + static_cast<std::uint16_t>(~word));
    }
    s[0] = static_cast<std::uint8_t>(sum >> 8);
    s[1] = static_cast<std::uint8_t>(sum);
    s[2] = static_cast<std::uint8_t>(inv >> 8);
    s[3] = static_cast<std::uint8_t>(inv);
    return s;
}
}  // namespace

std::uint32_t Exi::host_rtc() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const std::int64_t days = days_from_civil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                                              static_cast<unsigned>(local.tm_mday)) -
                              days_from_civil(2000, 1, 1);
    return static_cast<std::uint32_t>(days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
}

Exi::Exi(Pi& pi, std::span<std::uint8_t> mem1)
    : pi_(pi), mem1_(mem1), rtc_source_(&Exi::host_rtc), sram_(default_sram()) {
    channels_[0].csr = kExtInt;  // Dolphin's CEXIChannel constructor: channels 0 and 1
    channels_[1].csr = kExtInt;
}

void Exi::update_irq() {
    bool any = false;
    for (const Channel& ch : channels_) {
        any = any || ((ch.csr & kExiInt) && (ch.csr & kExiIntMask)) || ((ch.csr & kTcint) && (ch.csr & kTcintMask)) ||
              ((ch.csr & kExtInt) && (ch.csr & kExtIntMask));
    }
    pi_.set_cause(Pi::kExi, any);
}

void Exi::ipl_byte(std::uint8_t& byte, std::uint32_t channel) {
    if (ipl_command_bytes_ < 4) {  // the command word comes first, most significant byte first
        ipl_command_ = (ipl_command_ << 8) | byte;
        if (++ipl_command_bytes_ == 4) {
            rtc_latch_ = rtc_source_();  // Dolphin's UpdateRTC() on the latched command
            ipl_cursor_ = 0;
        }
        return;
    }
    const bool write = (ipl_command_ >> 31) != 0;
    const std::uint32_t address = (ipl_command_ >> 6) & 0x1FFFFFF;
    if (address >= kSramBase && address < kSramBase + kRtcSramSize) {
        const std::uint32_t at = (address - kSramBase + ipl_cursor_++) % kRtcSramSize;
        if (at < kRtcSize) {
            if (!write) byte = static_cast<std::uint8_t>(rtc_latch_ >> (24 - 8 * at));
            // A write to the RTC: accepted, no effect (the clock follows the host).
        } else if (write) {
            sram_[at - kRtcSize] = byte;
        } else {
            byte = sram_[at - kRtcSize];
        }
        return;
    }
    if (address < kRomEnd) {
        transfer_error(channel, "IPL ROM access (the BIOS and its fonts are not offered), command " + hex32(ipl_command_));
    }
    transfer_error(channel, "IPL chip command " + hex32(ipl_command_) + " (UART or unknown region) is not modeled");
}

void Exi::transfer(unsigned channel) {
    Channel& ch = channels_[channel];
    const std::uint32_t cs = (ch.csr & kCs) >> 7;
    if (cs == 0) transfer_error(channel, "transfer with no device selected");
    if (cs != 1 && cs != 2 && cs != 4) transfer_error(channel, "transfer with several devices selected");
    const unsigned device = cs == 1 ? 0 : cs == 2 ? 1 : 2;
    // Serial port 1 (channel 0, device 2) holds Dolphin's default None device, which drives
    // nothing: reads give 0 and writes vanish.
    const bool serial_port_1 = channel == 0 && device == 2;
    if (channel != 0 || (device != 1 && !serial_port_1)) {
        transfer_error(channel, "device " + std::to_string(device) + " has nothing plugged in (empty slot)");
    }
    const std::uint32_t rw = (ch.cr >> 2) & 3;
    if (rw > 1) transfer_error(channel, "read-write transfers are not modeled");
    const auto move_byte = [&](std::uint8_t& byte) {
        if (serial_port_1) byte = 0;
        else ipl_byte(byte, channel);
    };
    if (!(ch.cr & kDma)) {
        const unsigned bytes = ((ch.cr >> 4) & 3) + 1;
        if (rw == 0) {
            std::uint32_t result = 0;
            for (unsigned i = 0; i < bytes; ++i) {
                std::uint8_t byte = 0;
                move_byte(byte);
                result |= std::uint32_t{byte} << (24 - 8 * i);
            }
            ch.data = result;
        } else {
            for (unsigned i = 0; i < bytes; ++i) {
                auto byte = static_cast<std::uint8_t>(ch.data >> (24 - 8 * i));
                move_byte(byte);
            }
        }
    } else {
        if ((ch.mar & 31) || (ch.length & 31)) {
            transfer_error(channel, "DMA at " + hex32(ch.mar) + ", " + hex32(ch.length) + " bytes, is not 32-byte aligned");
        }
        if (std::uint64_t{ch.mar} + ch.length > mem1_.size()) {
            transfer_error(channel, "DMA at " + hex32(ch.mar) + ", " + hex32(ch.length) + " bytes, runs past MEM1");
        }
        for (std::uint32_t i = 0; i < ch.length; ++i) {  // Dolphin's DMARead / DMAWrite, byte by byte
            std::uint8_t byte = rw == 0 ? 0 : mem1_[ch.mar + i];
            move_byte(byte);
            if (rw == 0) mem1_[ch.mar + i] = byte;  // a DMA write never changes MEM1
        }
    }
    ch.cr &= ~kTstart;
    ch.csr |= kTcint;
}

std::uint32_t Exi::read32(std::uint32_t offset) {
    if (offset >= 3 * kChannelStride || (offset & 3)) unmodeled("read", offset);
    const Channel& ch = channels_[offset / kChannelStride];
    switch (offset % kChannelStride) {
        case 0x00: return ch.csr;  // EXT (bit 12) stays 0: no card in either slot
        case 0x04: return ch.mar;
        case 0x08: return ch.length;
        case 0x0C: return ch.cr;
        default: return ch.data;
    }
}

void Exi::write32(std::uint32_t offset, std::uint32_t value) {
    if (offset >= 3 * kChannelStride || (offset & 3)) unmodeled("write", offset);
    const unsigned channel = offset / kChannelStride;
    Channel& ch = channels_[channel];
    switch (offset % kChannelStride) {
        case 0x00: {
            std::uint32_t stored = kExiIntMask | kTcintMask | kClk | kCs, clears = kExiInt | kTcint;
            if (channel < 2) {
                stored |= kExtIntMask;
                clears |= kExtInt;
            }
            if (channel == 0) stored |= kRomDis;
            const bool ipl_selected = channel == 0 && !(ch.csr & kIplSelect) && (value & kIplSelect);
            ch.csr = ((ch.csr & ~stored) | (value & stored)) & ~(value & clears);
            if (ipl_selected) {  // Dolphin's CEXIIPL::SetCS: a new command begins
                ipl_command_bytes_ = 0;
                ipl_cursor_ = 0;
            }
            update_irq();
            return;
        }
        case 0x04: ch.mar = value; return;
        case 0x08: ch.length = value; return;
        case 0x0C:
            ch.cr = value & 0x3F;
            if (ch.cr & kTstart) {
                try {
                    transfer(channel);
                } catch (...) {
                    ch.cr &= ~kTstart;  // the machine stops; leave the channel idle
                    throw;
                }
                update_irq();
            }
            return;
        default: ch.data = value; return;
    }
}

std::uint16_t Exi::read16(std::uint32_t offset) { unmodeled("16-bit read (the register is 32 bits)", offset); }

void Exi::write16(std::uint32_t offset, std::uint16_t) {
    unmodeled("16-bit write (the register is 32 bits)", offset);
}

}  // namespace ezy::gc
