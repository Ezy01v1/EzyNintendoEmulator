#include "gc/vi.hpp"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Vi);
constexpr std::uint64_t kCyclesPerLine = 2 * Vi::kCyclesPerHalfLine;
constexpr std::uint64_t kCyclesPerSample = 36;  // 486 MHz / 13.5 MHz

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled video interface register at offset 0x" + b, kBase + offset);
}

bool is_di_high(std::uint32_t offset) { return offset >= 0x30 && offset <= 0x3C && (offset & 3) == 0; }
}  // namespace

std::uint32_t yuv_to_argb(int y, int u, int v) {
    const int c = y - 16, d = u - 128, e = v - 128;
    auto clamp = [](int x) { return static_cast<std::uint32_t>(std::clamp(x, 0, 255)); };
    const std::uint32_t r = clamp((298 * c + 409 * e + 128) >> 8);
    const std::uint32_t g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
    const std::uint32_t b = clamp((298 * c + 516 * d + 128) >> 8);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

Yuv rgb_to_yuv(int r, int g, int b) {
    auto clamp = [](int x) { return static_cast<std::uint8_t>(std::clamp(x, 0, 255)); };
    return {clamp(16 + ((66 * r + 129 * g + 25 * b + 128) >> 8)), clamp(128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8)),
            clamp(128 + ((112 * r - 94 * g - 18 * b + 128) >> 8))};
}

Vi::Vi(Scheduler& scheduler, Pi& pi) : sched_(scheduler), pi_(pi) {
    // Dolphin's VideoInterface::Preset() (NTSC).
    regs_[0x00 / 2] = 6;                // VTR: EQU = 6, ACV = 0
    regs_[0x02 / 2] = 1;                // DCR: ENB
    regs_[0x06 / 2] = 429;              // HTR0: HLW = 429
    regs_[0x30 / 2] = 0x1000 | 263;     // DI0: enabled, VCT = 263
    regs_[0x32 / 2] = 430;              //      HCT = 430 (> HLW: the second half-line)
    regs_[0x34 / 2] = 0x1000 | 1;       // DI1: enabled, VCT = 1
    regs_[0x36 / 2] = 1;                //      HCT = 1
    regs_[0x48 / 2] = (40 << 8) | 40;   // picture configuration: WPL = STD = 40
    sched_.schedule_in(EventType::ViLine, kCyclesPerHalfLine);
    frame_start_ = sched_.now();
    check_half_line();  // line 1 of the first field starts now
}

bool Vi::on_event(std::uint64_t when) {
    sched_.schedule(EventType::ViLine, when + kCyclesPerHalfLine);  // from `when`: lateness never accumulates
    half_line_ = (half_line_ + 1) % kHalfLinesPerFrame;
    if (half_line_ == 0) frame_start_ = when;
    const bool new_field = half_line_ % kHalfLinesPerField == 0;
    if (new_field) ++fields_;
    check_half_line();
    return new_field;
}

void Vi::check_half_line() {
    std::uint32_t hlw = regs_[0x06 / 2] & 0x3FFu;  // HTR0's HLW
    if (hlw == 0) hlw = 429;                        // never set up: the reset value
    for (std::uint32_t offset = 0x30; offset <= 0x3C; offset += 4) {
        std::uint16_t& hi = regs_[offset / 2];
        const std::uint32_t vct = hi & 0x7FFu, hct = regs_[offset / 2 + 1] & 0x7FFu;
        const std::uint32_t second_half = hct > hlw ? 1 : 0;
        if (1 + half_line_ / 2 == vct && (half_line_ & 1) == second_half) hi |= 0x8000;
    }
    update_irq();
}

void Vi::update_irq() {
    bool any = false;
    for (std::uint32_t offset = 0x30; offset <= 0x3C; offset += 4) {
        const std::uint16_t hi = regs_[offset / 2];
        any = any || ((hi & 0x8000) && (hi & 0x1000));  // INT and ENB
    }
    pi_.set_cause(Pi::kVi, any);
}

std::uint16_t Vi::read16(std::uint32_t offset) {
    if (offset & 1 || offset >= kRegisterBytes) unmodeled("read", offset);
    if (offset == 0x2C || offset == 0x2E) {  // DPV / DPH: where the beam is now in the frame
        const std::uint64_t t = std::min<std::uint64_t>(sched_.now() - frame_start_, 2 * kCyclesPerField - 1);
        return static_cast<std::uint16_t>(offset == 0x2C ? t / kCyclesPerLine + 1 : (t % kCyclesPerLine) / kCyclesPerSample + 1);
    }
    return regs_[offset / 2];
}

