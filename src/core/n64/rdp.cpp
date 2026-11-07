#include "n64/rdp.hpp"

#include <algorithm>
#include <bit>

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kXbus = 1U << 0, kFreeze = 1U << 1, kFlush = 1U << 2, kStartClock = 1U << 3,
                        kPipeBusy = 1U << 5, kCommandBusy = 1U << 6, kDmaBusy = 1U << 8, kStartValid = 1U << 10;
constexpr std::uint32_t kAddressMask = 0xFFFFF8, kCounterMask = 0xFFFFFF;
constexpr std::uint64_t kWordCycles = 3;  // approximate command fetch and execution in CPU cycles

std::uint64_t rdp_cycles(std::uint64_t cpu_cycles) {
    return (cpu_cycles / 3) * 2 + (cpu_cycles % 3) * 2 / 3;
}

unsigned opcode(std::uint64_t word) { return static_cast<unsigned>(word >> 56) & 63; }

unsigned command_words(unsigned op) {
    if (op >= 0x08 && op <= 0x0F) {  // Edges plus optional shade, texture and depth coefficients.
        return 4 + ((op & 4) ? 8U : 0U) + ((op & 2) ? 8U : 0U) + ((op & 1) ? 2U : 0U);
    }
    return (op == 0x24 || op == 0x25) ? 2 : 1;  // Texture Rectangle and Texture Rectangle Flip
}

unsigned coordinate(std::uint64_t word, unsigned shift) {
    return static_cast<unsigned>(word >> shift) & 0xFFF;
}

unsigned ceil_pixel(unsigned fixed) { return (fixed + 3) >> 2; }

std::int32_t sign_extend(std::uint32_t bits, unsigned width) {
    const std::uint32_t sign = 1U << (width - 1);
    return static_cast<std::int32_t>(((bits & ((sign << 1) - 1)) ^ sign) - sign);
}

// X is signed fixed point with 16 fractional bits. Each increment advances one of the
// four subscanlines; the low bit of both the base and the increment is discarded.
struct TriangleEdge {
    std::int32_t x, step, y;

    TriangleEdge(std::uint64_t word, std::int32_t origin)
        : x(sign_extend(static_cast<std::uint32_t>(word >> 32), 28) & ~1),
          step((sign_extend(static_cast<std::uint32_t>(word), 30) >> 2) & ~1), y(origin) {}

    [[nodiscard]] int fixed_x(std::int32_t subscanline) const {
        const auto position = x + std::int64_t{step} * (subscanline - y);
        return sign_extend(static_cast<std::uint32_t>(position), 28);
    }

    [[nodiscard]] int sample_x(std::int32_t subscanline) const {
        const int fixed = fixed_x(subscanline);
        // X samples use eighths. The sticky bit keeps an inexact edge from landing
        // on a sample position when it is truncated.
        return ((fixed >> 14) * 2) | ((fixed & 0x3FFE) != 0);
    }
};

struct TextureGradient {
    std::int32_t initial = 0, dx = 0, de = 0, dy = 0;

    [[nodiscard]] std::int64_t scanline_start(int lines, int x_fraction, bool late) const {
        const std::int64_t along_edge = initial + std::int64_t{de} * lines;
        // Depending on the major edge, attributes latch on the first or last subscanline.
        const std::int64_t edge_step = de & ~0x1FF, y_step = dy & ~0x1FF;
        const auto correction = late ? edge_step - (edge_step >> 2) - y_step + (y_step >> 2) : 0;
        return ((along_edge & ~0x1FFLL) + correction - x_fraction * ((dx >> 8) & ~1)) & ~0x3FFLL;
    }

    [[nodiscard]] int sample(std::int64_t start, int pixels) const {
        const auto fixed = start + std::int64_t{dx & ~31} * pixels;
        return static_cast<std::int32_t>(fixed) >> 16;  // The accumulator wraps to 32 bits.
    }
};

std::int32_t texture_coefficient(std::uint64_t integer, std::uint64_t fraction, unsigned shift) {
    const auto high = static_cast<std::uint32_t>((integer >> shift) & 0xFFFF);
    const auto low = static_cast<std::uint32_t>((fraction >> shift) & 0xFFFF);
    return static_cast<std::int32_t>((high << 16) | low);
}

