#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include "n64/rdp_worker.hpp"

namespace ezy::n64 {

// State and execution owned by the raster thread; serialize only after draining.
class RdpRasterizer {
protected:
    void execute_packet(const RdpPacket& packet, std::span<std::uint8_t> rdram);
    static constexpr std::uint64_t kTexturePerspective = 1ULL << 51;
    void execute(std::uint64_t word, std::span<std::uint8_t> rdram);
    [[nodiscard]] bool can_fill() const;
    void fill_rectangle(std::uint64_t word, std::span<std::uint8_t> rdram) const;
    void draw_flat_rectangle(std::uint64_t word, std::span<std::uint8_t> rdram) const;
    void draw_triangle(std::span<std::uint8_t> rdram) const;
    void fill_span(unsigned y, unsigned left, unsigned right, std::span<std::uint8_t> rdram) const;
    [[nodiscard]] bool field_matches(unsigned y) const;
    [[nodiscard]] bool can_write_opaque(bool depth = false) const;
    [[nodiscard]] unsigned hidden_bits(std::size_t address) const;
    void set_hidden_bits(std::size_t address, unsigned bits) const;
    [[nodiscard]] unsigned memory_coverage(unsigned x, unsigned y, std::span<const std::uint8_t> rdram) const;
    [[nodiscard]] bool depth_test(unsigned x, unsigned y, std::uint32_t z, unsigned dz, unsigned& coverage,
                                  std::span<std::uint8_t> rdram) const;
    using Rgba = std::array<unsigned, 4>;
    [[nodiscard]] unsigned texels_read() const;
    unsigned next_noise() const;
    struct LodLevel {
        unsigned first, second;  // TEXEL0 and TEXEL1 tile offsets from the primitive's tile
        int fraction;            // LOD_FRACTION combiner input (9-bit signed)
    };
    [[nodiscard]] LodLevel select_lod(int step, unsigned max_level) const;
    [[nodiscard]] Rgba combine(const Rgba& texel0, const Rgba& texel1, const Rgba& shade, int lod_fraction = 0,
                               const Rgba* next_texel0 = nullptr) const;
    void write_depth(unsigned x, unsigned y, std::uint32_t z, unsigned dz, std::span<std::uint8_t> rdram) const;
    bool write_opaque_pixel(unsigned x, unsigned y, const Rgba& combined, const Rgba& shade, unsigned coverage,
                            std::span<std::uint8_t> rdram) const;
    void load_texture(std::uint64_t word, bool block, std::span<const std::uint8_t> rdram);
    void load_palette(std::uint64_t word, std::span<const std::uint8_t> rdram);
    void draw_textured_rectangle(std::span<std::uint8_t> rdram) const;
    void copy_rectangle(std::span<std::uint8_t> rdram) const;

    struct Tile {
        std::uint64_t descriptor = 0, bounds = 0;

        template <class Ar>
        void serialize(Ar& ar) { ar(descriptor), ar(bounds); }
    };

    [[nodiscard]] bool can_sample_texture(const Tile& tile, bool allow_lod = false) const;
    [[nodiscard]] Rgba sample_texel(const Tile& tile, int s, int t) const;
    [[nodiscard]] std::uint16_t read_texel(const Tile& tile, unsigned s, unsigned t, unsigned bank) const;
    [[nodiscard]] Rgba sample_texture_rgba(const Tile& tile, int s, int t) const;

    struct TextureState {
        std::uint64_t image = 0;
        std::array<Tile, 8> tiles{};
        std::array<std::uint16_t, 2048> tmem{};  // 4 KiB, in physical halfword order

        template <class Ar>
        void serialize(Ar& ar) { ar(image), ar(tiles), ar(tmem); }
    };

    struct SolidState {
        std::uint64_t modes = 0, combine = 0;
        std::uint32_t blend = 0, fog = 0, primitive = 0, environment = 0;
        std::uint32_t format = 0;

        template <class Ar>
        void serialize(Ar& ar) {
            ar(modes), ar(combine), ar(blend), ar(fog), ar(primitive), ar(environment), ar(format);
        }
    };

    std::uint32_t color_address_ = 0, color_width_ = 0, pixel_size_ = 0;
    std::uint32_t fill_color_ = 0, cycle_type_ = 0;
    // Two 10.2 fixed-point coordinate pairs and the field selection bits.
    std::uint64_t scissor_ = 0;
    std::array<std::uint64_t, 22> raster_command_{};  // up to edges, shade, texture and depth
    std::uint32_t mask_address_ = 0, prim_depth_ = 0;  // Set Mask Image and Set Prim Depth
    std::uint64_t key_gb_ = 0;  // Set Key GB
    std::uint32_t key_r_ = 0;   // Set Key R
    std::uint64_t convert_ = 0;   // Set Convert: K0..K5, 9 bits each
    std::uint32_t prim_lod_ = 0;  // Set Primitive Color: min level and LOD fraction
    mutable std::uint32_t noise_state_ = 1;
    // RDRAM hidden bits (two per halfword), kept sparse: only nonzero entries, keyed by
    // halfword index. Only depth writes set them; fills clear them.
    mutable std::map<std::uint32_t, std::uint8_t> hidden_;  // combiner noise generator, advanced per pixel
    SolidState solid_;
    TextureState texture_;
};

} // namespace ezy::n64
