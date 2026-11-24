#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>

#include "gc/mmio.hpp"

namespace ezy::gc {

class Pi;

// The external interface at 0x0C006800 (design, "Periféricos": EXI). Offsets and bit layouts
// from Dolphin's Core/HW/EXI/EXI_Channel.h/.cpp and EXI_DeviceIPL.h/.cpp (master, read on
// 2026-11-23) and YAGCD's "EXI". Three channels of five 32-bit registers, 0x14 apart (a 16-bit
// access throws, as does any other offset):
//   +0x00 CSR   EXIINTMASK 0, EXIINT 1, TCINTMASK 2, TCINT 3, CLK 4-6, CS 7-9 (one bit per
//               device: 1 device 0, 2 device 1, 4 device 2), EXTINTMASK 10, EXTINT 11, EXT 12
//               (read-only: a device in the slot), ROMDIS 13 (channel 0). Masks, CLK, CS and
//               ROMDIS are stored; writing 1 to an interrupt bit clears it. Channels 0 and 1
//               start with EXTINT set (Dolphin's constructor). Selecting a device (its CS bit
//               rising) starts a new command on it.
//   +0x04 MAR   DMA address; +0x08 LENGTH  DMA length (stored as written, as Dolphin).
//   +0x0C CR    TSTART 0, DMA 1, RW 2-3 (0 read, 1 write, 2 read-write), TLEN 4-5 (immediate
//               bytes - 1). Writing TSTART runs the transfer at once on the selected device:
//               an immediate one moves TLEN + 1 bytes through DATA, most significant first; a
//               DMA one moves LENGTH bytes between MEM1 at MAR and the device. Then TSTART
//               clears and TCINT rises.
//   +0x10 DATA  immediate data.
// The EXI cause to the PI: any channel's (EXIINT & EXIINTMASK) | (TCINT & TCINTMASK) |
// (EXTINT & EXTINTMASK).
//
// Devices: channel 0 device 1 is the IPL chip's RTC and SRAM; nothing else is plugged in
// (the memory card slots, channel 0 device 0 and channel 1 device 0, are empty: EXT reads 0).
// A transfer with no device selected, to an empty slot, in read-write mode, or a DMA that is
// not 32-byte aligned or runs past MEM1 throws BusError (at the CR write, so with the PC).
//
// The IPL chip (Dolphin's CEXIIPL): the first 4 bytes after selecting it are a command word,
// bit 31 = write, address = (command >> 6) & 0x1FFFFFF. Addresses 0x800000-0x800043 are the
// RTC (4 bytes, 0x20000000) followed by the 64-byte SRAM (0x20000100); every following byte
// moves to or from address + n, wrapping in those 0x44 bytes (as Dolphin). The RTC is
// latched when the command word completes; writes to it are accepted with no effect (the
// clock follows the host, as in Dolphin). The IPL ROM (addresses below 0x200000: the BIOS
// and its fonts, not offered), the UART and everything else throw.
class Exi final : public MmioHandler {
public:
    // Seconds since 2000-01-01 00:00:00 as the RTC reports them.
    using RtcSource = std::function<std::uint32_t()>;
    static constexpr std::size_t kSramSize = 64;

    // `mem1` is MEM1 in guest order; raises Pi::kExi on `pi`. The RTC follows host_rtc().
    Exi(Pi& pi, std::span<std::uint8_t> mem1);

    void set_rtc_source(RtcSource source) { rtc_source_ = std::move(source); }
    // The host's local time as seconds since 2000-01-01 00:00 (Dolphin's GetEmulatedTime:
    // local time since 1970 minus GC_EPOCH = 0x386D4380, so the console shows the host's
    // wall clock).
    [[nodiscard]] static std::uint32_t host_rtc();

    // The SRAM as the guest sees it (Dolphin's SramSettings + SramSettingsEx).
    [[nodiscard]] std::span<const std::uint8_t, kSramSize> sram() const { return sram_; }

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    struct Channel {
        std::uint32_t csr = 0, mar = 0, length = 0, cr = 0, data = 0;
    };

    void transfer(unsigned channel);
    // One byte to or from the IPL chip (Dolphin's CEXIIPL::TransferByte); `channel` names
    // the channel in an error.
    void ipl_byte(std::uint8_t& byte, std::uint32_t channel);
    void update_irq();

    Pi& pi_;
    std::span<std::uint8_t> mem1_;
    std::array<Channel, 3> channels_{};
    RtcSource rtc_source_;
    std::array<std::uint8_t, kSramSize> sram_{};
    // The IPL chip's command state.
    std::uint32_t ipl_command_ = 0;
    unsigned ipl_command_bytes_ = 0;
    std::uint32_t ipl_cursor_ = 0;
    std::uint32_t rtc_latch_ = 0;
};

}  // namespace ezy::gc