constexpr auto kPerspectiveReciprocal = [] {
    // Rounded samples of 1/W across the normalized mantissa, including the far endpoint.
    std::array<int, 65> points{};
    for (int i = 0; i <= 64; ++i) {
        const int denominator = 64 + i;
        points[i] = ((1 << 20) + denominator / 2) / denominator;
    }
    points[6] = 0x3A83;  // The RDP table rounds this entry down.
    return points;
}();

std::array<int, 2> project_texture_coordinates(int s, int t, int w) {
    if (w <= 0) return {32767, 32767};

    // W is signed 1.15. Normalize its leading bit, then interpolate the 64-segment ROM.
    const int shift = 15 - std::bit_width(static_cast<unsigned>(w));
    const unsigned normalized = (static_cast<unsigned>(w) << shift) & 0x3FFF;
    const unsigned index = normalized >> 8;
    const int fraction = static_cast<int>(normalized & 255);
    const int base = kPerspectiveReciprocal[index];
    const int reciprocal = base + (((kPerspectiveReciprocal[index + 1] - base) * fraction) >> 8);
    const auto project = [reciprocal, shift](int coord) {
        const int product = coord * reciprocal;
        const int projected = shift == 14 ? product * 2 : product >> (13 - shift);
        return std::clamp(projected, -32768, 32767);  // Saturate before shifting tile coordinates.
    };
    return {project(s), project(t)};
}

struct SampleSpan {
    int left = 0, right = 0;
    [[nodiscard]] bool covers(int x) const { return left <= x && x < right; }
};

// Shade accumulators are 9 bits after the integer part; overflow saturates and underflow is zero.
unsigned clamp_shade(std::int64_t start, const TextureGradient& gradient, int pixels) {
    const auto value = static_cast<unsigned>(gradient.sample(start, pixels)) & 0x1FF;
    if (!(value & 0x100)) return value;
    return (value & 0x80) ? 0 : 255;
}

unsigned dither_channel(unsigned channel, unsigned threshold) {
    if ((channel & 7) <= threshold) return channel;
    return std::min(255U, (channel & ~7U) + 8);
}

}  // namespace

unsigned Rdp::buffered_draw_words(unsigned op) {
    switch (op) {
        case 0x08: return 4;
        case 0x0A: return 12;
        case 0x0C: return 12;
        case 0x24: return 2;
        default: return 0;
    }
}

std::uint32_t Rdp::read(std::uint32_t offset, std::uint64_t now) const {
    const auto elapsed = static_cast<std::uint32_t>(rdp_cycles(now) - rdp_cycles(last_update_));
    switch (offset & 0x1F) {
        case 0x00: return start_;
        case 0x04: return end_;
        case 0x08: return current_;
        case 0x0C: return status_;
        case 0x10: return static_cast<std::uint32_t>(rdp_cycles(now) - rdp_cycles(clock_origin_)) & kCounterMask;
        case 0x14: return (command_cycles_ + ((status_ & kCommandBusy) ? elapsed : 0)) & kCounterMask;
        case 0x18: return (pipe_cycles_ + ((status_ & kPipeBusy) ? elapsed : 0)) & kCounterMask;
        default: return 0;  // TMEM has no active load pipeline yet.
    }
}

void Rdp::update_counters(std::uint64_t when) {
    const auto elapsed = static_cast<std::uint32_t>(rdp_cycles(when) - rdp_cycles(last_update_));
    if (status_ & kCommandBusy) command_cycles_ = (command_cycles_ + elapsed) & kCounterMask;
    if (status_ & kPipeBusy) pipe_cycles_ = (pipe_cycles_ + elapsed) & kCounterMask;
    last_update_ = when;
}

void Rdp::write(std::uint32_t offset, std::uint32_t value, Scheduler& scheduler, std::uint64_t when) {
    switch (offset & 0x1F) {
        case 0x00:  // START latches once and becomes CURRENT on the next END write.
            if (!(status_ & kStartValid)) start_ = value & kAddressMask;
            status_ |= kStartValid;
            break;
        case 0x04:
            update_counters(when);
            end_ = value & kAddressMask;
            if (status_ & kStartValid) {
                current_ = start_;
                status_ &= ~kStartValid;
            }
            if (!(status_ & (kFreeze | kFlush))) status_ |= kStartClock | kPipeBusy;
            schedule_word(scheduler, when);
            break;
        case 0x0C: {
            update_counters(when);
            const auto apply_pair = [this, value](unsigned shift, std::uint32_t flag) {
                if (value & (1U << shift)) status_ &= ~flag;
                if (value & (2U << shift)) status_ |= flag;
            };
            apply_pair(0, kXbus);
            apply_pair(2, kFreeze);
            apply_pair(4, kFlush);
            if (value & (1U << 7)) pipe_cycles_ = 0;
            if (value & (1U << 8)) command_cycles_ = 0;
            if (value & (1U << 9)) clock_origin_ = when;
            schedule_word(scheduler, when);
            break;
        }
        default: break;
    }
}

