#include "n64/rdp.hpp"

#include <algorithm>

namespace ezy::n64 {

namespace {

constexpr unsigned kHalfwordsPerWord = 4;
constexpr unsigned kBytesPerWord = 8;
constexpr unsigned kCopyPixelsPerGroup = 4;
constexpr unsigned kTmemMask = 0x7FF;  // Addresses are in halfwords.
constexpr unsigned kTlutBase = 0x400;

unsigned field12(std::uint64_t word, unsigned shift) {
    return static_cast<unsigned>(word >> shift) & 0xFFF;
}

unsigned texture_size(std::uint64_t word) {
    return static_cast<unsigned>(word >> 51) & 3;
}

unsigned texture_format(std::uint64_t word) {
    return static_cast<unsigned>(word >> 53) & 7;
}

bool supported_texture(std::uint64_t descriptor, bool indexed) {
    const unsigned size = texture_size(descriptor), format = texture_format(descriptor);
    return indexed ? (size <= 1 && (format == 0 || format == 2)) : (size == 2 && format == 0);
}

unsigned image_width(std::uint64_t word) {
    return (static_cast<unsigned>(word >> 32) & 0x3FF) + 1;
}

std::uint16_t read_texture_halfword(std::span<const std::uint8_t> rdram, unsigned address) {
    address &= 0xFFFFFF;
    if (address + 1 >= rdram.size()) return 0;
    return static_cast<std::uint16_t>((rdram[address] << 8) | rdram[address + 1]);
}

int shift_texture_coord(int coord, unsigned shift) {
    coord = std::clamp(coord, -32768, 32767);
    if (shift < 11) return coord >> shift;
    return static_cast<std::int16_t>(static_cast<unsigned>(coord) << (16 - shift));
}

int texture_coord(int coord, unsigned origin, unsigned shift) {
    return (shift_texture_coord(coord, shift) - static_cast<int>(origin * 8)) >> 5;
}

unsigned wrap_coord(int coord, unsigned mask_bits, bool mirror) {
    const auto bits = static_cast<unsigned>(coord);
    if (mask_bits == 0) return bits;
    const unsigned period = 1U << std::min(mask_bits, 10U);
    return (mirror && (bits & period) ? ~bits : bits) & (period - 1);
}

int clamp_texture_coord(int coord, unsigned low, unsigned high, unsigned sampler) {
    const unsigned mask = (sampler >> 4) & 15;
    coord = shift_texture_coord(coord, sampler & 15);
    int relative = coord - static_cast<int>(low * 8);  // Convert the 10.2 tile origin to 10.5.
    if ((sampler & 0x200) || mask == 0) {
        if ((coord >> 3) >= static_cast<int>(high)) {
            relative = static_cast<int>(((high >> 2) - (low >> 2)) & 0x3FF) * 32;
        } else {
            relative = std::max(relative, 0);
        }
    }
    return relative;
}

int wrap_texture_coord(int coord, unsigned sampler) {
    return static_cast<int>(wrap_coord(coord, (sampler >> 4) & 15, (sampler & 0x100) != 0));
}

unsigned expand_5_to_8(unsigned value) {
    return (value << 3) | (value >> 2);
}

}  // namespace

void Rdp::load_texture(std::uint64_t word, bool block, std::span<const std::uint8_t> rdram) {
    auto& tile = texture_.tiles[(word >> 24) & 7];
    tile.bounds = word;  // Loads also latch the tile coordinates.
    const unsigned size = texture_size(texture_.image);
    // CI4 is uploaded in 8- or 16-bit units, then rendered with a 4-bit tile.
    if ((size != 1 && size != 2) || size != texture_size(tile.descriptor) ||
        texture_format(tile.descriptor) == 1) return;  // YUV uses a separate layout.
    const unsigned bytes_per_texel = 1U << (size - 1);
    const unsigned texels_per_word = kBytesPerWord / bytes_per_texel;

    const unsigned stride = static_cast<unsigned>(tile.descriptor >> 41) & 0x1FF;
    const unsigned base = static_cast<unsigned>(tile.descriptor >> 32) & 0x1FF;
    const unsigned width = image_width(texture_.image);
    const unsigned image_address = static_cast<unsigned>(texture_.image) & 0xFFFFFF;
    const unsigned sl = field12(word, 44), tl = field12(word, 32);
    const unsigned sh = field12(word, 12), th = field12(word, 0);

    const auto transfer_word = [&](unsigned destination, unsigned source_texel, unsigned row) {
        const unsigned address = image_address + source_texel * bytes_per_texel;
        for (unsigned i = 0; i < kHalfwordsPerWord; ++i) {
            // Odd rows exchange the two 32-bit halves of each TMEM word.
            const unsigned index = destination * kHalfwordsPerWord + (i ^ ((row & 1) * 2));
            texture_.tmem[index & kTmemMask] = read_texture_halfword(rdram, address + i * 2);
        }
    };

    if (block) {
        // Load Block uses integer source coordinates. DXT advances once per 64-bit word.
        const unsigned pixels = (sh - sl + 1) & 0xFFF;
        if (pixels > texture_.tmem.size()) return;
        const unsigned source = tl * width + sl;
        for (unsigned group = 0; group * texels_per_word < pixels; ++group) {
            const unsigned row = (group * th) >> 11;
            transfer_word(base + group + row * stride, source + group * texels_per_word, row);
        }
        return;
    }

    // Load Tile uses 10.2 coordinates and an inclusive lower-right texel.
    const unsigned left = sl >> 2, top = tl >> 2, right = sh >> 2, bottom = th >> 2;
    if (right < left || bottom < top) return;
    for (unsigned row = 0; row <= bottom - top; ++row) {
        const unsigned source = (top + row) * width + left;
        for (unsigned group = 0; group * texels_per_word <= right - left; ++group) {
            transfer_word(base + row * stride + group, source + group * texels_per_word, row);
        }
    }
}

void Rdp::load_palette(std::uint64_t word, std::span<const std::uint8_t> rdram) {
    auto& tile = texture_.tiles[(word >> 24) & 7];
    tile.bounds = word;
    const unsigned base = (static_cast<unsigned>(tile.descriptor >> 32) & 0x1FF) * kHalfwordsPerWord;
    const unsigned first = field12(word, 44) >> 2, last = field12(word, 12) >> 2;
    const unsigned row = field12(word, 32) >> 2;
    if (texture_size(texture_.image) != 2 || base < kTlutBase || first > last ||
        (field12(word, 0) >> 2) != row) return;
    const unsigned count = last - first + 1;
    if (base + count * kHalfwordsPerWord > texture_.tmem.size()) return;
    const unsigned image_address = static_cast<unsigned>(texture_.image) & 0xFFFFFF;
    const unsigned source = row * image_width(texture_.image) + first;
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = read_texture_halfword(rdram, image_address + (source + i) * 2);
        // Each entry is replicated across the four TMEM banks.
        std::fill_n(texture_.tmem.begin() + base + i * kHalfwordsPerWord, kHalfwordsPerWord, entry);
    }
}

