#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gb/model.hpp"

namespace ezy::gb {

inline constexpr int kLcdWidth = 160;
inline constexpr int kLcdHeight = 144;
inline constexpr int kDotsPerLine = 456;
inline constexpr int kLinesPerFrame = 154;
// RGB555 (bits 0-4 red, 5-9 green, 10-14 blue), the CGB's native colour format.
// The DMG model shows its 4 shades as these plain greys, lightest to darkest.
inline constexpr std::array<std::uint16_t, 4> kDmgGreys = {0x7FFF, 0x56B5, 0x294A, 0x0000};

inline constexpr int kDotsPerFrame = kDotsPerLine * kLinesPerFrame;  // 70224 T-cycles

// Picture Processing Unit (DMG).
//
// Timing per visible line: mode 2 (OAM scan, 80 dots) -> mode 3 (drawing, 172 dots plus
// penalties) -> mode 0 (HBlank, rest of the 456). Lines 144-153 are mode 1 (VBlank).
// The mode 3 length is estimated when it starts (Pan Docs, "Mode 3 length"): +SCX % 8 for
// the discarded pixels, +6 if the window shows on the line, and 6 to 11 per sprite.
//
// Rendering model: a *scanline* renderer. The whole line is drawn when mode 3 ends, using
// the register values at that moment. Real hardware draws pixel by pixel through a FIFO,
// so mid-line register changes are not reproduced. That is enough for dmg-acid2 and nearly
// all games; a FIFO renderer can replace render_scanline() later without touching the rest.
//
// The CPU can't reach OAM in modes 2-3 or VRAM in mode 3 (the bus checks *_accessible()).
class Ppu {
public:
    void tick(int dots = 4);  // 4 dots per CPU M-cycle at normal speed, 2 in double speed

    // CPU-side VRAM access goes through the bank selected by VBK (always 0 on a DMG).
    [[nodiscard]] std::uint8_t read_vram(std::uint16_t addr) const { return vram_[vram_index(vram_bank_, addr)]; }
    void write_vram(std::uint16_t addr, std::uint8_t value) { vram_[vram_index(vram_bank_, addr)] = value; }
    [[nodiscard]] std::uint8_t read_vram_bank(int bank, std::uint16_t addr) const { return vram_[vram_index(bank, addr)]; }
    void write_vram_bank(int bank, std::uint16_t addr, std::uint8_t value) { vram_[vram_index(bank, addr)] = value; }
    [[nodiscard]] int vram_bank() const { return vram_bank_; }
    void set_vram_bank(int bank) { vram_bank_ = bank & 1; }
    // OPRI bit 0: 0 = sprites ordered by OAM index (CGB), 1 = by X coordinate (DMG).
    [[nodiscard]] std::uint8_t opri() const { return opri_; }
    void set_opri(std::uint8_t value) { opri_ = value & 1; }
    [[nodiscard]] std::uint8_t read_oam(std::uint16_t addr) const { return oam_[addr - 0xFE00]; }
    void write_oam(std::uint16_t addr, std::uint8_t value) { oam_[addr - 0xFE00] = value; }

    // 0xFF40-0xFF4B except 0xFF46 (DMA, handled by the bus).
    [[nodiscard]] std::uint8_t read_register(std::uint16_t addr) const;
    void write_register(std::uint16_t addr, std::uint8_t value);

    // Interrupt bits (VBlank / STAT) raised since the last call.
    std::uint8_t take_interrupts();
    // True once each time line 144 is reached (a complete frame is in the framebuffer).
    bool take_frame_ready();
    // True once each time a visible line enters HBlank (when HDMA copies a block).
    bool take_hblank_started();

    // Set once by the bus at power-on. cgb_mode = a CGB game on a CGB.
    void set_model(Model model, bool cgb_mode);
    // A real boot ROM switching to DMG compatibility mode: keeps the palettes it wrote.
    void set_cgb_mode(bool cgb_mode) { cgb_mode_ = cgb_mode; }
    // The colours the DMG model uses for its 4 shades (kDmgGreys by default).
    void set_dmg_colors(const std::array<std::uint16_t, 4>& colors) { dmg_colors_ = colors; }
    // DMG compatibility mode: the palettes the CGB boot ROM picked for a DMG game. BGP/OBP0/OBP1
    // shades index into them (BG palette 0, OBJ palettes 0 and 1).
    void set_compat_palettes(const std::array<std::uint16_t, 4>& bg, const std::array<std::uint16_t, 4>& obj0,
                             const std::array<std::uint16_t, 4>& obj1);

