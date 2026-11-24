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

// The inverse, for the EFB copy's clear color (GxFifo): RGB to BT.601 limited range, in 8.8
// fixed point, Y = 16 + (66 R + 129 G + 25 B + 128) >> 8, U = 128 + (-38 R - 74 G + 112 B +
// 128) >> 8, V = 128 + (112 R - 94 G - 18 B + 128) >> 8 (the shifts floor). Black is (16,
// 128, 128) and white (235, 128, 128): both come back exact through yuv_to_argb.
struct Yuv {
    std::uint8_t y = 0, u = 0, v = 0;
};
[[nodiscard]] Yuv rgb_to_yuv(int r, int g, int b);

// The video interface at 0x0C002000: registers, the frame timing and the display interrupts
// (YAGCD "Video Interface"; Dolphin's Core/HW/VideoInterface.h/.cpp, read on 2026-11-22,
// whose register offsets and bit layouts these follow).
//
// Every register is stored as 16-bit halves (a 32-bit access is the high half at the lower
// offset, then the low half). Valid offsets are [0x00, 0x76):
//   00 VTR vertical timing (EQU bits 0-3, ACV bits 4-13 = active lines per field)
//   02 DCR display config: ENB 0, RST 1, NIN 2 (non-interlaced), DLR 3, LE0 4-5, LE1 6-7,
//        FMT 8-9. RST is not stored: writing it clears the four DI registers (INT included),
//        as Dolphin does.
//   04/08 HTR0/HTR1 (HTR0's HLW = bits 0-9 of the 32-bit value, the half-line width in
//        samples; 429 at reset)   0C/10 VTO/VTE   14/18 BBEI/BBOI
//   1C/20/24/28 TFBL/TFBR/BFBL/BFBR: FBB bits 0-23 (the XFB address), XOFF 24-27 (ignored),
//        POFF 28 (the address is FBB << 5; the top register's POFF applies to the bottom
//        one too), CLRPOFF 29-31 (writing it nonzero clears that register's POFF)
//   2C DPV (1..525), 2E DPH  the beam position, read-only, derived from the scheduler's time
//   30/34/38/3C DI0-DI3: HCT bits 0-10, VCT bits 16-26 (lines 1..525), ENB bit 28, INT bit 31.
//        The high half is stored as written (INT included, as Dolphin: the SDK clears INT by
//        writing the register back with bit 31 = 0). INT is set when the beam reaches
//        VCT/HCT whether ENB is set or not; ENB only gates the interrupt line to the PI.
//   40/44 DL0/DL1
//   48 picture configuration (Dolphin's PictureConfigurationRegister; YAGCD's HSW): STD bits
//        0-7 = the XFB line stride in 32-byte units, WPL bits 8-14 = words per line (40 =
//        640 pixels; more than 40 throws: wider lines are not modeled); 40/40 at reset.   4A HSR   4C..64 FCT0-FCT6   68 AA   6C VICLK
//        6E DTV   70 FBWIDTH   72 HBE (border blank end)   74 HBS (border blank start)
// Everything except DPV/DPH, the DI registers, DCR.RST and the XFB fields is a store-only
// register: accepted, no effect on the emulated picture. Any other offset throws BusError.
// Reset values follow Dolphin's VideoInterface::Preset(): VTR EQU 6, DCR ENB, DI0 = line 263
// / HCT 430 / enabled, DI1 = line 1 / HCT 1 / enabled, the DIs not pending.
//
// Timing: NTSC, 525 lines per frame = 1050 half-lines of 15444 Gekko cycles (858 samples at
// 13.5 MHz = 36 CPU cycles each, per line). A field is 525 half-lines = 8,108,100 cycles =
// 59.94 Hz; the frame is two fields. Lines run 1..525 over the frame. Every half-line, a DI
// register with VCT == 1 + half_line / 2 whose HCT > HLW matches the second half-line of that
// line and otherwise the first (Dolphin's Update()).
class Vi final : public MmioHandler {
public:
    static constexpr std::uint64_t kCyclesPerHalfLine = 15444;
    static constexpr std::uint32_t kHalfLinesPerField = 525;
    static constexpr std::uint32_t kHalfLinesPerFrame = 2 * kHalfLinesPerField;
    static constexpr std::uint64_t kCyclesPerField = kCyclesPerHalfLine * kHalfLinesPerField;
    static constexpr int kXfbWidth = 640;

    // Schedules its first half-line event on `scheduler`; raises Pi::kVi on `pi`.
    Vi(Scheduler& scheduler, Pi& pi);

    // Handles the EventType::ViLine event that was due at `when`: advances a half-line,
    // checks the display interrupts, reschedules. True when it started a new field. The
    // construction itself checks half-line 0 (line 1 of the first field).
    bool on_event(std::uint64_t when);

    [[nodiscard]] std::uint64_t fields() const { return fields_; }
    // Which field the beam is in: false = top (odd lines; the first after a reset).
    [[nodiscard]] bool bottom_field() const { return (fields_ & 1) != 0; }

    [[nodiscard]] std::uint32_t xfb_top() const;
    [[nodiscard]] std::uint32_t xfb_bottom() const;
    [[nodiscard]] int active_lines() const { return (regs_[0x00 / 2] >> 4) & 0x3FF; }  // VTR ACV
    [[nodiscard]] bool interlaced() const { return (regs_[0x02 / 2] & 0x4) == 0; }      // DCR NIN clear

    // The XFB(s) as a picture. `ram` is MEM1 (guest order). Width: the picture configuration's
    // WPL * 16 pixels (WPL counts 32-byte words per line; MKDD's XFB is 608 wide; at most 40
    // words, kXfbWidth pixels, which is also the width when WPL = 0). The line stride comes from its STD
    // (STD * 32 bytes, 2560 for an interleaved 640-wide double-field buffer; 2 * width bytes if
    // STD = 0). Interlaced: ACV*2 rows, even rows from the top XFB, odd rows from the bottom
    // one. Non-interlaced: the top XFB only, each line doubled when ACV <= 240 (240p), else as
    // is (480p). ACV = 0 (VI not set up) is treated as 480 rows. An XFB outside MEM1 gives
    // black.
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
    void check_half_line();

    Scheduler& sched_;
    Pi& pi_;
    std::array<std::uint16_t, kRegisterBytes / 2> regs_{};
    std::uint32_t half_line_ = 0;      // 0..1049 within the frame
    std::uint64_t frame_start_ = 0;    // scheduler time the frame began
    std::uint64_t fields_ = 0;
};

}  // namespace ezy::gc