void Rdp::schedule_word(Scheduler& scheduler, std::uint64_t when) {
    if ((status_ & (kFreeze | kFlush)) || current_ >= end_) {
        status_ &= ~(kDmaBusy | kCommandBusy);
        scheduler.cancel(EventType::RdpWord);
        return;
    }
    status_ |= kStartClock | kPipeBusy | kDmaBusy | kCommandBusy;
    if (!scheduler.pending(EventType::RdpWord)) scheduler.schedule(EventType::RdpWord, when + kWordCycles);
}

void Rdp::step(Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
               std::span<const std::uint8_t, 0x1000> dmem, std::uint64_t when) {
    if ((status_ & (kFreeze | kFlush)) || current_ >= end_) return;
    update_counters(when);
    if (words_to_skip_) {
        --words_to_skip_;
    } else {
        std::uint64_t word = 0;
        const bool xbus = (status_ & kXbus) != 0;
        const std::size_t address = xbus ? current_ & 0xFF8 : current_;
        const std::span<const std::uint8_t> source = xbus ? std::span<const std::uint8_t>(dmem)
                                                          : std::span<const std::uint8_t>(rdram);
        if (address + 8 <= source.size()) {
            for (unsigned i = 0; i < 8; ++i) word = (word << 8) | source[address + i];
        }
        const unsigned op = opcode(word);
        if (draw_words_ != 0 || buffered_draw_words(op) != 0) {
            draw_command_[draw_words_++] = word;
            const unsigned draw_op = opcode(draw_command_[0]);
            if (draw_words_ == buffered_draw_words(draw_op)) {
                if (draw_op != 0x24) draw_triangle(rdram);
                else if (cycle_type_ == 2) copy_rectangle(rdram);
                else draw_textured_rectangle(rdram);
                draw_words_ = 0;
            }
        } else {
            // Preserve packet framing for unsupported commands across END extensions.
            words_to_skip_ = command_words(op) - 1;
            if (words_to_skip_ == 0) execute(word, mi, rdram);
        }
    }
    current_ += 8;
    schedule_word(scheduler, when);
}

void Rdp::execute(std::uint64_t word, Mi& mi, std::span<std::uint8_t> rdram) {
    switch (opcode(word)) {
        case 0x29:  // Sync Full: writes are complete before the CPU sees the interrupt.
            status_ &= ~(kStartClock | kPipeBusy);
            mi.raise(MiInterrupt::Dp);
            break;
        case 0x2D: scissor_ = word; break;  // Set Scissor
        case 0x2F:  // Set Other Modes
            cycle_type_ = static_cast<std::uint32_t>(word >> 52) & 3;
            solid_.modes = word;
            break;
        case 0x30: load_palette(word, rdram); break;  // Load TLUT
        case 0x32: texture_.tiles[(word >> 24) & 7].bounds = word; break;  // Set Tile Size
        case 0x33: load_texture(word, true, rdram); break;  // Load Block
        case 0x34: load_texture(word, false, rdram); break;  // Load Tile
        case 0x35: texture_.tiles[(word >> 24) & 7].descriptor = word; break;  // Set Tile
        case 0x36: fill_rectangle(word, rdram); break;
        case 0x37: fill_color_ = static_cast<std::uint32_t>(word); break;
        case 0x38: solid_.fog = static_cast<std::uint32_t>(word); break;
        case 0x39: solid_.blend = static_cast<std::uint32_t>(word); break;
        case 0x3A: solid_.primitive = static_cast<std::uint32_t>(word); break;
        case 0x3B: solid_.environment = static_cast<std::uint32_t>(word); break;
        case 0x3C: solid_.combine = word; break;
        case 0x3D: texture_.image = word; break;  // Set Texture Image
        case 0x3F:  // Set Color Image
            solid_.format = static_cast<std::uint32_t>(word >> 53) & 7;
            pixel_size_ = static_cast<std::uint32_t>(word >> 51) & 3;
            color_width_ = (static_cast<std::uint32_t>(word >> 32) & 0x3FF) + 1;
            color_address_ = static_cast<std::uint32_t>(word) & 0x3FFFFFF;
            break;
        // Load, Pipe and Tile Sync need no action with strictly ordered commands.
        default: break;
    }
}