    // One RGB555 colour per pixel.
    [[nodiscard]] std::span<const std::uint16_t> framebuffer() const { return frame_; }
    // CGB palette memory: 8 palettes x 4 colours x 2 bytes (little-endian RGB555).
    [[nodiscard]] std::span<const std::uint8_t, 64> bg_palette_ram() const { return bg_palettes_; }
    [[nodiscard]] std::span<const std::uint8_t, 64> obj_palette_ram() const { return obj_palettes_; }

    [[nodiscard]] std::uint8_t ly() const { return ly_; }
    [[nodiscard]] int mode() const { return mode_; }
    [[nodiscard]] std::uint8_t lcdc() const { return lcdc_; }
    [[nodiscard]] bool lcd_on() const { return (lcdc_ & 0x80) != 0; }
    [[nodiscard]] bool vram_accessible() const { return !lcd_on() || mode_ != 3; }
    [[nodiscard]] bool oam_accessible() const { return !lcd_on() || (mode_ != 2 && mode_ != 3); }

private:
    void render_scanline();
    void write_palette_data(std::array<std::uint8_t, 64>& ram, std::uint8_t& spec, std::uint8_t value);
    [[nodiscard]] std::uint16_t bg_color(int palette, std::uint8_t index) const;
    [[nodiscard]] std::uint16_t obj_color(std::uint8_t attributes, std::uint8_t index) const;
    void update_stat_line();
    [[nodiscard]] int mode3_length() const;
    struct BgPixel {
        std::uint8_t index;       // colour index 0-3, before palettes
        std::uint8_t attributes;  // CGB map attributes (0 outside CGB mode)
    };
    [[nodiscard]] BgPixel bg_pixel(std::uint16_t map_base, int x, int y) const;
    [[nodiscard]] std::uint8_t tile_color_index(int bank, std::uint16_t tile_addr, int row, int col) const;

    [[nodiscard]] static std::size_t vram_index(int bank, std::uint16_t addr) {
        return static_cast<std::size_t>(bank) * 0x2000 + (addr - 0x8000);
    }

    std::array<std::uint8_t, 0x4000> vram_{};  // 2 banks of 8 KiB (bank 1 only used on CGB)
    int vram_bank_ = 0;
    std::array<std::uint8_t, 0xA0> oam_{};
    [[nodiscard]] static std::uint16_t palette_color(const std::array<std::uint8_t, 64>& ram, int palette, int index) {
        const auto i = static_cast<std::size_t>(palette * 8 + index * 2);
        return static_cast<std::uint16_t>((ram[i] | (ram[i + 1] << 8)) & 0x7FFF);
    }
    [[nodiscard]] std::uint16_t blank_color() const { return model_ == Model::Dmg ? dmg_colors_[0] : 0x7FFF; }

    std::array<std::uint16_t, kLcdWidth * kLcdHeight> frame_{};
    Model model_ = Model::Dmg;
    bool cgb_mode_ = false;
    std::uint8_t opri_ = 1;
    std::array<std::uint16_t, 4> dmg_colors_ = kDmgGreys;
    std::array<std::uint8_t, 64> bg_palettes_{};
    std::array<std::uint8_t, 64> obj_palettes_{};
    std::uint8_t bcps_ = 0, ocps_ = 0;  // bit 7 auto-increment, bits 0-5 index

    // Registers (post-boot values).
    std::uint8_t lcdc_ = 0x91;
    std::uint8_t stat_ = 0x00;  // only the interrupt-enable bits 3-6 are stored
    std::uint8_t scy_ = 0, scx_ = 0;
    std::uint8_t ly_ = 0, lyc_ = 0;
    std::uint8_t bgp_ = 0xFC, obp0_ = 0xFF, obp1_ = 0xFF;
    std::uint8_t wy_ = 0, wx_ = 0;

    int dot_ = 0;
    int hblank_start_ = 80 + 172;  // dot where this line's mode 3 ends
    int mode_ = 2;
    bool stat_line_ = false;     // STAT interrupt fires on the rising edge of this
    bool window_y_hit_ = false;  // WY matched LY at some point this frame
    int window_line_ = 0;        // the window has its own line counter
    std::uint8_t interrupts_ = 0;
    bool frame_ready_ = false;
    bool hblank_started_ = false;
};

}  // namespace ezy::gb
