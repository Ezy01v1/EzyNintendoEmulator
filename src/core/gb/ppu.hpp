#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ezy::gb {

inline constexpr int kLcdWidth = 160;
inline constexpr int kLcdHeight = 144;
inline constexpr int kDotsPerLine = 456;
inline constexpr int kLinesPerFrame = 154;
inline constexpr int kDotsPerFrame = kDotsPerLine * kLinesPerFrame;  // 70224 T-cycles

// Picture Processing Unit (DMG).
//
// Timing per visible line: mode 2 (OAM scan, 80 dots) -> mode 3 (drawing, fixed 172 dots
// here) -> mode 0 (HBlank, rest of the 456). Lines 144-153 are mode 1 (VBlank).
//
// Rendering model: a *scanline* renderer. The whole line is drawn when mode 3 ends, using
// the register values at that moment. Real hardware draws pixel by pixel through a FIFO,
// so mid-line register changes are not reproduced. That is enough for dmg-acid2 and nearly
// all games; a FIFO renderer can replace render_scanline() later without touching the rest.
//
// Not modelled yet: VRAM/OAM access blocking during modes 2/3, variable mode 3 length.
class Ppu {
public:
    void tick();  // one M-cycle = 4 dots

    [[nodiscard]] std::uint8_t read_vram(std::uint16_t addr) const { return vram_[addr - 0x8000]; }
    void write_vram(std::uint16_t addr, std::uint8_t value) { vram_[addr - 0x8000] = value; }
    [[nodiscard]] std::uint8_t read_oam(std::uint16_t addr) const { return oam_[addr - 0xFE00]; }
    void write_oam(std::uint16_t addr, std::uint8_t value) { oam_[addr - 0xFE00] = value; }

    // 0xFF40-0xFF4B except 0xFF46 (DMA, handled by the bus).
    [[nodiscard]] std::uint8_t read_register(std::uint16_t addr) const;
    void write_register(std::uint16_t addr, std::uint8_t value);

    // Interrupt bits (VBlank / STAT) raised since the last call.
    std::uint8_t take_interrupts();
    // True once each time line 144 is reached (a complete frame is in the framebuffer).
    bool take_frame_ready();

    // One byte per pixel: shade 0 (lightest) .. 3 (darkest), after palettes.
    [[nodiscard]] std::span<const std::uint8_t> framebuffer() const { return frame_; }

    [[nodiscard]] std::uint8_t ly() const { return ly_; }
    [[nodiscard]] int mode() const { return mode_; }
    [[nodiscard]] std::uint8_t lcdc() const { return lcdc_; }
    [[nodiscard]] bool lcd_on() const { return (lcdc_ & 0x80) != 0; }

private:
    void render_scanline();
    void update_stat_line();
    [[nodiscard]] std::uint8_t bg_color_index(std::uint16_t map_base, int x, int y) const;
    [[nodiscard]] std::uint8_t tile_color_index(std::uint16_t tile_addr, int row, int col) const;

    std::array<std::uint8_t, 0x2000> vram_{};
    std::array<std::uint8_t, 0xA0> oam_{};
    std::array<std::uint8_t, kLcdWidth * kLcdHeight> frame_{};

    // Registers (post-boot values).
    std::uint8_t lcdc_ = 0x91;
    std::uint8_t stat_ = 0x00;  // only the interrupt-enable bits 3-6 are stored
    std::uint8_t scy_ = 0, scx_ = 0;
    std::uint8_t ly_ = 0, lyc_ = 0;
    std::uint8_t bgp_ = 0xFC, obp0_ = 0xFF, obp1_ = 0xFF;
    std::uint8_t wy_ = 0, wx_ = 0;

    int dot_ = 0;
    int mode_ = 2;
    bool stat_line_ = false;     // STAT interrupt fires on the rising edge of this
    bool window_y_hit_ = false;  // WY matched LY at some point this frame
    int window_line_ = 0;        // the window has its own line counter
    std::uint8_t interrupts_ = 0;
    bool frame_ready_ = false;
};

}  // namespace ezy::gb
