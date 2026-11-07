#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "n64/cartridge.hpp"
#include "n64/mi.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Video Interface (registers at 0x04400000, n64brew "Video Interface").
//
// Timing: one ViLine event per scan line. VI_V_CURRENT counts half-lines, so it advances
// by 2 per line; a field has VI_V_SYNC + 1 half-lines (525 NTSC, 625 PAL by default), and
// its length follows the TV standard: 60/1.001 Hz (NTSC, MPAL) or 50 Hz (PAL). In
// interlaced mode (VI_CTRL bit 6) bit 0 of V_CURRENT is the field.
// VI_V_CURRENT == VI_V_INTR raises the VI interrupt; writing VI_V_CURRENT clears it.
class Vi {
public:
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> argb;
    };

    void set_region(Region region) { region_ = region; }
    void start(Scheduler& scheduler);  // schedules the first line

    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void write(std::uint32_t offset, std::uint32_t value, Mi& mi);
    void on_line(Mi& mi, Scheduler& scheduler, std::uint64_t when);
    // True once per field, when the line counter wraps (the frame is complete).
    bool take_vblank();

    // The framebuffer VI_ORIGIN points at, at its own resolution (the visible screen area
    // times the VI scale factors), as 0xAARRGGBB. Empty when the VI is off (type 0 or 1).
    [[nodiscard]] Image image(std::span<const std::uint8_t> rdram) const;

    template <class Ar>
    void serialize(Ar& ar) {
        ar(regs_), ar(v_current_), ar(vblank_), ar(field_), ar(fraction_), ar(region_);
    }

private:
    [[nodiscard]] double cycles_per_line() const;

    std::array<std::uint32_t, 14> regs_{};
    std::uint32_t v_current_ = 0;
    bool vblank_ = false;
    bool field_ = false;
    std::uint8_t pad_[6] = {};
    double fraction_ = 0;  // fractional cycles carried to the next line
    Region region_ = Region::Ntsc;
    std::uint8_t pad2_[7] = {};
};

}  // namespace ezy::n64