bool Rdp::can_fill() const {
    return cycle_type_ == 3 && pixel_size_ >= 1 && pixel_size_ <= 3 && color_width_ != 0;
}

void Rdp::fill_rectangle(std::uint64_t word, std::span<std::uint8_t> rdram) const {
    if (!can_fill()) return;
    // Fill includes the lower-right pixel, while the scissor limit is exclusive.
    const unsigned left = ceil_pixel(std::max(coordinate(word, 12), coordinate(scissor_, 44)));
    const unsigned top = ceil_pixel(std::max(coordinate(word, 0), coordinate(scissor_, 32)));
    const unsigned right = std::min((coordinate(word, 44) >> 2) + 1, ceil_pixel(coordinate(scissor_, 12)));
    const unsigned bottom = std::min((coordinate(word, 32) >> 2) + 1, ceil_pixel(coordinate(scissor_, 0)));
    if (left >= right || top >= bottom) return;
    for (unsigned y = top; y < bottom; ++y) fill_span(y, left, right, rdram);
}

void Rdp::draw_triangle(std::span<std::uint8_t> rdram) const {
    const std::uint64_t header = draw_command_[0];
    const bool textured = opcode(header) == 0x0A, shaded = opcode(header) == 0x0C;
    const auto& tile = texture_.tiles[(header >> 48) & 7];
    const auto combiner = textured ? opaque_texture_combiner(tile) : std::nullopt;
    const bool fill = !textured && can_fill();
    const auto color = textured ? std::nullopt : opaque_color();
    const bool shade_color = shaded && !fill && !color && uses_shade_color();
    if (!fill && !color && !combiner && !shade_color) return;

    const auto yh = sign_extend(static_cast<std::uint32_t>(header), 14);
    const auto ym = sign_extend(static_cast<std::uint32_t>(header >> 16), 14);
    const auto yl = sign_extend(static_cast<std::uint32_t>(header >> 32), 14);
    const int top = std::max(yh, static_cast<int>(coordinate(scissor_, 32)));
    const int bottom = std::min(yl, static_cast<int>(coordinate(scissor_, 0)));
    const int clip_left = static_cast<int>(coordinate(scissor_, 44)) * 2;
    const int clip_right = static_cast<int>(coordinate(scissor_, 12)) * 2;
    if (top >= bottom || clip_left >= clip_right) return;

    const bool major_left = (header & (1ULL << 55)) != 0;
    const TriangleEdge major(draw_command_[2], yh & ~3);
    const TriangleEdge upper(draw_command_[3], yh & ~3);
    const TriangleEdge lower(draw_command_[1], ym);
    const bool perspective = combiner && (solid_.modes & kTexturePerspective);
    TextureGradient s, t, w;
    if (combiner) {
        // STW integer and fractional halves are stored in separate words.
        const auto gradient = [&](unsigned shift) -> TextureGradient {
            return {texture_coefficient(draw_command_[4], draw_command_[6], shift),
                    texture_coefficient(draw_command_[5], draw_command_[7], shift),
                    texture_coefficient(draw_command_[8], draw_command_[10], shift),
                    texture_coefficient(draw_command_[9], draw_command_[11], shift)};
        };
        s = gradient(48);
        t = gradient(32);
        if (perspective) w = gradient(16);
    }
    std::array<TextureGradient, 3> shade{};
    if (shade_color) {
        // RGBA coefficients share the STW layout: integers and fractions in separate words.
        for (unsigned i = 0; i < shade.size(); ++i) {
            const unsigned at = 48 - 16 * i;
            shade[i] = {texture_coefficient(draw_command_[4], draw_command_[6], at),
                        texture_coefficient(draw_command_[5], draw_command_[7], at),
                        texture_coefficient(draw_command_[8], draw_command_[10], at),
                        texture_coefficient(draw_command_[9], draw_command_[11], at)};
        }
    }
    const bool late = (major.step < 0) == major_left;
    for (int y = top >> 2; 4 * y < bottom; ++y) {
        if (!field_matches(static_cast<unsigned>(y))) continue;
        int left = clip_right, right = clip_left;
        bool valid = false, all_left = true, all_right = true;
        std::array<SampleSpan, 4> samples{};
        for (unsigned row = 0; row < samples.size(); ++row) {
            const int sub = 4 * y + static_cast<int>(row);
            const int major_x = major.sample_x(sub);
            const int minor_x = (sub < ym ? upper : lower).sample_x(sub);
            const int x0 = major_left ? major_x : minor_x;
            const int x1 = major_left ? minor_x : major_x;
            all_left &= std::max(x0, x1) < clip_left;
            all_right &= std::min(x0, x1) >= clip_right;
            if (sub < top || sub >= bottom || (x0 >> 1) > (x1 >> 1)) continue;
            valid = true;
            samples[row] = {std::clamp(x0, clip_left, clip_right), std::clamp(x1, clip_left, clip_right)};
            left = std::min(left, samples[row].left);
            right = std::max(right, samples[row].right);
        }
        if (!valid || all_left || all_right) continue;
        if (fill) {
            // The fill pipeline writes the inclusive span of the four subscanlines.
            fill_span(static_cast<unsigned>(y), static_cast<unsigned>(left >> 3),
                      static_cast<unsigned>((right >> 3) + 1), rdram);
            continue;
        }
        const bool antialias = (solid_.modes & (1U << 3)) != 0;
        int texture_x = 0;
        std::int64_t start_s = 0, start_t = 0, start_w = 0;
        std::array<std::int64_t, 3> start_shade{};
        if (combiner || shade_color) {
            const int edge_x = major.fixed_x(4 * y + (late ? 3 : 0));
            const int x_fraction = (edge_x >> 8) & 255;
            const int lines = y - (yh >> 2);
            texture_x = edge_x >> 16;
            start_s = s.scanline_start(lines, x_fraction, late);
            start_t = t.scanline_start(lines, x_fraction, late);
            if (perspective) start_w = w.scanline_start(lines, x_fraction, late);
            for (unsigned i = 0; i < shade.size(); ++i) start_shade[i] = shade[i].scanline_start(lines, x_fraction, late);
        }
        for (int x = left >> 3; x <= (right >> 3); ++x) {
            if (!antialias && !samples[0].covers(x * 8)) continue;
            unsigned coverage = 0;
            for (unsigned row = 0; row < samples.size(); ++row) {
                const int sample = x * 8 + static_cast<int>(row & 1) * 2;
                coverage += samples[row].covers(sample);
                coverage += samples[row].covers(sample + 4);
            }
            if (coverage != 0) {
                std::uint32_t pixel;
                if (combiner) {
                    const int dx = x - texture_x;
                    const int sample_s = s.sample(start_s, dx), sample_t = t.sample(start_t, dx);
                    const auto [u, v] = perspective ? project_texture_coordinates(sample_s, sample_t, w.sample(start_w, dx))
                                                    : std::array{sample_s, sample_t};
                    pixel = textured_color(tile, u, v, *combiner);
                } else if (shade_color) {
                    const int dx = x - texture_x;
                    pixel = (clamp_shade(start_shade[0], shade[0], dx) << 24) |
                            (clamp_shade(start_shade[1], shade[1], dx) << 16) |
                            (clamp_shade(start_shade[2], shade[2], dx) << 8);
                } else {
                    pixel = *color;
                }
                write_opaque_pixel(static_cast<unsigned>(x), static_cast<unsigned>(y), pixel, coverage, rdram);
            }
        }
    }
}

