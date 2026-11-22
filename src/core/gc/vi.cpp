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

Vi::Vi(Scheduler& scheduler, Pi& pi) : sched_(scheduler), pi_(pi) {
    sched_.schedule_in(EventType::ViLine, kCyclesPerHalfLine);
    field_start_ = sched_.now();
}

bool Vi::on_event(std::uint64_t when) {
    sched_.schedule(EventType::ViLine, when + kCyclesPerHalfLine);  // from `when`: lateness never accumulates
    half_line_ = (half_line_ + 1) % kHalfLinesPerField;
    const bool new_field = half_line_ == 0;
    if (new_field) {
        ++fields_;
        field_start_ = when;
    }
    if ((half_line_ & 1) == 0) check_lines();
    return new_field;
}

void Vi::check_lines() {
    const std::uint32_t line = half_line_ / 2 + 1;
    for (std::uint32_t offset = 0x30; offset <= 0x3C; offset += 4) {
        std::uint16_t& hi = regs_[offset / 2];
        if ((hi & 0x1000) && (hi & 0x7FFu) == line) hi |= 0x8000;
    }
    update_irq();
}

void Vi::update_irq() {
    bool any = false;
    for (std::uint32_t offset = 0x30; offset <= 0x3C; offset += 4) {
        const std::uint16_t hi = regs_[offset / 2];
        any = any || ((hi & 0x8000) && (hi & 0x1000));
    }
    pi_.set_cause(Pi::kVi, any);
}

std::uint16_t Vi::read16(std::uint32_t offset) {
    if (offset & 1 || offset >= kRegisterBytes) unmodeled("read", offset);
    if (offset == 0x2C || offset == 0x2E) {  // DPV / DPH: where the beam is now
        const std::uint64_t t = std::min<std::uint64_t>(sched_.now() - field_start_, kCyclesPerField - 1);
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
        // INT (bit 15 of the high half) only clears; the other bits are VCT and ENB.
        reg = static_cast<std::uint16_t>((value & 0x1FFF) | ((value & 0x8000) ? (reg & 0x8000) : 0));
        update_irq();
        return;
    }
    reg = value;  // every other register: stored, no effect (see the header)
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
    return (r & 0x10000000) ? (r & 0xFFFFFF) << 5 : (r & 0xFFFFFF);
}

Image Vi::xfb_image(std::span<const std::uint8_t> ram) const {
    const int acv = active_lines() == 0 ? 480 : active_lines();
    const bool interlaced_mode = interlaced();
    Image image;
    image.width = kXfbWidth;
    image.height = interlaced_mode ? acv * 2 : (acv <= 240 ? acv * 2 : acv);
    image.argb.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height), 0xFF000000u);
    if (active_lines() == 0) return image;  // the VI was never set up: black

    constexpr std::size_t kStride = 2 * kXfbWidth;
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
        const std::uint64_t at = base + static_cast<std::uint64_t>(line) * kStride;
        if (at + kStride > ram.size()) continue;  // outside MEM1: this row stays black
        const std::uint8_t* p = ram.data() + at;
        std::uint32_t* out = image.argb.data() + static_cast<std::size_t>(row) * kXfbWidth;
        for (int x = 0; x < kXfbWidth; x += 2, p += 4) {  // Y0 U Y1 V
            out[x] = yuv_to_argb(p[0], p[1], p[3]);
            out[x + 1] = yuv_to_argb(p[2], p[1], p[3]);
        }
    }
    return image;
}

}  // namespace ezy::gc
