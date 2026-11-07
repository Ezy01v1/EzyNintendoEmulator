#include "n64/vi.hpp"

#include <cmath>

namespace ezy::n64 {

namespace {

constexpr double kCpuHz = 93'750'000.0;
enum Reg : std::uint32_t { kCtrl, kOrigin, kWidth, kVIntr, kVCurrent, kBurst, kVSync, kHSync, kHSyncLeap, kHVideo,
                           kVVideo, kVBurst, kXScale, kYScale };

constexpr std::uint32_t expand5(std::uint32_t v) { return (v << 3) | (v >> 2); }

}  // namespace

void Vi::start(Scheduler& scheduler) {
    regs_[kVIntr] = 0x3FF;
    fraction_ = 0;
    const double cycles = cycles_per_line();
    const auto step = static_cast<std::uint64_t>(cycles);
    fraction_ = cycles - static_cast<double>(step);
    scheduler.schedule_in(EventType::ViLine, step);
}

double Vi::cycles_per_line() const {
    const std::uint32_t v_sync = regs_[kVSync] & 0x3FF;
    const double half_lines = v_sync != 0 ? v_sync + 1.0 : (region_ == Region::Pal ? 625.0 : 525.0);
    const double rate = region_ == Region::Pal ? 50.0 : 60.0 / 1.001;
    return kCpuHz / (rate * half_lines / 2.0);
}

std::uint32_t Vi::read(std::uint32_t offset) const {
    const std::uint32_t reg = (offset & 0x3F) / 4;
    if (reg == kVCurrent) return v_current_;
    return reg < regs_.size() ? regs_[reg] : 0;
}

void Vi::write(std::uint32_t offset, std::uint32_t value) {
    const std::uint32_t reg = (offset & 0x3F) / 4;
    if (reg >= regs_.size() || reg == kVCurrent) return;
    regs_[reg] = value;
}

void Vi::write(std::uint32_t offset, std::uint32_t value, Mi& mi) {
    if ((offset & 0x3F) / 4 == kVCurrent) {
        mi.clear(MiInterrupt::Vi);
        return;
    }
    write(offset, value);
}

void Vi::on_line(Mi& mi, Scheduler& scheduler, std::uint64_t when) {
    const std::uint32_t v_sync = (regs_[kVSync] & 0x3FF) != 0 ? regs_[kVSync] & 0x3FF : (region_ == Region::Pal ? 625 : 525);
    const bool interlaced = (regs_[kCtrl] & (1U << 6)) != 0;
    std::uint32_t line = (v_current_ & ~1U) + 2;
    if (line >= v_sync) {
        line = 0;
        field_ = interlaced ? !field_ : false;
        vblank_ = true;
    }
    v_current_ = line | (field_ ? 1U : 0U);
    if ((v_current_ & 0x3FE) == (regs_[kVIntr] & 0x3FE)) mi.raise(MiInterrupt::Vi);

    const double cycles = cycles_per_line() + fraction_;
    const auto step = static_cast<std::uint64_t>(cycles);
    fraction_ = cycles - static_cast<double>(step);
    scheduler.schedule(EventType::ViLine, when + step);
}

bool Vi::take_vblank() {
    const bool v = vblank_;
    vblank_ = false;
    return v;
}

Vi::Image Vi::image(std::span<const std::uint8_t> rdram) const {
    Image img;
    const std::uint32_t type = regs_[kCtrl] & 3;
    if (type < 2) return img;
    const std::uint32_t h_start = (regs_[kHVideo] >> 16) & 0x3FF, h_end = regs_[kHVideo] & 0x3FF;
    const std::uint32_t v_start = (regs_[kVVideo] >> 16) & 0x3FF, v_end = regs_[kVVideo] & 0x3FF;
    if (h_end <= h_start || v_end <= v_start) return img;
    const std::uint32_t x_scale = regs_[kXScale] & 0xFFF, y_scale = regs_[kYScale] & 0xFFF;
    // Width: the active screen pixels (H_VIDEO) times X_SCALE (2.10 fixed point). Height:
    // the nominal 240 (NTSC) or 288 (PAL) lines of a field times Y_SCALE. V_VIDEO usually
    // shows a few lines less (474 of a 480-line framebuffer in PeterLemon's 640x480 mode),
    // but the framebuffer, which is what we show and compare, is the full height.
    const std::uint32_t nominal_lines = region_ == Region::Pal ? 288 : 240;
    img.width = static_cast<int>(((h_end - h_start) * x_scale) >> 10);
    img.height = static_cast<int>((nominal_lines * y_scale) >> 10);
    if (img.width <= 0 || img.height <= 0) return Image{};
    img.argb.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height), 0xFF000000);

    const std::uint32_t origin = regs_[kOrigin] & 0xFFFFFF;
    const std::uint32_t stride = regs_[kWidth] & 0xFFF;
    const std::uint32_t bpp = type == 2 ? 2 : 4;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const std::size_t addr = origin + (static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)) * bpp;
            if (addr + bpp > rdram.size()) continue;
            std::uint32_t argb;
            if (bpp == 2) {
                const std::uint32_t p = (rdram[addr] << 8) | rdram[addr + 1];
                argb = 0xFF000000 | (expand5((p >> 11) & 31) << 16) | (expand5((p >> 6) & 31) << 8) | expand5((p >> 1) & 31);
            } else {
                argb = 0xFF000000 | (static_cast<std::uint32_t>(rdram[addr]) << 16) | (rdram[addr + 1] << 8) | rdram[addr + 2];
            }
            img.argb[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) + static_cast<std::size_t>(x)] = argb;
        }
    }
    return img;
}

}  // namespace ezy::n64