std::uint16_t Rdp::read_texel(const Tile& tile, unsigned s, unsigned t, unsigned bank) const {
    const unsigned size = texture_size(tile.descriptor);
    const bool indexed = (solid_.modes & (1ULL << 47)) != 0;
    const unsigned stride = static_cast<unsigned>(tile.descriptor >> 41) & 0x1FF;
    const unsigned base = static_cast<unsigned>(tile.descriptor >> 32) & 0x1FF;
    // Address in nibbles so the same calculation works for 4-, 8- and 16-bit tiles.
    const unsigned row = (base + t * stride) * kBytesPerWord * 2;
    const unsigned nibble = (row + (s << size)) ^ ((t & 1) * kBytesPerWord);
    const unsigned tmem_mask = indexed ? kTlutBase - 1 : kTmemMask;
    const auto texel = texture_.tmem[(nibble >> 2) & tmem_mask];
    if (!indexed) return texel;
    const unsigned shift = 4 * (4 - (1U << size) - (nibble & 3));
    const unsigned palette = static_cast<unsigned>(tile.descriptor >> 20) & 15;
    const unsigned entry = size == 0 ? ((palette << 4) | ((texel >> shift) & 15))
                                     : ((texel >> shift) & 255);
    return texture_.tmem[kTlutBase + entry * kHalfwordsPerWord + bank];
}

