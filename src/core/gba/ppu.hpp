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
// The PPU doesn't own memory: it works on views of the bus's I/O registers, palette and
// VRAM. It keeps DISPSTAT's read-only bits and VCOUNT up to date in the I/O storage and
// raises interrupts by setting bits in IF there.
//
// Implemented: bitmap modes 3, 4 and 5, forced blank, backdrop. Tile modes 0-2, sprites,
// windows and blending come next; for now those modes show the backdrop.
class Ppu {
public:
    Ppu(std::span<std::uint8_t> io, std::span<const std::uint8_t> palette, std::span<const std::uint8_t> vram,
        Scheduler* scheduler);

    void start();  // schedule the first HDraw end (line 0 starts now)
    void on_hdraw_end(std::uint64_t when);
    void on_hblank_end(std::uint64_t when);

    // The bus calls this after any I/O write that touched DISPSTAT/VCOUNT, to put the
    // read-only bits back (and re-check the VCount match against a new LYC).
    void after_io_write();

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

    std::span<std::uint8_t> io_;
    std::span<const std::uint8_t> palette_;
    std::span<const std::uint8_t> vram_;
    Scheduler* scheduler_;

    int vcount_ = 0;
    bool vblank_ = false;
    bool hblank_ = false;
    bool vcount_match_ = false;
    bool frame_done_ = false;
    std::array<std::uint16_t, kScreenWidth * kScreenHeight> framebuffer_{};
};

}  // namespace ezy::gba