std::uint32_t Vi::read32(std::uint32_t offset) {
    return (std::uint32_t{read16(offset)} << 16) | read16(offset + 2);
}

void Vi::write16(std::uint32_t offset, std::uint16_t value) {
    if (offset & 1 || offset >= kRegisterBytes || offset == 0x2C || offset == 0x2E) unmodeled("write", offset);
    std::uint16_t& reg = regs_[offset / 2];
    if (is_di_high(offset)) {
        reg = value & 0x97FF;  // INT, ENB and VCT, stored as written (INT included: Dolphin)
        update_irq();
    } else if (offset >= 0x32 && offset <= 0x3E && (offset & 3) == 2) {
        reg = value & 0x7FF;  // HCT
    } else if (offset == 0x02) {
        reg = value & 0x3FD;  // DCR without RST
        if (value & 0x2) {    // RST: Dolphin clears the four display interrupt registers
            for (std::uint32_t i = 0x30 / 2; i < 0x40 / 2; ++i) regs_[i] = 0;
            update_irq();
        }
    } else if ((offset == 0x1C || offset == 0x20 || offset == 0x24 || offset == 0x28)) {
        reg = value;
        if (value & 0xE000) reg &= ~0x1000;  // CLRPOFF clears this register's POFF
    } else {
        reg = value;  // every other register: stored, no effect (see the header)
    }
}

void Vi::write32(std::uint32_t offset, std::uint32_t value) {
    write16(offset, static_cast<std::uint16_t>(value >> 16));
    write16(offset + 2, static_cast<std::uint16_t>(value));
}

std::uint32_t Vi::xfb_top() const {
    const std::uint32_t r = reg32(0x1C);
    return (r & 0x10000000) ? (r & 0xFFFFFF) << 5 : (r & 0xFFFFFF);
}

std::uint32_t Vi::xfb_bottom() const {
    const std::uint32_t r = reg32(0x24);
    // The bottom register has no POFF of its own: the top register's applies.
    return (reg32(0x1C) & 0x10000000) ? (r & 0xFFFFFF) << 5 : (r & 0xFFFFFF);
}

Image Vi::xfb_image(std::span<const std::uint8_t> ram) const {
    const int acv = active_lines() == 0 ? 480 : active_lines();
    const bool interlaced_mode = interlaced();
    Image image;
    // The picture configuration's WPL (bits 8-14): words of 32 bytes, 16 pixels, per line.
    const int wpl = (regs_[0x48 / 2] >> 8) & 0x7F;
    image.width = wpl == 0 ? kXfbWidth : std::min(wpl * 16, kXfbWidth);
    image.height = interlaced_mode ? acv * 2 : (acv <= 240 ? acv * 2 : acv);
    image.argb.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height), 0xFF000000u);
    if (active_lines() == 0) return image;  // the VI was never set up: black

    // The line stride in bytes: the picture configuration's STD in 32-byte units (the SDK sets
    // WPL = 40 for 640 pixels, so 1280 bytes; STD = 2 * WPL = 2560 for an interleaved buffer).
    const std::size_t std_units = regs_[0x48 / 2] & 0xFFu;
    const std::size_t stride = std_units == 0 ? 2 * static_cast<std::size_t>(image.width) : std_units * 32;
    const std::size_t row_bytes = 2 * static_cast<std::size_t>(image.width);
    const std::uint32_t top = xfb_top(), bottom = xfb_bottom();
    for (int row = 0; row < image.height; ++row) {
        std::uint64_t base;
        int line;
        if (interlaced_mode) {
            base = (row & 1) ? bottom : top;
            line = row / 2;
        } else {
            base = top;
            line = acv <= 240 ? row / 2 : row;
        }
        const std::uint64_t at = base + static_cast<std::uint64_t>(line) * stride;
        if (at + row_bytes > ram.size()) continue;  // outside MEM1: this row stays black
        const std::uint8_t* p = ram.data() + at;
        std::uint32_t* out = image.argb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(image.width);
        for (int x = 0; x < image.width; x += 2, p += 4) {  // Y0 U Y1 V
            out[x] = yuv_to_argb(p[0], p[1], p[3]);
            out[x + 1] = yuv_to_argb(p[2], p[1], p[3]);
        }
    }
    return image;
}

}  // namespace ezy::gc