Rdp::Rgba Rdp::sample_texture_rgba(const Tile& tile, int s, int t) const {
    const unsigned sampler_s = static_cast<unsigned>(tile.descriptor);
    const unsigned sampler_t = static_cast<unsigned>(tile.descriptor >> 10);
    const int s0 = wrap_texture_coord(s >> 5, sampler_s);
    const int wrapped_t = wrap_texture_coord(t >> 5, sampler_t);
    const int t0 = wrapped_t & 255;
    const bool intensity = ((solid_.modes >> 46) & 3) == 3;  // IA16 palette entries.
    const auto fetch = [&](int x, int y, unsigned bank) -> Rgba {
        const unsigned texel = read_texel(tile, static_cast<unsigned>(x), static_cast<unsigned>(y), bank);
        if (intensity) return {texel >> 8, texel >> 8, texel >> 8, texel & 255};
        return {expand_5_to_8(texel >> 11), expand_5_to_8((texel >> 6) & 31), expand_5_to_8((texel >> 1) & 31),
                (texel & 1) ? 255U : 0U};
    };
    if (!(solid_.modes & (1ULL << 45))) return fetch(s0, t0, 0);  // Point sampling.

    const int s1 = wrap_texture_coord((s >> 5) + 1, sampler_s);
    // The next row uses the wrapped offset from the first row's 8-bit address.
    const int t1 = t0 + std::max(wrap_texture_coord((t >> 5) + 1, sampler_t) - wrapped_t, -255);
    const unsigned frac_s = static_cast<unsigned>(s) & 31, frac_t = static_cast<unsigned>(t) & 31;
    const bool upper = frac_s + frac_t >= 32;
    const unsigned bank_xor = upper ? 3 : 0;
    const auto right = fetch(s1, t0, 1 ^ bank_xor);
    const auto down = fetch(s0, t1, 2 ^ bank_xor);
    Rgba result{};
    if ((solid_.modes & (1ULL << 44)) && frac_s == 16 && frac_t == 16) {
        const auto top_left = fetch(s0, t0, 3);
        const auto bottom_right = fetch(s1, t1, 0);
        for (unsigned i = 0; i < result.size(); ++i) {
            result[i] = (top_left[i] + right[i] + down[i] + bottom_right[i] + 2) >> 2;
        }
        return result;
    }

    // Select the triangle on either side of the diagonal from top-right to bottom-left.
    const auto base = upper ? fetch(s1, t1, 0) : fetch(s0, t0, 0);
    const unsigned weight_right = upper ? 32 - frac_t : frac_s;
    const unsigned weight_down = upper ? 32 - frac_s : frac_t;
    const unsigned weight_base = 32 - weight_right - weight_down;
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i] = (base[i] * weight_base + right[i] * weight_right + down[i] * weight_down + 16) >> 5;
    }
    return result;
}

bool Rdp::can_sample_texture(const Tile& tile, bool allow_lod) const {
    constexpr std::uint64_t texture_lod = 1ULL << 48;
    constexpr std::uint64_t texture_filter_path = 1ULL << 43;  // TEXEL0 uses sampling instead of color conversion.
    // Callers that pick tiles by LOD pass allow_lod.
    const bool lod = (solid_.modes & texture_lod) != 0;
    if ((lod && !allow_lod) || !(solid_.modes & texture_filter_path) ||
        ((solid_.modes >> 44) & 3) == 1) return false;  // Mid-texel flag without quad sampling.
    return supported_texture(tile.descriptor, (solid_.modes & (1ULL << 47)) != 0);
}

