#include "n64/vi.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <intrin.h>
#include <cstdio>
unsigned long long g_t[8];

#include "n64/vi_filter.hpp"

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

namespace {

constexpr std::uint32_t kBlack = 0xFF000000;
constexpr int kScreenWidth = 640;
constexpr std::uint32_t kNoiseSeed = 0x2545F491;  // any non-zero value: the noise restarts each output

std::uint32_t to_argb(vi::Color c) {
    return kBlack | (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b;
}

std::uint32_t next_noise(std::uint32_t& state) {  // xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// The framebuffer rows the resampler reads, filtered (antialias / dither filter) and, with
// divot, divoted, for the columns [first, first + size). Two slots: the current row and the
// next. Each framebuffer row is decoded once (pixels and coverage, with the two extra
// columns on each side the filters look at) into one of three row buffers, which the rows
// above and below share, instead of fetching every neighbour of every pixel again.
class RowCache {
public:
    RowCache(const vi::Framebuffer& fb, int first, int size, bool dither_filter, bool divot)
        : fb_(fb), first_(first), size_(size), dither_filter_(dither_filter), divot_(divot),
          margin_(divot ? 1 : 0), neighbours_(fb.use_coverage || dither_filter) {
        for (auto& slot : slots_) slot.colors.resize(static_cast<std::size_t>(size));
        if (divot) filtered_.resize(static_cast<std::size_t>(size + 2 * margin_));
        for (auto& row : decoded_) row.colors.resize(static_cast<std::size_t>(size + 2 * margin_ + 2 * kPad));
    }

    // Row `row`, keeping the slot that holds row `keep` if there is one.
    const vi::Color* get(int row, int keep) {
        for (auto& slot : slots_)
            if (slot.valid && slot.row == row) return slot.colors.data();
        Slot& slot = slots_[0].valid && slots_[0].row == keep ? slots_[1] : slots_[0];
        fill(slot, row);
        return slot.colors.data();
    }

private:
    static constexpr int kPad = 2;  // columns the filters read on each side

    struct Slot {
        std::vector<vi::Color> colors;
        int row = 0;
        bool valid = false;
    };

    // Framebuffer row `row`, columns [first - margin - kPad, ...), decoded into a buffer that
    // does not hold one of the rows centre - 1..centre + 1 if it isn't there yet.
    const vi::Color* decoded(int row, int centre) {
        for (auto& d : decoded_)
            if (d.valid && d.row == row) return d.colors.data();
        Slot* victim = &decoded_[0];
        for (auto& d : decoded_) {
            if (!d.valid || d.row < centre - 1 || d.row > centre + 1) {
                victim = &d;
                break;
            }
        }
        decode(*victim, row);
        return victim->colors.data();
    }

    // As vi::fetch for each column: black with coverage 7 outside RDRAM.
    void decode(Slot& d, int row) {
        d.row = row;
        d.valid = true;
        const std::int64_t bytes = fb_.rgba32 ? 4 : 2;
        const std::int64_t index = static_cast<std::int64_t>(row) * fb_.width + (first_ - margin_ - kPad);
        const std::int64_t start = static_cast<std::int64_t>(fb_.origin) + index * bytes;
        const auto count = static_cast<std::int64_t>(d.colors.size());
        const auto limit = static_cast<std::int64_t>(fb_.rdram.size());
        vi::Color* out = d.colors.data();
        const bool inside = start >= 0 && start + count * bytes <= limit;
        for (std::int64_t i = 0; i < count; ++i) {
            const std::int64_t address = start + i * bytes;
            if (!inside && (address < 0 || address + bytes > limit)) {
                out[i] = vi::Color{};
                continue;
            }
            const std::uint8_t* p = fb_.rdram.data() + address;
            vi::Color& c = out[i];
            if (fb_.rgba32) {
                c.r = p[0], c.g = p[1], c.b = p[2];
                c.coverage = fb_.use_coverage ? static_cast<std::uint8_t>((p[3] >> 5) & 7) : 7;
            } else {
                const unsigned pixel = (static_cast<unsigned>(p[0]) << 8) | p[1];
                c.r = static_cast<std::uint8_t>(((pixel >> 11) & 31) << 3);
                c.g = static_cast<std::uint8_t>(((pixel >> 6) & 31) << 3);
                c.b = static_cast<std::uint8_t>(((pixel >> 1) & 31) << 3);
                c.coverage = 7;
                if (fb_.use_coverage) {
                    const unsigned hidden = fb_.hidden ? fb_.hidden->read(static_cast<std::size_t>(address >> 1)) : 0;
                    c.coverage = static_cast<std::uint8_t>(((pixel & 1) << 2) | (hidden & 3));
                }
            }
        }
    }

    void fill(Slot& slot, int row) {
        slot.row = row;
        slot.valid = true;
        const vi::Color* mid = decoded(row, row) + kPad;
        const vi::Color* above = neighbours_ ? decoded(row - 1, row) + kPad : nullptr;
        const vi::Color* below = neighbours_ ? decoded(row + 1, row) + kPad : nullptr;
        // Without antialias or dither filter a pixel is its own filtered value.
        const auto filtered = [&](int i) {
            if (!neighbours_) return mid[i];
            if (mid[i].coverage < 7) return vi::detail::antialias_pixel(above + i, mid + i, below + i);
            return dither_filter_ ? vi::detail::dither_pixel(above + i, mid + i, below + i) : mid[i];
        };
        if (!divot_) {
            for (int i = 0; i < size_; ++i) slot.colors[static_cast<std::size_t>(i)] = filtered(i);
            return;
        }
        // Divot looks at the filtered neighbours on the left and right: one more column each side.
        for (int i = 0; i < size_ + 2; ++i) filtered_[static_cast<std::size_t>(i)] = filtered(i);
        for (std::size_t i = 0; i < static_cast<std::size_t>(size_); ++i)
            slot.colors[i] = vi::divot(filtered_[i + 1], filtered_[i], filtered_[i + 2]);
    }

    const vi::Framebuffer& fb_;
    int first_, size_;
    bool dither_filter_, divot_;
    int margin_;       // 1 with divot: the filtered columns start one before first_
    bool neighbours_;  // antialias or dither filter may read the rows above and below
    std::array<Slot, 2> slots_;
    std::array<Slot, 3> decoded_;
    std::vector<vi::Color> filtered_;
};

}  // namespace

Vi::Image Vi::output(std::span<const std::uint8_t> rdram, const RdpHiddenBits* hidden) const {
    const auto t0 = __rdtsc();
    const bool pal = region_ == Region::Pal;
    Image img;
    img.width = kScreenWidth;
    img.height = pal ? 576 : 480;
    const std::size_t size = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    const int field_lines = img.height / 2;

    const std::uint32_t ctrl = regs_[kCtrl];
    const std::uint32_t type = ctrl & 3;
    const bool interlaced = (ctrl & (1U << 6)) != 0;
    const int h_start = static_cast<int>((regs_[kHVideo] >> 16) & 0x3FF), h_end = static_cast<int>(regs_[kHVideo] & 0x3FF);
    const int v_start = static_cast<int>((regs_[kVVideo] >> 16) & 0x3FF), v_end = static_cast<int>(regs_[kVVideo] & 0x3FF);
    int hres = h_end - h_start;
    int vres = v_end > v_start ? (v_end - v_start) >> 1 : 0;
    int column = h_start - (pal ? 128 : 108);
    int line = (v_start - (pal ? 44 : 34)) >> 1;  // floor, also for a window above the first line
    const std::uint32_t x_add = regs_[kXScale] & 0xFFF, y_add = regs_[kYScale] & 0xFFF;
    std::uint32_t x_start = (regs_[kXScale] >> 16) & 0xFFF, y_start = (regs_[kYScale] >> 16) & 0xFFF;
    if (column < 0) {
        x_start += x_add * static_cast<std::uint32_t>(-column);
        hres += column;
        column = 0;
    }
    if (line < 0) {
        y_start += y_add * static_cast<std::uint32_t>(-line);
        vres += line;
        line = 0;
    }
    hres = std::min(hres, kScreenWidth - column);
    vres = std::min(vres, field_lines - line);

    if (type < 2 || hres <= 0 || vres <= 0) {
        weave_.clear();
        weave_key_ = 0;
        img.argb.assign(size, kBlack);
        return img;
    }

    // Interlaced: start from the previous field's output if it had the same size and pixel type.
    const int field = static_cast<int>(v_current_ & 1);
    const std::uint32_t key = 1U | (type << 1) | (static_cast<std::uint32_t>(img.height) << 3);
    if (interlaced && weave_key_ == key && weave_.size() == size) {
        img.argb = std::move(weave_);
        for (int y = 0; y < field_lines; ++y) {
            auto* row = img.argb.data() + static_cast<std::size_t>(2 * y + field) * kScreenWidth;
            std::fill(row, row + kScreenWidth, kBlack);
        }
    } else {
        img.argb.assign(size, kBlack);
    }
    const auto t1 = __rdtsc(); g_t[0] += t1 - t0;

    const std::uint32_t aa_mode = (ctrl >> 8) & 3;
    vi::Framebuffer fb;
    fb.rdram = rdram;
    fb.hidden = hidden;
    fb.origin = regs_[kOrigin] & 0xFFFFFF;
    fb.width = regs_[kWidth] & 0xFFF;
    fb.rgba32 = type == 3;
    fb.use_coverage = aa_mode <= 1;
    const bool resample = aa_mode != 3;
    const bool gamma = (ctrl & (1U << 3)) != 0, gamma_dither = (ctrl & (1U << 2)) != 0;

    // Columns read: from x_start's to the last pixel's, plus the one it interpolates with.
    const auto first_col = static_cast<int>(x_start >> 10);
    const auto last_col = static_cast<int>((x_start + static_cast<std::uint32_t>(hres - 1) * x_add) >> 10) + 1;
    RowCache rows(fb, first_col, last_col - first_col + 1, (ctrl & (1U << 16)) != 0, (ctrl & (1U << 4)) != 0);
    std::uint32_t noise = kNoiseSeed;

    for (int y = 0; y < vres; ++y) {
        const std::uint32_t curry = y_start + static_cast<std::uint32_t>(y) * y_add;
        const auto fb_row = static_cast<int>(curry >> 10);
        const unsigned yfrac = (curry >> 5) & 31;
        const auto ta = __rdtsc();
        const vi::Color* top = rows.get(fb_row, fb_row + 1);
        const vi::Color* bottom = resample && yfrac != 0 ? rows.get(fb_row + 1, fb_row) : top;
        g_t[1] += __rdtsc() - ta;
        const int screen_line = line + y;
        auto* out = img.argb.data() + static_cast<std::size_t>(2 * screen_line + (interlaced ? field : 0)) * kScreenWidth + column;
        for (int x = 0; x < hres; ++x) {
            const std::uint32_t xoffs = x_start + static_cast<std::uint32_t>(x) * x_add;
            const auto i = static_cast<std::size_t>(static_cast<int>(xoffs >> 10) - first_col);
            const unsigned xfrac = (xoffs >> 5) & 31;
            vi::Color c = top[i];
            if (resample && (xfrac != 0 || yfrac != 0)) {
                const vi::Color left = vi::lerp(top[i], bottom[i], yfrac);
                const vi::Color right = vi::lerp(top[i + 1], bottom[i + 1], yfrac);
                c = vi::lerp(left, right, xfrac);
            }
            out[x] = to_argb(gamma || gamma_dither ? vi::gamma(c, gamma, gamma_dither, next_noise(noise)) : c);
        }
        if (!interlaced) std::copy(out, out + hres, out + kScreenWidth);
    }

    const auto t2 = __rdtsc(); g_t[2] += t2 - t1; if (++g_t[7] % 100 == 0) std::printf("assign %llu fill %llu loop+fill %llu
", g_t[0]/g_t[7], g_t[1]/g_t[7], g_t[2]/g_t[7]);
    if (interlaced) {
        weave_ = img.argb;
        weave_key_ = key;
    } else {
        weave_.clear();
        weave_key_ = 0;
    }
    return img;
}

}  // namespace ezy::n64
