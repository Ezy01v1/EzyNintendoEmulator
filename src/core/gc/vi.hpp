#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "gc/mmio.hpp"

namespace ezy::gc {

class Pi;
class Scheduler;

// A picture: ARGB 0xAARRGGBB, row-major.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint32_t> argb;
};

// One YUV 4:2:2 pixel to ARGB, BT.601 limited range (Y 16..235, U/V centred on 128), in 8.8
// fixed point: R = 1.164 (Y-16) + 1.596 (V-128), G = 1.164 (Y-16) - 0.391 (U-128) - 0.813
// (V-128), B = 1.164 (Y-16) + 2.018 (U-128); clamped to 0..255, alpha 0xFF.
[[nodiscard]] std::uint32_t yuv_to_argb(int y, int u, int v);

// The video interface at 0x0C002000: registers, the field timing and the display interrupts
// (YAGCD "Video Interface"; Dolphin's Core/HW/VideoInterface.h/.cpp, whose register offsets
// these follow: VideoInterface.h's VI_VERTICAL_TIMING ... VI_BORDER_BLANK_START).
//
// Every register is stored as 16-bit halves (a 32-bit access is the high half at the lower
// offset, then the low half). Offsets, all valid ones in [0x00, 0x76):
//   00 VTR vertical timing (ACV, bits 4-13, = active lines per field)    02 DCR display config
//   (ENB bit 0, RST 1, NIN 2 = non-interlaced, DLR 3, LE0/LE1, FMT)
//   04/08 HTR0/HTR1   0C/10 VTO/VTE   14/18 BBEI/BBOI
//   1C/20/24/28 TFBL/TFBR/BFBL/BFBR  (FBB bits 0-23 = the XFB address, XOFF bits 24-27, and
//        bit 28 = shift: the address is FBB << 5)
//   2C DPV, 2E DPH  current beam position, read-only, derived from the scheduler's time
//   30/34/38/3C DI0-DI3  display interrupts: VCT bits 16-26 (line, from 1), ENB bit 28, INT
//        bit 31. INT is set when the beam reaches VCT while ENB; writing the register with
//        bit 31 = 0 clears it (a 1 never sets it), as Dolphin does.
//   40/44 DL0/DL1   48 HSW   4A HSR   4C..64 FCT0-FCT6   68 AA   6C VICLK   6E DTV
//   70 FBWIDTH   72 HBE (border blank end)   74 HBS (border blank start)
// Everything except DPV/DPH and the DI registers' INT is a store-only register: accepted,
// no effect on the emulated picture (the timing is always NTSC 525-line interlaced).
// Any other offset throws BusError.
//
// Timing: one field = 525 half-lines of 15444 Gekko cycles (858 samples at 13.5 MHz = 36
// CPU cycles each, per line; 486 MHz / 59.94 Hz = 8,108,100 cycles per field). The first
// half-line of a field is line 1. A DI line is checked at the start of its line, the
// horizontal count (HCT) is ignored.
class Vi final : public MmioHandler {
public:
    static constexpr std::uint64_t kCyclesPerHalfLine = 15444;
    static constexpr std::uint32_t kHalfLinesPerField = 525;
    static constexpr std::uint64_t kCyclesPerField = kCyclesPerHalfLine * kHalfLinesPerField;
    static constexpr int kXfbWidth = 640;

    // Schedules its first half-line event on `scheduler`; raises Pi::kVi on `pi`.
    Vi(Scheduler& scheduler, Pi& pi);

    // Handles the EventType::ViLine event that was due at `when`: advances a half-line,
    // checks the display interrupts, reschedules. True when it started a new field.
    bool on_event(std::uint64_t when);

    [[nodiscard]] std::uint64_t fields() const { return fields_; }
    // Which field the beam is in: false = top (odd lines; the first after a reset).
    [[nodiscard]] bool bottom_field() const { return (fields_ & 1) != 0; }

    [[nodiscard]] std::uint32_t xfb_top() const;
    [[nodiscard]] std::uint32_t xfb_bottom() const;
    [[nodiscard]] int active_lines() const { return (regs_[0x00 / 2] >> 4) & 0x3FF; }  // VTR ACV
    [[nodiscard]] bool interlaced() const { return (regs_[0x02 / 2] & 0x4) == 0; }      // DCR NIN clear

    // The XFB(s) as a picture. `ram` is MEM1 (guest order). Interlaced: ACV*2 rows, even rows
    // from the top XFB, odd rows from the bottom one. Non-interlaced: the top XFB only, each
    // line doubled when ACV <= 240 (240p), else as is (480p). ACV = 0 (VI not set up) is
    // treated as 480 rows. Width is kXfbWidth; the line stride 2 * kXfbWidth bytes. An XFB
    // outside MEM1 gives black.
    [[nodiscard]] Image xfb_image(std::span<const std::uint8_t> ram) const;

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    static constexpr std::uint32_t kRegisterBytes = 0x76;
    [[nodiscard]] std::uint32_t reg32(std::uint32_t offset) const {
        return (std::uint32_t{regs_[offset / 2]} << 16) | regs_[offset / 2 + 1];
    }
    void update_irq();
    void check_lines();

    Scheduler& sched_;
    Pi& pi_;
    std::array<std::uint16_t, kRegisterBytes / 2> regs_{};
    std::uint32_t half_line_ = 0;      // 0..524 within the field
    std::uint64_t field_start_ = 0;    // scheduler time the field began
    std::uint64_t fields_ = 0;
};

}  // namespace ezy::gc