Rdp::Rgba Rdp::sample_texel(const Tile& tile, int s, int t) const {
    s = clamp_texture_coord(s, field12(tile.bounds, 44), field12(tile.bounds, 12),
                            static_cast<unsigned>(tile.descriptor));
    t = clamp_texture_coord(t, field12(tile.bounds, 32), field12(tile.bounds, 0),
                            static_cast<unsigned>(tile.descriptor >> 10));
    return sample_texture_rgba(tile, s, t);
}

void Rdp::draw_textured_rectangle(std::span<std::uint8_t> rdram) const {
    if (solid_.modes & kTexturePerspective) return;  // Rectangles do not supply W.
    const auto header = render_command_[0], coefficients = render_command_[1];
    const unsigned base_tile = static_cast<unsigned>(header >> 24) & 7;
    const unsigned texels = texels_read();
    const int ds_coefficient = static_cast<std::int16_t>(coefficients >> 16);
    const int dt_coefficient = static_cast<std::int16_t>(coefficients);
    // Rectangles carry no max level, so LOD always lands on the last (only) level: max level 0.
    // Their texel step is constant: DsDx and DtDy (s5.10) converted to 10.5.
    const bool lod = texels != 0 && (solid_.modes & (1ULL << 48));
    LodLevel level{0, 1, 0};
    if (lod) level = select_lod(std::max(std::abs(ds_coefficient), std::abs(dt_coefficient)) >> 5, 0);
    const auto& tile = texture_.tiles[(base_tile + level.first) & 7];
    const auto& next_tile = texture_.tiles[(base_tile + level.second) & 7];
    if (!can_write_opaque() || ((texels & 1) && !can_sample_texture(tile, true)) ||
        ((texels & 2) && !can_sample_texture(next_tile, true))) return;

    const unsigned x0 = field12(header, 12), y0 = field12(header, 0);
    const unsigned left = std::max(x0, field12(scissor_, 44));
    const unsigned top = std::max(y0, field12(scissor_, 32));
    const unsigned right = std::min(field12(header, 44), field12(scissor_, 12));
    const unsigned bottom = std::min(field12(header, 32), field12(scissor_, 0));
    if (left >= right || top >= bottom) return;

    const int s0 = static_cast<std::int16_t>(coefficients >> 48), t0 = static_cast<std::int16_t>(coefficients >> 32);
    const int ds = static_cast<std::int16_t>(coefficients >> 16), dt = static_cast<std::int16_t>(coefficients);
    const bool antialias = (solid_.modes & (1U << 3)) != 0;
    const auto covers_x = [left, right](unsigned sample) { return left <= sample && sample < right; };

    for (unsigned y = top >> 2; 4 * y < bottom; ++y) {
        if (!field_matches(y)) continue;
        const int dy = static_cast<int>(y - (y0 >> 2));
        const int t = t0 + ((dy * dt) >> 5);
        for (unsigned x = left >> 2; 4 * x < right; ++x) {
            if (!antialias && (4 * y < top || !covers_x(4 * x))) continue;
            unsigned coverage = 0;
            for (unsigned sub = std::max(4 * y, top); sub < std::min(4 * y + 4, bottom); ++sub) {
                const unsigned sample = 4 * x + (sub & 1);
                coverage += covers_x(sample);
                coverage += covers_x(sample + 2);
            }
            if (coverage == 0) continue;

            // Texture coordinates keep the original rectangle origin after scissoring.
            const int dx_quarters = static_cast<int>(4 * x) - static_cast<int>(x0);
            const int s = s0 + ((dx_quarters * ds) >> 7);
            // Rectangles carry no shade coefficients, so shade inputs read as zero.
            const Rgba texel0 = (texels & 1) ? sample_texel(tile, s, t) : Rgba{};
            const Rgba texel1 = (texels & 2) ? sample_texel(next_tile, s, t) : Rgba{};
            // Two-cycle mode's cycle 1 TEXEL1 is the next pixel's TEXEL0.
            Rgba next_texel0{};
            if (cycle_type_ == 1 && (texels & 1)) {
                next_texel0 = sample_texel(tile, s0 + (((dx_quarters + 4) * ds) >> 7), t);
            }
            write_opaque_pixel(x, y, combine(texel0, texel1, Rgba{}, level.fraction, &next_texel0), Rgba{}, coverage,
                               rdram);
        }
    }
}

