#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gba/scheduler.hpp"

namespace ezy::gba {

inline constexpr int kScreenWidth = 240;
inline constexpr int kScreenHeight = 160;
inline constexpr std::uint32_t kCyclesHDraw = 960;
inline constexpr std::uint32_t kCyclesHBlank = 272;
inline constexpr std::uint32_t kCyclesPerLine = kCyclesHDraw + kCyclesHBlank;  // 1232
inline constexpr int kLinesPerFrame = 228;

// Interrupt request bits (IE/IF, GBATEK "Interrupt Control").
namespace irq {
inline constexpr std::uint16_t kVBlank = 1U << 0;
inline constexpr std::uint16_t kHBlank = 1U << 1;
inline constexpr std::uint16_t kVCount = 1U << 2;
}  // namespace irq

// The picture processing unit: line timing, DISPSTAT/VCOUNT, and rendering.
//
// Timing runs on two scheduler events per line (HDraw end at 960 cycles, HBlank end at
// 1232); the PPU does nothing in between. Each visible line is rendered whole when its
// HDraw ends, so register changes made during HBlank apply from the next line on, as on
// hardware.
//
// The PPU doesn't own memory: it works on views of the bus's I/O registers, palette,
// VRAM and OAM. It keeps DISPSTAT's read-only bits and VCOUNT up to date in the I/O storage and
// raises interrupts by setting bits in IF there.
//
// Each line is drawn into one buffer per layer (BG0-3, OBJ), then composed by priority:
// lower priority value wins, OBJ beats a BG of the same priority, and among BGs the lower
// number wins. Where nothing is opaque, the backdrop (palette entry 0) shows.
//
// Windows (WIN0, WIN1, the OBJ window, outside) pick per pixel which layers show and
// whether color effects apply. Color effects (BLDCNT) then work on the top two visible
// layers: alpha blending of a 1st target over a 2nd target, or brightening/darkening the
// 1st target. Semi-transparent sprites always alpha-blend over a 2nd target below them.
//
// Implemented: tile modes 0-2 (text and affine backgrounds), bitmap modes 3-5 (through
// BG2's affine transform), sprites (regular, affine, semi-transparent, OBJ window; 16/256
// colors, 1D/2D mapping, per-line cycle budget), mosaic, windows, alpha blending,
// brightness, forced blank, backdrop.
// Simplified: a whole line is drawn at once (no mid-line register changes), and the
// sprite that runs out of cycles is dropped whole instead of being cut off.
class Ppu {
public:
    Ppu(std::span<std::uint8_t> io, std::span<const std::uint8_t> palette, std::span<const std::uint8_t> vram,
        std::span<const std::uint8_t> oam, Scheduler* scheduler);

    void start();  // schedule the first HDraw end (line 0 starts now)
    void on_hdraw_end(std::uint64_t when);
    void on_hblank_end(std::uint64_t when);

    // The bus calls this after any I/O write that touched DISPSTAT/VCOUNT, to put the
    // read-only bits back (and re-check the VCount match against a new LYC).
    void after_io_write();

    // The bus calls this after a write to BGxX (`y` false) or BGxY (`y` true), bg = 2 or 3:
    // the internal reference point is reloaded from the register right away. Otherwise it
    // only reloads at VBlank, and each drawn line adds PB/PD to it.
    void reload_affine_reference(int bg, bool y);

    [[nodiscard]] int vcount() const { return vcount_; }
    [[nodiscard]] bool in_vblank() const { return vblank_; }
    [[nodiscard]] bool in_hblank() const { return hblank_; }
    // True once each time line 160 (VBlank) starts: the frame is complete.
    bool take_frame_done();

    // BGR555 pixels (bit 15 clear), row by row.
    [[nodiscard]] std::span<const std::uint16_t> framebuffer() const { return framebuffer_; }

private:
    [[nodiscard]] std::uint16_t io16(std::uint32_t offset) const;
    [[nodiscard]] std::uint16_t palette16(std::uint32_t index) const;
    [[nodiscard]] std::uint16_t vram16(std::uint32_t offset) const;
    void request_irq(std::uint16_t bits);
    void update_status();  // writes DISPSTAT bits 0-2 and VCOUNT into the I/O storage
    void check_vcount_match();
    void render_line(int y);

    // One line of one layer: BGR555, or kTransparent where nothing is drawn.
    using LineBuffer = std::array<std::uint16_t, kScreenWidth>;
    static constexpr std::uint16_t kTransparent = 0x8000;

    void render_text_bg(int bg, int y, LineBuffer& out) const;
    // Affine and bitmap BGs sample from a reference point (19.8 fixed point) stepping by
    // PA/PC per pixel.
    void render_affine_bg(int bg, std::int32_t ref_x, std::int32_t ref_y, LineBuffer& out) const;
    void render_bitmap_bg(int mode, std::int32_t ref_x, std::int32_t ref_y, LineBuffer& out) const;

    // MOSAIC block sizes in pixels (1 = no effect).
    struct Mosaic {
        int bg_h, bg_v, obj_h, obj_v;
    };
    [[nodiscard]] Mosaic mosaic_sizes() const;
    // The OBJ layer of one line. `window` is the OBJ window: where an OBJ-window sprite is
    // opaque (those sprites draw nothing themselves).
    struct ObjLine {
        LineBuffer color{};
        std::array<std::uint8_t, kScreenWidth> priority{};
        std::array<bool, kScreenWidth> semi_transparent{};
        std::array<bool, kScreenWidth> window{};
    };
    void render_sprites(int y, ObjLine& out) const;
    // Which layers (bits 0-3 BG0-3, bit 4 OBJ) and whether effects (bit 5) show at each x.
    void window_masks(int y, const ObjLine& obj, std::array<std::uint8_t, kScreenWidth>& out) const;
    void advance_affine_references();

    std::span<std::uint8_t> io_;
    std::span<const std::uint8_t> palette_;
    std::span<const std::uint8_t> vram_;
    std::span<const std::uint8_t> oam_;
    Scheduler* scheduler_;

    // Internal affine reference points of BG2 and BG3 (signed 19.8 fixed point).
    std::array<std::int32_t, 2> affine_x_{};
    std::array<std::int32_t, 2> affine_y_{};
    // The reference points at the first line of the current vertical mosaic block.
    std::array<std::int32_t, 2> mosaic_ref_x_{};
    std::array<std::int32_t, 2> mosaic_ref_y_{};

    int vcount_ = 0;
    bool vblank_ = false;
    bool hblank_ = false;
    bool vcount_match_ = false;
    bool frame_done_ = false;
    std::array<std::uint16_t, kScreenWidth * kScreenHeight> framebuffer_{};
};

}  // namespace ezy::gba
