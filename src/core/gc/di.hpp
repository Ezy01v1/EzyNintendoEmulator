#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gc/mmio.hpp"

namespace ezy::gc {

class Disc;
class Pi;
class Scheduler;

// The DVD interface at 0x0C006000 (design, "Disco": "Interfaz DI"). Offsets and bit layouts
// from Dolphin's Core/HW/DVD/DVDInterface.cpp/.h (master, read on 2026-11-23) and YAGCD's
// "DVD Interface". Every register is 32 bits; a 16-bit access throws BusError, as does any
// offset not listed.
//   0x00 DISR   status: BREAK 0, DEINTMASK 1, DEINT 2, TCINTMASK 3, TCINT 4, BRKINTMASK 5,
//               BRKINT 6. The masks are stored; writing 1 to an interrupt bit clears it.
//               BREAK (abort the command) is not modeled and throws.
//   0x04 DICVR  cover: CVR 0 (1 = open), CVRINTMASK 1, CVRINT 2 (write 1 clears). The cover
//               is closed with the disc in, so CVR reads 0 and CVRINT never rises.
//   0x08-0x10   DICMDBUF0-2, the command words (stored).
//   0x14 DIMAR  DMA address, masked with 0x03FFFFE0 (Dolphin's GameCube mask ~0xFC00001F).
//   0x18 DILENGTH DMA length, masked with ~0x1F.
//   0x1C DICR   control: TSTART 0, DMA 1, RW 2 (stored as val & 7, as Dolphin). Writing
//               TSTART runs the command in DICMDBUF0's top byte.
//   0x20 DIIMMBUF immediate data (stored; Request Error and the audio status answer here).
//   0x24 DICFG  configuration, read-only: 1 (Dolphin: "disable bootrom descrambler").
//
// Commands (Dolphin's DICommand, ExecuteCommand). Each one finishes kCommandCycles (50 us)
// plus its DMA bytes at 3.1 MB/s after TSTART, scheduled (EventType::DiTransfer); then TSTART
// clears, DIMAR advances by the DMA length, DILENGTH drops to 0 and TCINT rises (and the PI
// DI cause, when TCINTMASK is set). A DMA lands in MEM1 at that moment, not before.
//   0x12 Inquiry: DMAs 0x20 bytes of drive information (Dolphin's words 0x00000002,
//        0x20060526, 0x41000000, then zeros). Needs DICR.DMA and DILENGTH = 0x20.
//   0xA8 Read: subcommand (DICMDBUF0 & 0xFF) 0x00 reads DICMDBUF2 bytes at DICMDBUF1 << 2;
//        0x40 reads the disc ID (0x20 bytes at 0). Needs DICR.DMA and DILENGTH equal to the
//        length. Counted in reads().
//   0xAB Seek: no effect (Dolphin ignores it too).
//   0xE0 Request Error: DIIMMBUF = drive state << 24 | error = 0 (ready, no error).
//   0xE1 Audio Stream, 0xE4 Audio Buffer Config: accepted, no effect (streaming is off).
//   0xE2 Request Audio Status, subcommands 0-3: DIIMMBUF = 0 (not playing, no address/length).
// Anything else (another command, a subcommand, a DMA write, a read without DMA) throws
// BusError naming the command at TSTART, so the PC that issued it is known. So does a DMA
// whose MEM1 range (DIMAR + DILENGTH) passes the end of MEM1, or a read past the disc's
// end: nothing is copied and nothing is scheduled.
class Di final : public MmioHandler {
public:
    static constexpr std::uint64_t kCommandCycles = 24300;  // 50 us at 486 MHz
    // How long a command with a DMA of `bytes` takes: 50 us + bytes / 3.1 MB/s, in CPU cycles.
    [[nodiscard]] static constexpr std::uint64_t read_cycles(std::uint32_t bytes) {
        return kCommandCycles + std::uint64_t{bytes} * 4860 / 31;  // 486e6 / 3.1e6 = 4860 / 31
    }

    // `mem1` is MEM1 in guest order (Memory::mem1()); raises Pi::kDi on `pi`.
    Di(Scheduler& scheduler, Pi& pi, std::span<std::uint8_t> mem1, Disc& disc);

    // Handles EventType::DiTransfer: the command in flight finishes.
    void on_event();

    // Read commands (0xA8) started since the boot, for Stats.
    [[nodiscard]] std::uint64_t reads() const { return reads_; }

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    // Runs DICMDBUF0's command for the DICR value `dicr` (TSTART set); throws before
    // changing anything when the command is not modeled or its DMA does not fit.
    void start_command(std::uint32_t dicr);
    // Checks DICR.DMA, DILENGTH == `bytes` and the MEM1 range, then sizes the data to land.
    void start_dma(std::uint32_t dicr, std::uint32_t bytes);
    void update_irq();

    Scheduler& sched_;
    Pi& pi_;
    std::span<std::uint8_t> mem1_;
    Disc& disc_;
    std::uint32_t disr_ = 0, dicvr_ = 0, dimar_ = 0, dilength_ = 0, dicr_ = 0, diimmbuf_ = 0;
    std::array<std::uint32_t, 3> cmd_{};
    std::vector<std::uint8_t> pending_;  // the DMA data of the command in flight
    std::uint32_t dma_address_ = 0;      // where it lands (DIMAR at TSTART, range-checked)
    std::uint64_t reads_ = 0;
};

}  // namespace ezy::gc