bool Rdp::field_matches(unsigned y) const {
    const bool field_only = (scissor_ & (1ULL << 25)) != 0;
    const unsigned field = static_cast<unsigned>(scissor_ >> 24) & 1;
    return !field_only || (y & 1) == field;
}

void Rdp::fill_span(unsigned y, unsigned left, unsigned right, std::span<std::uint8_t> rdram) const {
    if (!field_matches(y)) return;
    const unsigned bytes_per_pixel = 1U << (pixel_size_ - 1);
    const auto stride = static_cast<std::size_t>(color_width_) * bytes_per_pixel;
    const std::size_t row = color_address_ + y * stride;
    const std::size_t first = row + left * bytes_per_pixel;
    const std::size_t end = std::min(row + right * bytes_per_pixel, rdram.size());
    for (std::size_t addr = first; addr < end; ++addr) {
        // The fill register is repeated as a packed big-endian word on the memory bus.
        rdram[addr] = static_cast<std::uint8_t>(fill_color_ >> ((3 - (addr & 3)) * 8));
    }
}

bool Rdp::can_write_opaque() const {
    constexpr std::uint64_t unsupported = (1ULL << 40) |  // chroma key
        (1U << 14) | (1U << 12) | (1U << 6) | (1U << 5) | (1U << 4) | 1U;
    return cycle_type_ == 0 && color_width_ != 0 && solid_.format == 0 && (pixel_size_ == 2 || pixel_size_ == 3) &&
           !(solid_.modes & unsupported) && ((solid_.modes >> 38) & 3) != 2;
}