void Rdp::copy_rectangle(std::span<std::uint8_t> rdram) const {
    const std::uint64_t header = render_command_[0], coefficients = render_command_[1];
    const auto& tile = texture_.tiles[(header >> 24) & 7];
    const bool indexed = (solid_.modes & (1ULL << 47)) != 0;
    if (pixel_size_ != 2 || solid_.format != 0 || color_width_ == 0 ||
        !supported_texture(tile.descriptor, indexed) || (solid_.modes & kTexturePerspective)) return;

    const unsigned x0 = field12(header, 12), y0 = field12(header, 0);
    const unsigned x1 = field12(header, 44), y1 = field12(header, 32) | 3U;
    const unsigned clip_left = field12(scissor_, 44), clip_right = field12(scissor_, 12);
    const unsigned top = std::max(y0, field12(scissor_, 32));
    const unsigned bottom = std::min(y1, field12(scissor_, 0));
    if (x0 > x1 || clip_left >= clip_right || x0 >= clip_right || x1 < clip_left || top >= bottom) return;
    const unsigned left = std::max(x0, clip_left) >> 2;
    const unsigned right = std::min(x1, clip_right) >> 2;

    const int s0 = static_cast<std::int16_t>(coefficients >> 48), t0 = static_cast<std::int16_t>(coefficients >> 32);
    const int ds = static_cast<std::int16_t>(coefficients >> 16), dt = static_cast<std::int16_t>(coefficients);
    const unsigned sl = field12(tile.bounds, 44), tl = field12(tile.bounds, 32);
    const unsigned shift_s = static_cast<unsigned>(tile.descriptor) & 15;
    const unsigned shift_t = static_cast<unsigned>(tile.descriptor >> 10) & 15;
    const unsigned mask_s = static_cast<unsigned>(tile.descriptor >> 4) & 15;
    const unsigned mask_t = static_cast<unsigned>(tile.descriptor >> 14) & 15;
    const bool mirror_s = (tile.descriptor & (1U << 8)) != 0;
    const bool mirror_t = (tile.descriptor & (1U << 18)) != 0;
    const bool alpha_test = (solid_.modes & 1) != 0;

    for (unsigned y = top >> 2; y * 4 < bottom; ++y) {
        if (!field_matches(y)) continue;
        const auto line = static_cast<int>(y - (y0 >> 2));
        const unsigned t = wrap_coord(texture_coord(t0 + ((line * dt) >> 5), tl, shift_t), mask_t, mirror_t);
        const auto destination = static_cast<std::size_t>(color_address_) +
                                 (static_cast<std::size_t>(y) * color_width_ + left) * 2;
        for (unsigned x = left; x <= right; x += kCopyPixelsPerGroup) {
            // Advance S once per four destination pixels. The tile shift applies to
            // the group base; the four samples then select consecutive texels.
            const auto group = static_cast<int>((x - left) / kCopyPixelsPerGroup);
            const int s0_group = texture_coord(s0 + ((group * ds) >> 5), sl, shift_s);
            const unsigned count = std::min(kCopyPixelsPerGroup, right - x + 1);
            for (unsigned i = 0; i < count; ++i) {
                const unsigned s = wrap_coord(s0_group + static_cast<int>(i), mask_s, mirror_s);
                const auto texel = read_texel(tile, s, t, i);  // COPY uses the raw entry in the pixel's bank.
                const auto address = destination + (x + i - left) * 2;
                if (address + 1 < rdram.size() && (!alpha_test || (texel & 1))) {
                    rdram[address] = static_cast<std::uint8_t>(texel >> 8);
                    rdram[address + 1] = static_cast<std::uint8_t>(texel);
                }
            }
        }
    }
}

}  // namespace ezy::n64
