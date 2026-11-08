#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "n64/mi.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Display Processor command interface and software rasterizer.
class Rdp {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, std::uint64_t now) const;
    void write(std::uint32_t offset, std::uint32_t value, Scheduler& scheduler, std::uint64_t when);
    void step(Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
              std::span<const std::uint8_t, 0x1000> dmem, std::uint64_t when);

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        ar(start_), ar(end_), ar(current_), ar(status_), ar(words_to_skip_);
        ar(clock_origin_), ar(last_update_), ar(command_cycles_), ar(pipe_cycles_);
        ar(color_address_), ar(color_width_), ar(pixel_size_), ar(fill_color_), ar(cycle_type_), ar(scissor_);
        if (version >= 5) {
            ar(draw_words_);
            const std::size_t saved_words = version >= 9 ? draw_command_.size() : version >= 8 ? 12 : 4;
            if constexpr (Ar::kLoading) draw_command_ = {};
            for (std::size_t i = 0; i < saved_words; ++i) ar(draw_command_[i]);
            if constexpr (Ar::kLoading) {
                if (draw_words_ != 0) {
                    const auto op = static_cast<unsigned>(draw_command_[0] >> 56) & 63;
                    if (draw_words_ >= buffered_draw_words(op) ||
                        (version < 8 && op == 0x0A) || (version < 7 && op == 0x24)) ar.fail();
                }
            }
        } else {
            draw_words_ = 0;
            draw_command_ = {};
        }
        if (version >= 6) solid_.serialize(ar);
        else solid_ = {};
        if (version >= 7) texture_.serialize(ar);
        else texture_ = {};
        if (version >= 9) ar(mask_address_), ar(prim_depth_);
        else mask_address_ = prim_depth_ = 0;
        if (version >= 10) ar(key_gb_), ar(key_r_), ar(noise_state_), ar(convert_), ar(prim_lod_);
        else key_gb_ = 0, key_r_ = 0, noise_state_ = 1, convert_ = 0, prim_lod_ = 0;
    }

    template <class Ar>
    void load_legacy(Ar& ar) {
        std::array<std::uint32_t, 8> registers{};
        ar(registers);
        *this = {};
        start_ = registers[0] & 0xFFFFF8;
        end_ = registers[1] & 0xFFFFF8;
        current_ = registers[2] & 0xFFFFF8;
    }

private:
    static constexpr std::uint64_t kTexturePerspective = 1ULL << 51;
    static unsigned buffered_draw_words(unsigned op);
    void update_counters(std::uint64_t when);
    void schedule_word(Scheduler& scheduler, std::uint64_t when);
    void execute(std::uint64_t word, Mi& mi, std::span<std::uint8_t> rdram);
    [[nodiscard]] bool can_fill() const;
    void fill_rectangle(std::uint64_t word, std::span<std::uint8_t> rdram) const;
    void draw_triangle(std::span<std::uint8_t> rdram) const;
    void fill_span(unsigned y, unsigned left, unsigned right, std::span<std::uint8_t> rdram) const;
    [[nodiscard]] bool field_matches(unsigned y) const;
    [[nodiscard]] bool can_write_opaque(bool depth = false) const;
    [[nodiscard]] unsigned memory_coverage(unsigned x, unsigned y, std::span<const std::uint8_t> rdram) const;
    [[nodiscard]] bool depth_test(unsigned x, unsigned y, std::uint32_t z, unsigned dz, unsigned coverage,
                                  std::span<std::uint8_t> rdram) const;
    using Rgba = std::array<unsigned, 4>;
    [[nodiscard]] unsigned texels_read() const;
    unsigned next_noise() const;
    [[nodiscard]] Rgba combine(const Rgba& texel0, const Rgba& texel1, const Rgba& shade) const;
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

    [[nodiscard]] bool can_sample_texture(const Tile& tile) const;
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

    std::uint32_t start_ = 0, end_ = 0, current_ = 0, status_ = 0x80;
    std::uint32_t words_to_skip_ = 0;
    std::uint64_t clock_origin_ = 0, last_update_ = 0;
    std::uint32_t command_cycles_ = 0, pipe_cycles_ = 0;
    std::uint32_t color_address_ = 0, color_width_ = 0, pixel_size_ = 0;
    std::uint32_t fill_color_ = 0, cycle_type_ = 0;
    // Two 10.2 fixed-point coordinate pairs and the field selection bits.
    std::uint64_t scissor_ = 0;
    std::uint32_t draw_words_ = 0;
    std::array<std::uint64_t, 22> draw_command_{};  // up to edges, shade, texture and depth
    std::uint32_t mask_address_ = 0, prim_depth_ = 0;  // Set Mask Image and Set Prim Depth
    std::uint64_t key_gb_ = 0;  // Set Key GB
    std::uint32_t key_r_ = 0;   // Set Key R
    std::uint64_t convert_ = 0;   // Set Convert: K0..K5, 9 bits each
    std::uint32_t prim_lod_ = 0;  // Set Primitive Color: min level and LOD fraction
    mutable std::uint32_t noise_state_ = 1;  // combiner noise generator, advanced per pixel
    SolidState solid_;
    TextureState texture_;
};

}  // namespace ezy::n64