std::optional<std::uint32_t> Rdp::opaque_color() const {
    if (!can_write_opaque()) return std::nullopt;

    // With image reads off, incoming coverage always overflows memory coverage (7).
    // With force blend off, the blender therefore passes its primary RGB input through.
    switch ((solid_.modes >> 30) & 3) {
        case 1: return std::nullopt;  // memory color
        case 2: return solid_.blend;
        case 3: return solid_.fog;
        default: break;
    }

    // One cycle uses cycle 1's combiner and cycle 0's blender. Support constant D
    // sources when the multiplier selects zero, so (A - B) * C cannot affect RGB.
    if (((solid_.combine >> 32) & 31) < 16) return std::nullopt;
    switch ((solid_.combine >> 6) & 7) {
        case 3: return solid_.primitive;
        case 5: return solid_.environment;
        case 6: return 0xFFFFFF00;
        case 7: return 0;
        default: return std::nullopt;
    }
}

bool Rdp::uses_shade_color() const {
    // Same opaque path as opaque_color, with the combiner's constant D input set to shade.
    return can_write_opaque() && ((solid_.modes >> 30) & 3) == 0 && ((solid_.combine >> 32) & 31) >= 16 &&
           ((solid_.combine >> 6) & 7) == 4;
}

void Rdp::write_opaque_pixel(unsigned x, unsigned y, std::uint32_t color, unsigned coverage,
                             std::span<std::uint8_t> rdram) const {
    const unsigned bytes = pixel_size_ == 2 ? 2 : 4;
    const std::size_t address = (color_address_ & ~(bytes - 1)) +
        (static_cast<std::size_t>(y) * color_width_ + x) * bytes;
    if (address + bytes > rdram.size()) return;

    unsigned r = color >> 24, g = (color >> 16) & 255, b = (color >> 8) & 255;
    const unsigned dither = static_cast<unsigned>(solid_.modes >> 38) & 3;
    if (dither < 2) {
        static constexpr std::array magic{0U, 6U, 1U, 7U, 4U, 2U, 5U, 3U, 3U, 5U, 2U, 4U, 7U, 1U, 6U, 0U};
        static constexpr std::array bayer{0U, 4U, 1U, 5U, 4U, 0U, 5U, 1U, 3U, 7U, 2U, 6U, 7U, 3U, 6U, 2U};
        const unsigned field_y = y >> ((scissor_ >> 25) & 1);
        const unsigned index = (field_y & 3) * 4 + (x & 3);
        const unsigned threshold = dither == 0 ? magic[index] : bayer[index];
        r = dither_channel(r, threshold);
        g = dither_channel(g, threshold);
        b = dither_channel(b, threshold);
    }

    // With memory coverage fixed at 7, clamp and wrap both store coverage - 1;
    // zap and save both store 7. The low hidden bits of 16-bit coverage aren't read here.
    const unsigned stored = (solid_.modes & (1U << 9)) ? 7 : coverage - 1;
    if (bytes == 2) {
        const unsigned packed = ((r & 0xF8) << 8) | ((g & 0xF8) << 3) | ((b & 0xF8) >> 2) | (stored >> 2);
        rdram[address] = static_cast<std::uint8_t>(packed >> 8);
        rdram[address + 1] = static_cast<std::uint8_t>(packed);
    } else {
        rdram[address] = static_cast<std::uint8_t>(r);
        rdram[address + 1] = static_cast<std::uint8_t>(g);
        rdram[address + 2] = static_cast<std::uint8_t>(b);
        rdram[address + 3] = static_cast<std::uint8_t>(stored << 5);
    }
}

}  // namespace ezy::n64
