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

// Z is 15.17 fixed point; the depth unit works on 18 bits and saturates outside them.
std::uint32_t clamp_depth(std::int64_t z) {
    const auto value = static_cast<std::uint32_t>(z >> 10) & 0x3FFFFF;
    if (value & 0x200000) return 0;
    return std::min(value, 0x3FFFFU);
}

// 18-bit Z is stored as a 3-bit count of leading ones and an 11-bit mantissa.
unsigned compress_depth(std::uint32_t z) {
    unsigned exponent = 0;
    while (exponent < 7 && (z & (0x20000U >> exponent))) ++exponent;
    const unsigned shift = exponent < 7 ? 6 - exponent : 0;
    return (exponent << 11) | ((z >> shift) & 0x7FF);
}

std::uint32_t decompress_depth(unsigned stored) {
    const unsigned exponent = (stored >> 11) & 7, mantissa = stored & 0x7FF;
    const std::uint32_t ones = (0x3FFFFU << (18 - exponent)) & 0x3FFFF;
    const unsigned shift = exponent < 7 ? 6 - exponent : 0;
    return ones | (mantissa << shift);
}

// DZ is kept as a power of two: the 4-bit code is the position of the highest set bit.
unsigned compress_dz(unsigned dz) {
    unsigned code = 0;
    while (code < 15 && (dz >> (code + 1)) != 0) ++code;
    return code;
}

// Rounds DZ down to its highest power of two, as the hardware comparator does.
unsigned highest_power_of_two(unsigned value) {
    return value == 0 ? 0 : 1U << compress_dz(value);
}

unsigned dither_channel(unsigned channel, unsigned threshold) {
    if ((channel & 7) <= threshold) return channel;
    return std::min(255U, (channel & ~7U) + 8);
}

}  // namespace

unsigned Rdp::words_in_command(unsigned op) { return command_words(op & 63); }

