#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "n64/mi.hpp"
#include "n64/rdp_worker.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Display Processor command interface and software rasterizer.
class Rdp {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, std::uint64_t now) const;
    void write(std::uint32_t offset, std::uint32_t value, Scheduler& scheduler, std::uint64_t when);
    void step(Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
              std::span<const std::uint8_t, 0x1000> dmem, std::uint64_t when);

    // Drawing on a background thread (off by default: everything then happens at its exact
    // cycle). The emulation waits for it at Sync Full, and before a save state or a read of
    // the picture; in between, the CPU and RSP may see RDRAM before the drawing reaches it.
    void set_threaded(bool threaded);
    // Waits until all submitted drawing is in RDRAM.
    void sync();
    // Command words that skip DPC (the graphics HLE): same framing and Sync Full handling as
    // words fetched from RDRAM or DMEM, without touching START/END/CURRENT.
    void submit_words(std::span<const std::uint64_t> words, Mi& mi, std::span<std::uint8_t> rdram);
    // When set, every command word the RDP consumes is appended here (for comparing runs).
    void set_trace(std::vector<std::uint64_t>* trace) { trace_ = trace; }
    // How many 64-bit words the command with this opcode (bits 61:56) takes.
    [[nodiscard]] static unsigned words_in_command(unsigned op);

    Rdp() = default;
    Rdp(Rdp&&) = default;
    Rdp& operator=(Rdp&&) = default;
    ~Rdp() { worker_.reset(); }  // stop the thread before the state it draws with goes away

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        sync();
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
        if (version >= 11) {
            // Saved sparse: the count of nonzero halfwords, then (index, bits) in index order.
            auto count = static_cast<std::uint32_t>(
                std::count_if(hidden_.begin(), hidden_.end(), [](std::uint8_t bits) { return bits != 0; }));
            ar(count);
            if constexpr (Ar::kLoading) {
                std::fill(hidden_.begin(), hidden_.end(), std::uint8_t{0});
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t halfword = 0;
                    std::uint8_t bits = 0;
                    ar(halfword), ar(bits);
                    if (bits > 3 || halfword >= hidden_.size()) ar.fail();
                    else hidden_[halfword] = bits;
                }
            } else {
                for (std::uint32_t halfword = 0; halfword < hidden_.size(); ++halfword) {
                    std::uint8_t bits = hidden_[halfword];
                    if (bits != 0) ar(halfword), ar(bits);
                }
            }
        } else {
            std::fill(hidden_.begin(), hidden_.end(), std::uint8_t{0});
        }
    }

    template <class Ar>
    void load_legacy(Ar& ar) {
        std::array<std::uint32_t, 8> registers{};
        ar(registers);
        const bool threaded = worker_ != nullptr;
        set_threaded(false);
        *this = {};
        set_threaded(threaded);
        start_ = registers[0] & 0xFFFFF8;
        end_ = registers[1] & 0xFFFFF8;
        current_ = registers[2] & 0xFFFFF8;
    }

private:
    static constexpr std::uint64_t kTexturePerspective = 1ULL << 51;
    static unsigned buffered_draw_words(unsigned op);
    void update_counters(std::uint64_t when);
    void schedule_word(Scheduler& scheduler, std::uint64_t when);
    struct RenderJob {
        bool draw;           // a buffered draw command, else a one-word command
        std::uint64_t word;  // the one-word command
        std::array<std::uint64_t, 22> command;
        std::span<std::uint8_t> rdram;
    };
    void render(const RenderJob& job);
    void submit(const RenderJob& job);
    void consume_word(std::uint64_t word, Mi& mi, std::span<std::uint8_t> rdram);
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
    // RDRAM hidden bits (two per halfword of the 8 MB), one byte per halfword. Only depth
    // writes set them; fills clear them.
    mutable std::vector<std::uint8_t> hidden_ = std::vector<std::uint8_t>(4 * 1024 * 1024, 0);
    SolidState solid_;
    TextureState texture_;
    // The draw command being rendered (draw_command_ is the one being fetched).
    std::array<std::uint64_t, 22> render_command_{};
    std::unique_ptr<RdpWorker<RenderJob>> worker_;
    std::vector<std::uint64_t>* trace_ = nullptr;  // not saved: a debugging aid
};

}  // namespace ezy::n64