unsigned Rdp::buffered_draw_words(unsigned op) {
    switch (op) {
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            return command_words(op);
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
    std::uint64_t word = 0;
    const bool xbus = (status_ & kXbus) != 0;
    const std::size_t address = xbus ? current_ & 0xFF8 : current_;
    const std::span<const std::uint8_t> source = xbus ? std::span<const std::uint8_t>(dmem)
                                                      : std::span<const std::uint8_t>(rdram);
    if (address + 8 <= source.size()) {
        for (unsigned i = 0; i < 8; ++i) word = (word << 8) | source[address + i];
    }
    consume_word(word, mi, rdram);
    current_ += 8;
    schedule_word(scheduler, when);
}

void Rdp::submit_words(std::span<const std::uint64_t> words, Mi& mi, std::span<std::uint8_t> rdram,
                       bool defer_sync_full) {
    if (words.empty()) return;
    status_ |= kStartClock | kPipeBusy;  // as if the words had come through DPC
    deferring_sync_full_ = defer_sync_full;
    for (const std::uint64_t word : words) consume_word(word, mi, rdram);
    deferring_sync_full_ = false;
}

void Rdp::finish_deferred_sync(Mi& mi) {
    if (!pending_sync_full_) return;
    pending_sync_full_ = false;
    // No wait for the drawing thread here, so the emulation runs on while it draws; only a
    // backlog of several frames' work makes it wait, so the thread never falls far behind.
    constexpr std::size_t kMaxBacklog = 20000;
    if (worker_ && worker_->backlog() > kMaxBacklog) sync();
    status_ &= ~(kStartClock | kPipeBusy);
    mi.raise(MiInterrupt::Dp);
}

// One command-stream word, from DPC or from the graphics HLE: packet framing, buffered draw
// commands, and Sync Full.
void Rdp::consume_word(std::uint64_t word, Mi& mi, std::span<std::uint8_t> rdram) {
    if (trace_) trace_->push_back(word);
    if (words_to_skip_) {
        --words_to_skip_;
        return;
    }
    const unsigned op = opcode(word);
    if (draw_words_ != 0 || buffered_draw_words(op) != 0) {
        draw_command_[draw_words_++] = word;
        if (draw_words_ == buffered_draw_words(opcode(draw_command_[0]))) {
            submit({true, 0, draw_command_, rdram});
            draw_words_ = 0;
        }
        return;
    }
    // Preserve packet framing for unsupported commands across END extensions.
    words_to_skip_ = command_words(op) - 1;
    if (words_to_skip_ != 0) return;
    if (op == 0x29) {  // Sync Full: writes are complete before the CPU sees the interrupt.
        if (deferring_sync_full_) {
            pending_sync_full_ = true;
            return;
        }
        sync();
        status_ &= ~(kStartClock | kPipeBusy);
        mi.raise(MiInterrupt::Dp);
    } else {
        submit({false, word, {}, rdram});
    }
}

// Drawing and the commands that set its state run here: on the worker thread when there is
// one, otherwise right away. Nothing here touches the command interface (status, counters).
void Rdp::render(const RenderJob& job) {
    if (!job.draw) {
        execute(job.word, job.rdram);
        return;
    }
    render_command_ = job.command;
    const unsigned op = opcode(render_command_[0]);
    if (op != 0x24) draw_triangle(job.rdram);
    else if (cycle_type_ == 2) copy_rectangle(job.rdram);
    else draw_textured_rectangle(job.rdram);
}

void Rdp::submit(const RenderJob& job) {
    if (worker_) worker_->push(job);
    else render(job);
}

void Rdp::sync() {
    if (worker_) worker_->drain();
}

void Rdp::set_threaded(bool threaded) {
    if (threaded == (worker_ != nullptr)) return;
    if (threaded) worker_ = std::make_unique<RdpWorker<RenderJob>>([this](const RenderJob& job) { render(job); });
    else worker_.reset();  // drains first
}

void Rdp::execute(std::uint64_t word, std::span<std::uint8_t> rdram) {
    switch (opcode(word)) {
        case 0x2D: scissor_ = word; break;  // Set Scissor
        case 0x2E: prim_depth_ = static_cast<std::uint32_t>(word); break;  // Set Prim Depth
        case 0x2A: key_gb_ = word & 0xFFFFFFFFFFFFFFULL; break;  // Set Key GB
        case 0x2B: key_r_ = static_cast<std::uint32_t>(word) & 0xFFFFFFF; break;  // Set Key R
        case 0x3E: mask_address_ = static_cast<std::uint32_t>(word) & 0x3FFFFFF; break;  // Set Mask Image
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
        case 0x3A:  // Set Primitive Color
            solid_.primitive = static_cast<std::uint32_t>(word);
            prim_lod_ = static_cast<std::uint32_t>(word >> 32) & 0x1FFF;
            break;
        case 0x2C: convert_ = word & 0x3FFFFFFFFFFFFFULL; break;  // Set Convert
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
    if (cycle_type_ <= 1) {
        draw_flat_rectangle(word, rdram);
        return;
    }
    if (!can_fill()) return;
    // Fill includes the lower-right pixel, while the scissor limit is exclusive.
    const unsigned left = ceil_pixel(std::max(coordinate(word, 12), coordinate(scissor_, 44)));
    const unsigned top = ceil_pixel(std::max(coordinate(word, 0), coordinate(scissor_, 32)));
    const unsigned right = std::min((coordinate(word, 44) >> 2) + 1, ceil_pixel(coordinate(scissor_, 12)));
    const unsigned bottom = std::min((coordinate(word, 32) >> 2) + 1, ceil_pixel(coordinate(scissor_, 0)));
    if (left >= right || top >= bottom) return;
    for (unsigned y = top; y < bottom; ++y) fill_span(y, left, right, rdram);
}

void Rdp::draw_flat_rectangle(std::uint64_t word, std::span<std::uint8_t> rdram) const {
    // In one- and two-cycle modes, Fill Rectangle goes through the combiner and blender like a
    // rectangle with no texture or shade, excluding its lower-right edge. Depth, if enabled,
    // can only come from the primitive depth.
    const bool prim_depth = (solid_.modes & (1U << 2)) != 0;
    if (!can_write_opaque(prim_depth)) return;
    const unsigned left = std::max(coordinate(word, 12), coordinate(scissor_, 44));
    const unsigned top = std::max(coordinate(word, 0), coordinate(scissor_, 32));
    const unsigned right = std::min(coordinate(word, 44), coordinate(scissor_, 12));
    const unsigned bottom = std::min(coordinate(word, 32), coordinate(scissor_, 0));
    if (left >= right || top >= bottom) return;
    const bool antialias = (solid_.modes & (1U << 3)) != 0;
    const bool use_depth = (solid_.modes & ((1U << 4) | (1U << 5))) != 0;
    const std::uint32_t z = ((prim_depth_ >> 16) & 0x7FFF) << 3;
    const unsigned dz = highest_power_of_two(prim_depth_ & 0xFFFF);
    const auto covers_x = [left, right](unsigned sample) { return left <= sample && sample < right; };
    const Rgba color = combine(Rgba{}, Rgba{}, Rgba{});
    for (unsigned y = top >> 2; 4 * y < bottom; ++y) {
        if (!field_matches(y)) continue;
        for (unsigned x = left >> 2; 4 * x < right; ++x) {
            if (!antialias && (4 * y < top || !covers_x(4 * x))) continue;
            unsigned coverage = 0;
            for (unsigned sub = std::max(4 * y, top); sub < std::min(4 * y + 4, bottom); ++sub) {
                const unsigned sample = 4 * x + (sub & 1);
                coverage += covers_x(sample);
                coverage += covers_x(sample + 2);
            }
            if (coverage == 0) continue;
            if (use_depth && !depth_test(x, y, z, dz, coverage, rdram)) continue;
            if (write_opaque_pixel(x, y, color, Rgba{}, coverage, rdram) && use_depth) write_depth(x, y, z, dz, rdram);
        }
    }
}

void Rdp::draw_triangle(std::span<std::uint8_t> rdram) const {
    const std::uint64_t header = render_command_[0];
    const unsigned op = opcode(header);
    const bool textured = (op & 2) != 0, shaded = (op & 4) != 0;
    // Primitive depth also lets triangles without Z coefficients use the depth buffer.
    const bool depth = (op & 1) != 0 || (solid_.modes & (1U << 2));
    const unsigned base_tile = static_cast<unsigned>(header >> 48) & 7;
    const bool fill = !textured && can_fill();
    if (!fill && !can_write_opaque(depth)) return;
    // Without texture coefficients, S and T stay at zero. TEXEL1 comes from the next tile.
    const unsigned texels = fill ? 0 : texels_read();
    const bool sample = texels != 0;
    // With LOD, the tile is picked per pixel from base_tile up to base_tile + max level.
    const bool lod = sample && (solid_.modes & (1ULL << 48));
    const unsigned max_level = static_cast<unsigned>(header >> 51) & 7;
    // The second texel only moves past the first below the last level, so max level is the highest tile.
    // Detail textures shift the chain up one tile.
    const unsigned last_tile = max_level + ((solid_.modes & (1ULL << 50)) ? 1 : 0);
    for (unsigned level = 0; level <= (lod ? last_tile : 1); ++level) {
        const bool used = lod || (level == 0 ? (texels & 1) : (texels & 2));
        if (used && !can_sample_texture(texture_.tiles[(base_tile + level) & 7], true)) return;
    }
    // Coefficient blocks follow the edges in shade, texture, depth order.
    const unsigned texture_at = shaded ? 12 : 4, depth_at = texture_at + (textured ? 8 : 0);

    const auto yh = sign_extend(static_cast<std::uint32_t>(header), 14);
    const auto ym = sign_extend(static_cast<std::uint32_t>(header >> 16), 14);
    const auto yl = sign_extend(static_cast<std::uint32_t>(header >> 32), 14);
    const int top = std::max(yh, static_cast<int>(coordinate(scissor_, 32)));
    const int bottom = std::min(yl, static_cast<int>(coordinate(scissor_, 0)));
    const int clip_left = static_cast<int>(coordinate(scissor_, 44)) * 2;
    const int clip_right = static_cast<int>(coordinate(scissor_, 12)) * 2;
    if (top >= bottom || clip_left >= clip_right) return;

    const bool major_left = (header & (1ULL << 55)) != 0;
    const TriangleEdge major(render_command_[2], yh & ~3);
    const TriangleEdge upper(render_command_[3], yh & ~3);
    const TriangleEdge lower(render_command_[1], ym);
    const bool perspective = sample && textured && (solid_.modes & kTexturePerspective);
    TextureGradient s, t, w;
    if (sample && textured) {
        // STW integer and fractional halves are stored in separate words.
        const auto gradient = [&](unsigned shift) -> TextureGradient {
            const unsigned i = texture_at;
            return {texture_coefficient(render_command_[i], render_command_[i + 2], shift),
                    texture_coefficient(render_command_[i + 1], render_command_[i + 3], shift),
                    texture_coefficient(render_command_[i + 4], render_command_[i + 6], shift),
                    texture_coefficient(render_command_[i + 5], render_command_[i + 7], shift)};
        };
        s = gradient(48);
        t = gradient(32);
        if (perspective) w = gradient(16);
    }
    std::array<TextureGradient, 4> shade{};
    if (shaded) {
        // RGBA coefficients share the STW layout: integers and fractions in separate words.
        for (unsigned i = 0; i < shade.size(); ++i) {
            const unsigned at = 48 - 16 * i;
            shade[i] = {texture_coefficient(render_command_[4], render_command_[6], at),
                        texture_coefficient(render_command_[5], render_command_[7], at),
                        texture_coefficient(render_command_[8], render_command_[10], at),
                        texture_coefficient(render_command_[9], render_command_[11], at)};
        }
    }
    TextureGradient z;
    const bool use_depth = !fill && (solid_.modes & ((1U << 4) | (1U << 5)));
    if (use_depth && (op & 1)) {
        const auto word = [this](unsigned i, unsigned shift) {
            return static_cast<std::int32_t>(render_command_[i] >> shift);
        };
        z = {word(depth_at, 32), word(depth_at, 0), word(depth_at + 1, 32), word(depth_at + 1, 0)};
    }
    // Pixel DZ is the sum of the X and Y slopes (or the primitive's DZ), rounded to a power of two.
    const auto magnitude = [](std::int32_t slope) { return static_cast<std::int64_t>(slope < 0 ? ~slope : slope); };
    const unsigned pixel_dz = highest_power_of_two(
        (solid_.modes & (1U << 2)) ? prim_depth_ & 0xFFFF
                                   : static_cast<unsigned>(std::min<std::int64_t>(
                                         (magnitude(z.dx) + magnitude(z.dy)) >> 16, 0x7FFF)));
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
        std::array<std::int64_t, 4> start_shade{};
        std::int64_t start_z = 0;
        if (sample || shaded || use_depth) {
            const int edge_x = major.fixed_x(4 * y + (late ? 3 : 0));
            const int x_fraction = (edge_x >> 8) & 255;
            const int lines = y - (yh >> 2);
            texture_x = edge_x >> 16;
            start_s = s.scanline_start(lines, x_fraction, late);
            start_t = t.scanline_start(lines, x_fraction, late);
            if (perspective) start_w = w.scanline_start(lines, x_fraction, late);
            for (unsigned i = 0; i < shade.size(); ++i) start_shade[i] = shade[i].scanline_start(lines, x_fraction, late);
            start_z = z.scanline_start(lines, x_fraction, late);
        }
        for (int x = left >> 3; x <= (right >> 3); ++x) {
            if (!antialias && !samples[0].covers(x * 8)) continue;
            unsigned coverage = 0;
            for (unsigned row = 0; row < samples.size(); ++row) {
                const int sample = x * 8 + static_cast<int>(row & 1) * 2;
                coverage += samples[row].covers(sample);
                coverage += samples[row].covers(sample + 4);
            }
            std::uint32_t pixel_z = 0;
            if (coverage != 0 && use_depth) {
                pixel_z = (solid_.modes & (1U << 2)) ? ((prim_depth_ >> 16) & 0x7FFF) << 3
                                                     : clamp_depth(start_z + std::int64_t{z.dx} * (x - texture_x));
                if (!depth_test(static_cast<unsigned>(x), static_cast<unsigned>(y), pixel_z, pixel_dz, coverage, rdram)) {
                    continue;
                }
            }
            if (coverage != 0) {
                const int dx = x - texture_x;
                Rgba texel0{}, texel1{}, shade_color{};  // Triangles without shade coefficients have zero shade.
                int lod_fraction = 0;
                Rgba next_texel0{};
                if (sample) {
                    const auto coordinates = [&](std::int64_t s_start, std::int64_t t_start, std::int64_t w_start, int at) {
                        const int sample_s = s.sample(s_start, at), sample_t = t.sample(t_start, at);
                        return perspective ? project_texture_coordinates(sample_s, sample_t, w.sample(w_start, at))
                                           : std::array{sample_s, sample_t};
                    };
                    // Samples one pixel's texels; returns its LOD fraction.
                    const auto sample_pixel = [&](int at, Rgba& out0, Rgba* out1) {
                        const auto [u, v] = coordinates(start_s, start_t, start_w, at);
                        unsigned first = base_tile, second = base_tile + 1;
                        int fraction = 0;
                        if (lod) {
                            // LOD is the largest texel step to the next pixel and the next line (10.5).
                            const auto right = coordinates(start_s, start_t, start_w, at + 1);
                            const auto down = coordinates(start_s + s.dy, start_t + t.dy, start_w + w.dy, at);
                            const int step = std::max({std::abs(right[0] - u), std::abs(right[1] - v),
                                                       std::abs(down[0] - u), std::abs(down[1] - v)});
                            const auto level = select_lod(step, max_level);
                            first = base_tile + level.first;
                            second = base_tile + level.second;
                            fraction = level.fraction;
                        }
                        if (texels & 1) out0 = sample_texel(texture_.tiles[first & 7], u, v);
                        if (out1 && (texels & 2)) *out1 = sample_texel(texture_.tiles[second & 7], u, v);
                        return fraction;
                    };
                    lod_fraction = sample_pixel(dx, texel0, &texel1);
                    // Two-cycle mode pipelines texels: cycle 1's TEXEL1 is the next pixel's TEXEL0.
                    if (cycle_type_ == 1) sample_pixel(dx + 1, next_texel0, nullptr);
                }
                if (shaded) {
                    for (unsigned i = 0; i < shade.size(); ++i) shade_color[i] = clamp_shade(start_shade[i], shade[i], dx);
                }
                // Z is only updated for pixels the blender actually writes (alpha compare can drop them).
                if (write_opaque_pixel(static_cast<unsigned>(x), static_cast<unsigned>(y),
                                       combine(texel0, texel1, shade_color, lod_fraction, &next_texel0),
                                       shade_color, coverage,
                                       rdram) &&
                    use_depth) {
                    write_depth(static_cast<unsigned>(x), static_cast<unsigned>(y), pixel_z, pixel_dz, rdram);
                }
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
    // Fills clear the hidden bits they cover. Hardware copies each halfword's low bit into them;
    // only depth reads use them here, and depth clears (0xFFFC) have that bit clear.
    if (first < end) {
        const std::size_t from = std::min(first >> 1, hidden_.size());
        const std::size_t to = std::min((end + 1) >> 1, hidden_.size());
        std::fill(hidden_.begin() + static_cast<std::ptrdiff_t>(from), hidden_.begin() + static_cast<std::ptrdiff_t>(to),
                  std::uint8_t{0});
    }
}

bool Rdp::can_write_opaque(bool depth) const {
    // Depth is only modeled for primitives that carry one.
    const bool depth_modes = (solid_.modes & ((1U << 4) | (1U << 5))) != 0;
    if (depth_modes && !depth) return false;
    return cycle_type_ <= 1 && color_width_ != 0 && solid_.format == 0 && (pixel_size_ == 2 || pixel_size_ == 3) &&
           ((solid_.modes >> 38) & 3) != 2;
}

namespace {

enum CombinerSource : unsigned { kCombined, kTexel0, kTexel1, kPrimitive, kShade, kEnvironment, kOne, kZero };

// 9-bit combiner result: overflow saturates to 255 and negative values to 0.
unsigned clamp_combined(int value) {
    const auto bits = static_cast<unsigned>(value) & 0x1FF;
    if (!(bits & 0x100)) return bits;
    return (bits & 0x80) ? 0 : 255;
}

}  // namespace

namespace {

// Bit positions of one combiner cycle's selectors in Set Combine.
struct CombinerFields {
    unsigned a, b, c, d, alpha_a, alpha_b, alpha_c, alpha_d;
};
constexpr CombinerFields kCycle0{52, 28, 47, 15, 44, 12, 41, 9};
constexpr CombinerFields kCycle1{37, 24, 32, 6, 21, 3, 18, 0};

}  // namespace

unsigned Rdp::texels_read() const {
    // Returns bit 0 for the primitive's tile and bit 1 for the next tile.
    const auto field = [this](unsigned shift, unsigned mask) {
        return static_cast<unsigned>(solid_.combine >> shift) & mask;
    };
    const auto sources = [&](const CombinerFields& f) {
        const unsigned multiplier = field(f.c, 31);
        const unsigned rgb_c = multiplier >= 7 && multiplier <= 12 ? multiplier - 7 : multiplier;
        return std::array{field(f.a, 15), field(f.b, 15), rgb_c, field(f.d, 7),
                          field(f.alpha_a, 7), field(f.alpha_b, 7), field(f.alpha_c, 7), field(f.alpha_d, 7)};
    };
    unsigned used = 0;
    const auto mark = [&](const CombinerFields& f, unsigned texel0_bit, unsigned texel1_bit) {
        for (const unsigned source : sources(f)) {
            if (source == kTexel0) used |= texel0_bit;
            if (source == kTexel1) used |= texel1_bit;
        }
    };
    if (cycle_type_ == 1) {
        mark(kCycle0, 1, 2);
        mark(kCycle1, 2, 1);  // Cycle 1 sees TEXEL1 as TEXEL0 and the next pixel's TEXEL0 as TEXEL1.
    } else {
        mark(kCycle1, 1, 1);  // One cycle reads both texel inputs from the same tile.
    }
    return used;
}

Rdp::LodLevel Rdp::select_lod(int step, unsigned max_level) const {
    // step is in 10.5 texels per pixel. Each mip level halves the texture, so the level is
    // log2 of the whole-texel step and the fraction is the position between two levels.
    unsigned lod = static_cast<unsigned>(step);
    if (lod & 0x4000) lod = 0x7FFF;
    lod = std::max(lod, (prim_lod_ >> 8) & 31);  // minimum level from Set Primitive Color
    const bool magnify = lod < 32;
    unsigned level = 0;
    while (level < 7 && (lod >> 5) >> (level + 1)) ++level;
    const bool distant = (lod & 0x6000) || level >= max_level;
    if (distant) level = max_level;
    int fraction = static_cast<int>(((lod << 3) >> level) & 0xFF);
    const bool sharpen = (solid_.modes & (1ULL << 49)) != 0, detail = (solid_.modes & (1ULL << 50)) != 0;
    // Without sharpen or detail textures, magnification and the last level clamp the fraction.
    if (!sharpen && !detail) {
        if (distant) fraction = 0xFF;
        else if (magnify) fraction = 0;
    }
    // Sharpen sets the 9-bit sign while magnifying, so the combiner extrapolates past level 0.
    if (sharpen && magnify) fraction -= 0x100;
    if (!detail) {
        const bool same = distant || (!sharpen && magnify);
        return {level, same ? level : level + 1, fraction};
    }
    // Detail textures sit in the primitive tile; the mip chain starts one tile later.
    const unsigned first = magnify ? 0 : level + 1;
    return {first, (!distant && !magnify) ? level + 2 : level + 1, fraction};
}

unsigned Rdp::next_noise() const {
    // A deterministic generator stands in for the hardware's free-running noise.
    noise_state_ = noise_state_ * 1103515245U + 12345U;
    return noise_state_ >> 16;
}

Rdp::Rgba Rdp::combine(const Rgba& texel0, const Rgba& texel1, const Rgba& shade, int lod_fraction,
                       const Rgba* next_texel0) const {
    // Each cycle evaluates (A - B) * C + D. Two-cycle mode feeds cycle 0 into COMBINED.
    // Key center and scale per channel (R, G, B) and the 4.8 key widths.
    const std::array<int, 3> key_center{static_cast<int>((key_r_ >> 8) & 255), static_cast<int>((key_gb_ >> 24) & 255),
                                        static_cast<int>((key_gb_ >> 8) & 255)};
    const std::array<int, 3> key_scale{static_cast<int>(key_r_ & 255), static_cast<int>((key_gb_ >> 16) & 255),
                                       static_cast<int>(key_gb_ & 255)};
    const std::array<int, 3> key_width{static_cast<int>((key_r_ >> 16) & 0xFFF),
                                       static_cast<int>((key_gb_ >> 44) & 0xFFF),
                                       static_cast<int>((key_gb_ >> 32) & 0xFFF)};
    // Noise is a 9-bit signed value with only its top three bits random.
    const int noise = [&] {
        const unsigned value = ((next_noise() & 7) << 6) | 0x20;
        return value & 0x100 ? static_cast<int>(value) - 0x200 : static_cast<int>(value);
    }();
    std::array<int, 3> raw{};  // the final cycle's unclamped RGB, for the chroma key
    // Set Convert's K4 and K5 are 9-bit values; the YUV converter's K0-K3 are not modeled.
    const int k4 = static_cast<int>((convert_ >> 9) & 0x1FF), k5 = static_cast<int>(convert_ & 0x1FF);
    const int prim_lod_fraction = static_cast<int>(prim_lod_ & 0xFF);
    const auto unpack = [](std::uint32_t rgba) {
        return Rgba{rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255};
    };
    const Rgba primitive = unpack(solid_.primitive), environment = unpack(solid_.environment);
    const auto field = [this](unsigned shift, unsigned mask) {
        return static_cast<unsigned>(solid_.combine >> shift) & mask;
    };
    const auto unclamped = [](int a, int b, int c, int d) { return ((a - b) * c + (d << 8) + 0x80) >> 8; };
    const auto equation = [&](int a, int b, int c, int d) { return clamp_combined(unclamped(a, b, c, d)); };

    const auto cycle = [&](const CombinerFields& f, const Rgba& combined, const Rgba& t0, const Rgba& t1) {
        const auto input = [&](unsigned source, unsigned channel) -> int {
            switch (source) {
                case kCombined: return static_cast<int>(combined[channel]);
                case kTexel0: return static_cast<int>(t0[channel]);
                case kTexel1: return static_cast<int>(t1[channel]);
                case kPrimitive: return static_cast<int>(primitive[channel]);
                case kShade: return static_cast<int>(shade[channel]);
                case kEnvironment: return static_cast<int>(environment[channel]);
                case kOne: return 255;
                default: return 0;
            }
        };
        Rgba out{};
        const unsigned sub_a = field(f.a, 15), sub_b = field(f.b, 15), multiplier = field(f.c, 31), add = field(f.d, 7);
        for (unsigned i = 0; i < 3; ++i) {
            // Inputs 6 and up differ per slot: A has ONE then NOISE, B has KEY CENTER then K4.
            const int a = sub_a < 6 ? input(sub_a, i) : (sub_a == 6 ? 255 : sub_a == 7 ? noise : 0);
            const int b = sub_b < 6 ? input(sub_b, i) : (sub_b == 6 ? key_center[i] : sub_b == 7 ? k4 : 0);
            int c = 0;
            if (multiplier < 6) c = input(multiplier, i);
            else if (multiplier == 6) c = key_scale[i];
            else if (multiplier >= 7 && multiplier <= 12) c = input(multiplier - 7, 3);  // alpha inputs
            else if (multiplier == 13) c = lod_fraction;
            else if (multiplier == 14) c = prim_lod_fraction;
            else if (multiplier == 15) c = k5;
            raw[i] = unclamped(a, b, c, input(add, i));
            out[i] = clamp_combined(raw[i]);
        }
        // Alpha multiplier inputs 0 and 6 are the LOD fraction and the primitive LOD fraction.
        const unsigned alpha_multiplier = field(f.alpha_c, 7);
        const int c = alpha_multiplier == 0 ? lod_fraction
                    : alpha_multiplier == 6 ? prim_lod_fraction
                                            : input(alpha_multiplier, 3);
        out[3] = equation(input(field(f.alpha_a, 7), 3), input(field(f.alpha_b, 7), 3), c, input(field(f.alpha_d, 7), 3));
        return out;
    };

    Rgba result{};
    if (cycle_type_ != 1) {
        result = cycle(kCycle1, Rgba{}, texel0, texel1);  // COMBINED has no earlier cycle.
    } else {
        const Rgba first = cycle(kCycle0, Rgba{}, texel0, texel1);
        // Cycle 1 sees this pixel's TEXEL1 as TEXEL0 and the next pixel's TEXEL0 as TEXEL1.
        // Callers without a next pixel fall back to this pixel's TEXEL0.
        result = cycle(kCycle1, first, texel1, next_texel0 ? *next_texel0 : texel0);
    }
    if (solid_.modes & (1ULL << 40)) {
        // Chroma key: each channel's distance from the key center (already scaled by the
        // equation) is subtracted from its width; the smallest margin becomes the alpha.
        int key_alpha = 255;
        for (unsigned i = 0; i < 3; ++i) {
            int key = (raw[i] << 15) >> 15;  // 17-bit signed
            if (key > 0) key = (key & 15) == 8 ? 16 - key : -key;
            key_alpha = std::min(key_alpha, (key_width[i] << 4) + key);
        }
        result[3] = static_cast<unsigned>(std::clamp(key_alpha, 0, 255));
    }
    return result;
}
unsigned Rdp::memory_coverage(unsigned x, unsigned y, std::span<const std::uint8_t> rdram) const {
    // Without image reads, memory coverage reads as full (7). RGBA5551 keeps only the top
    // coverage bit; the two hidden low bits are taken as set.
    if (!(solid_.modes & (1U << 6))) return 7;
    const unsigned bytes = pixel_size_ == 2 ? 2 : 4;
    const std::size_t address = (color_address_ & ~(bytes - 1)) +
        (static_cast<std::size_t>(y) * color_width_ + x) * bytes;
    if (address + bytes > rdram.size()) return 7;
    if (bytes == 2) return ((rdram[address + 1] & 1U) << 2) | 3;
    return rdram[address + 3] >> 5;
}

bool Rdp::depth_test(unsigned x, unsigned y, std::uint32_t z, unsigned dz, unsigned& coverage,
                     std::span<std::uint8_t> rdram) const {
    const std::size_t address = (mask_address_ & ~1U) + (static_cast<std::size_t>(y) * color_width_ + x) * 2;
    if (address + 2 > rdram.size()) return false;
    if (!(solid_.modes & (1U << 4))) return true;
    const unsigned stored = (unsigned{rdram[address]} << 8) | rdram[address + 1];
    const std::uint32_t memory_z = decompress_depth(stored >> 2);
    // The DZ code's low two bits live in the RDRAM hidden bits of the same halfword.
    const unsigned memory_dz_code = ((stored & 3) << 2) | hidden_bits(address);
    unsigned memory_dz = 1U << memory_dz_code;
    // Low Z exponents have more precision than DZ can describe, so memory DZ is widened.
    bool coplanar = false;
    if ((stored >> 13) < 3) {
        if (memory_dz != 0x8000) memory_dz = std::max(memory_dz << 1, 16U >> (stored >> 13));
        else coplanar = true, memory_dz = 0xFFFF;
    }
    const unsigned range_shift = compress_dz(dz | memory_dz);
    const std::int64_t range = std::int64_t{highest_power_of_two(dz | memory_dz)} << 3;
    const std::int64_t pixel = z, memory = memory_z;
    const bool farther = coplanar || pixel + range >= memory;
    const bool nearer = coplanar || pixel - range <= memory;
    const bool in_front = pixel < memory;
    const bool max = memory_z == 0x3FFFF;
    const bool overflow = ((coverage + memory_coverage(x, y, rdram)) & 8) != 0;
    switch ((solid_.modes >> 10) & 3) {
        case 1:  // interpenetrating
            // Where a full pixel pierces the stored surface within the DZ range, coverage is
            // scaled by how far in front it is, so intersecting surfaces antialias their seam.
            // TODO: the blender shift for the non-piercing case.
            if (in_front && farther && overflow) {
                const unsigned depth_coverage = static_cast<unsigned>(((memory >> range_shift) - (pixel >> range_shift)) & 15);
                coverage = ((depth_coverage * coverage) >> 3) & 15;
            }
            return max || (overflow ? in_front : nearer);
        case 2: return in_front || max;                             // transparent
        case 3: return farther && nearer && !max;                   // decal
        default: return max || (overflow ? in_front : nearer);      // opaque
    }
}

unsigned Rdp::hidden_bits(std::size_t address) const {
    const std::size_t index = address >> 1;
    return index < hidden_.size() ? hidden_[index] : 0;
}

void Rdp::set_hidden_bits(std::size_t address, unsigned bits) const {
    const std::size_t index = address >> 1;
    if (index < hidden_.size()) hidden_[index] = static_cast<std::uint8_t>(bits & 3);
}

void Rdp::write_depth(unsigned x, unsigned y, std::uint32_t z, unsigned dz, std::span<std::uint8_t> rdram) const {
    if (!(solid_.modes & (1U << 5))) return;
    const std::size_t address = (mask_address_ & ~1U) + (static_cast<std::size_t>(y) * color_width_ + x) * 2;
    if (address + 2 > rdram.size()) return;
    // Only the top two bits of the 4-bit DZ code fit in RDRAM; the low two are hidden bits.
    const unsigned code = compress_dz(dz);
    const unsigned packed = (compress_depth(z) << 2) | (code >> 2);
    rdram[address] = static_cast<std::uint8_t>(packed >> 8);
    rdram[address + 1] = static_cast<std::uint8_t>(packed);
    set_hidden_bits(address, code & 3);
}

bool Rdp::write_opaque_pixel(unsigned x, unsigned y, const Rgba& combined, const Rgba& shade, unsigned coverage,
                             std::span<std::uint8_t> rdram) const {
    const unsigned bytes = pixel_size_ == 2 ? 2 : 4;
    const std::size_t address = (color_address_ & ~(bytes - 1)) +
        (static_cast<std::size_t>(y) * color_width_ + x) * bytes;
    if (address + bytes > rdram.size()) return false;

    // Coverage times alpha scales the 0-8 coverage count by the combined alpha; alpha
    // coverage select then replaces the pixel alpha with the (scaled) coverage.
    unsigned alpha = combined[3];
    const unsigned scaled = alpha * coverage + 4;
    if (solid_.modes & (1U << 12)) coverage = (scaled >> 8) & 15;
    if (solid_.modes & (1U << 13)) {
        alpha = (solid_.modes & (1U << 12)) ? std::min(255U, scaled >> 5) : std::min(255U, coverage << 5);
    }
    // Alpha compare drops pixels whose alpha is below the blend color alpha.
    // With dither alpha the threshold is random instead of the blend color alpha.
    if (solid_.modes & 1) {
        const unsigned threshold = (solid_.modes & 2) ? next_noise() & 255 : solid_.blend & 255;
        if (alpha < threshold) return false;
    }
    if (coverage == 0) return false;

    // Memory color and coverage. Without image reads, memory coverage reads as full (7).
    // RGBA5551 keeps only the top coverage bit; the two hidden low bits are taken as set.
    std::array<unsigned, 3> memory{};
    unsigned memory_coverage = 7;
    if (solid_.modes & (1U << 6)) {
        if (bytes == 2) {
            const unsigned value = (unsigned{rdram[address]} << 8) | rdram[address + 1];
            memory = {((value >> 11) & 31) << 3, ((value >> 6) & 31) << 3, ((value >> 1) & 31) << 3};
            memory_coverage = ((value & 1) << 2) | 3;
        } else {
            memory = {rdram[address], rdram[address + 1], rdram[address + 2]};
            memory_coverage = rdram[address + 3] >> 5;
        }
    }

    // Each blender cycle computes (P * A + M * B) / (A + B). One cycle uses cycle 0's
    // selectors. Two-cycle mode always blends cycle 0 without dividing and feeds the
    // result to cycle 1 as its pixel input. In the final cycle, without force blend,
    // only pixels whose coverage does not overflow memory coverage (edges) are blended.
    const bool overflow = coverage + memory_coverage >= 8;
    const bool force_blend = (solid_.modes & (1U << 14)) != 0;
    const auto constant = [](std::uint32_t rgba) {
        return std::array<unsigned, 3>{rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255};
    };
    const auto blend_cycle = [&](const std::array<unsigned, 3>& pixel, unsigned p_shift, unsigned a_shift,
                                 unsigned m_shift, unsigned b_shift, bool final) {
        const auto input = [&](unsigned shift) {
            switch ((solid_.modes >> shift) & 3) {
                case 1: return memory;
                case 2: return constant(solid_.blend);
                case 3: return constant(solid_.fog);
                default: return pixel;
            }
        };
        const auto first = input(p_shift);
        if (final && !force_blend && overflow) return first;
        const unsigned factor = [&] {
            switch ((solid_.modes >> a_shift) & 3) {
                case 0: return alpha;
                case 1: return solid_.fog & 255;
                case 2: return shade[3];
                default: return 0U;
            }
        }();
        const auto second = input(m_shift);
        // Both factors keep five bits; 1 - A is the complement, so the weights sum to 32.
        const unsigned a = factor >> 3;
        const unsigned m = [&] {
            switch ((solid_.modes >> b_shift) & 3) {
                case 0: return 32 - a;
                case 1: return memory_coverage << 2;
                case 2: return 31U;
                default: return 0U;
            }
        }();
        std::array<unsigned, 3> out{};
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned sum = first[i] * a + second[i] * m;
            if (!final || force_blend) out[i] = std::min(255U, sum >> 5);
            else out[i] = (a + m) != 0 ? std::min(255U, sum / (a + m)) : first[i];
        }
        return out;
    };
    std::array<unsigned, 3> blended{combined[0], combined[1], combined[2]};
    if (cycle_type_ == 1) {
        blended = blend_cycle(blended, 30, 26, 22, 18, false);
        blended = blend_cycle(blended, 28, 24, 20, 16, true);
    } else {
        blended = blend_cycle(blended, 30, 26, 22, 18, true);
    }
    unsigned r = blended[0], g = blended[1], b = blended[2];    const unsigned dither = static_cast<unsigned>(solid_.modes >> 38) & 3;
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

    // Coverage destination: clamp, wrap, zap or save. An overflowing pixel that replaces
    // memory keeps its own coverage, stored as count - 1.
    const unsigned sum = coverage + memory_coverage;
    const unsigned stored = [&] {
        switch ((solid_.modes >> 8) & 3) {
            case 0: return overflow ? (force_blend ? 7U : coverage - 1) : sum;
            case 1: return sum & 7;
            case 2: return 7U;
            default: return memory_coverage;
        }
    }();
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
    return true;
}

}  // namespace ezy::n64
