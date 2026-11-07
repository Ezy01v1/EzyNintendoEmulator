#include "gb/ppu.hpp"

#include <algorithm>

#include "gb/interrupts.hpp"

namespace ezy::gb {

namespace {

constexpr int kOamScanDots = 80;

constexpr std::uint8_t apply_palette(std::uint8_t palette, std::uint8_t color_index) {
    return static_cast<std::uint8_t>((palette >> (color_index * 2)) & 0x03);
}

}  // namespace

// The LY register runs a few dots ahead of the line itself: it shows the next line number
// 3 dots before the line ends (the same 3-dot offset as the mode 3 calibration in
// ppu_draw.cpp). Line 153 is special: LY reads 153 for 4 dots, then 0 for the rest of it.
constexpr int kLyEarlyDots = kLineOriginOffsetDots;
constexpr int kFirstLineStartDot = 0;   // line 0 after LCD on starts this far in
constexpr int kFirstLineDrawDelay = 0;   // ...and its mode 3 starts this much later
// On a CGB the mode 2 STAT source of line 144 rises one M-cycle before VBlank does
// (Mooneye misc/ppu/vblank_stat_intr-C); on a DMG both come together.
constexpr int kCgbVblankOamLeadDots = 4;

// Outside mode 3 a dot only matters at a few places in the line: dot 1 (line 153's LY
// change, and the end of line 144's dot-0 mode 2 STAT source), the start of mode 3, the
// CGB's early mode 2 STAT source before line 144, the early LY change and the end of the
// line. Every other dot of modes 0, 1 and 2 leaves the state as it was, so tick() skips
// straight over them.
int Ppu::next_event_dot() const {
    constexpr int kEvents[] = {4 - kLyEarlyDots, kOamScanDots + kFirstLineDrawDelay,
                               kDotsPerLine - kCgbVblankOamLeadDots, kDotsPerLine - kLyEarlyDots, kDotsPerLine};
    for (const int event : kEvents) {
        if (event > dot_) return event;
    }
    return kDotsPerLine;
}

void Ppu::tick(int dots) {
    if (!lcd_on()) return;
    while (dots > 0) {
        if (mode_ != 3) {
            const int idle = std::min(dots, next_event_dot() - dot_ - 1);
            if (idle > 0) {
                dot_ += idle;
                dots -= idle;
                if (dots == 0) break;
            }
        }
        step_dot();
        --dots;
    }
}

void Ppu::step_dot() {
    ++dot_;
    if (dot_ == kDotsPerLine - kLyEarlyDots && ly_ != kLinesPerFrame - 1) ly_reg_ = static_cast<std::uint8_t>(ly_ + 1);
    if (ly_ == kLinesPerFrame - 1 && dot_ == 4 - kLyEarlyDots) ly_reg_ = 0;
    if (dot_ == kDotsPerLine) {
        dot_ = 0;
        first_line_ = false;
        ++ly_;
        if (ly_ == kLcdHeight) {
            interrupts_ |= interrupt::kVBlank;
            frame_ready_ = true;
        } else if (ly_ == kLinesPerFrame) {
            ly_ = 0;
            window_y_hit_ = false;
            window_line_ = 0;
        }
    }

    int new_mode = mode_;
    if (ly_ >= kLcdHeight) {
        new_mode = 1;
    } else if (dot_ < kOamScanDots) {
        new_mode = first_line_ ? 0 : 2;  // right after LCD on there's no OAM scan
    } else if (first_line_ && mode_ == 0 && dot_ < kOamScanDots + kFirstLineDrawDelay) {
        new_mode = 0;
    } else if (mode_ == 2 || (first_line_ && dot_ == kOamScanDots + kFirstLineDrawDelay)) {
        new_mode = 3;
        begin_mode3();
    } else if (mode_ == 3 && draw_dot()) {
        new_mode = 0;
        hblank_started_ = true;
        if (window_drawn_) ++window_line_;
    }
    mode_ = new_mode;
    lyc_flag_ = ly_reg_ == lyc_;
    update_stat_line();
}

// The STAT interrupt line is the OR of the enabled sources; only its rising edge requests
// an interrupt. On the first dot of line 144 the mode 2 source fires too (the hardware
// starts an OAM scan there before switching to VBlank). A CGB raises it one M-cycle
// earlier, at the end of line 143.
void Ppu::update_stat_line() {
    const bool cgb_early = model_ == Model::Cgb && ly_ == kLcdHeight - 1 && dot_ >= kDotsPerLine - kCgbVblankOamLeadDots;
    const bool vblank_oam = (ly_ == kLcdHeight && dot_ == 0) || cgb_early;
    const bool line = ((stat_ & 0x40) && lyc_flag_) || ((stat_ & 0x08) && mode_ == 0) ||
                      ((stat_ & 0x10) && mode_ == 1) || ((stat_ & 0x20) && (mode_ == 2 || vblank_oam));
    if (line && !stat_line_) interrupts_ |= interrupt::kStat;
    stat_line_ = line;
}

std::uint8_t Ppu::read_register(std::uint16_t addr) const {
    switch (addr) {
        case 0xFF40: return lcdc_;
        case 0xFF41: {
            const int mode = lcd_on() ? mode_ : 0;
            const int coincidence = lyc_flag_ ? 0x04 : 0;  // kept while the LCD is off
            return static_cast<std::uint8_t>(0x80 | stat_ | coincidence | mode);
        }
        case 0xFF42: return scy_;
        case 0xFF43: return scx_;
        case 0xFF44: return ly_reg_;
        case 0xFF45: return lyc_;
        case 0xFF47: return bgp_;
        case 0xFF48: return obp0_;
        case 0xFF49: return obp1_;
        case 0xFF4A: return wy_;
        case 0xFF4B: return wx_;
        case 0xFF68: return static_cast<std::uint8_t>(0x40 | bcps_);
        case 0xFF6A: return static_cast<std::uint8_t>(0x40 | ocps_);
        // Palette data can't be reached while the PPU is drawing (mode 3).
        case 0xFF69: return mode_ == 3 && lcd_on() ? 0xFF : bg_palettes_[bcps_ & 0x3F];
        case 0xFF6B: return mode_ == 3 && lcd_on() ? 0xFF : obj_palettes_[ocps_ & 0x3F];
        default: return 0xFF;
    }
}

void Ppu::write_register(std::uint16_t addr, std::uint8_t value) {
    switch (addr) {
        case 0xFF40: {
            const bool was_on = lcd_on();
            lcdc_ = value;
            if (was_on && !lcd_on()) {  // turning the LCD off resets the line state
                ly_ = 0;
                ly_reg_ = 0;
                dot_ = 0;
                mode_ = 0;
                window_y_hit_ = false;
                window_line_ = 0;
                frame_.fill(blank_color());
            } else if (!was_on && lcd_on()) {
                // Line 0 starts over, in mode 0 (no OAM scan), and LY=LYC is checked at once.
                // On hardware this line is also shorter and draws a little late (Mooneye
                // lcdon_timing); those offsets aren't modelled yet (the two knobs below stay
                // at 0), see the known failures in roms/README.md.
                dot_ = kFirstLineStartDot;
                mode_ = 0;
                first_line_ = true;
                lyc_flag_ = ly_reg_ == lyc_;
            }
            break;
        }
        case 0xFF41: stat_ = value & 0x78; break;
        case 0xFF42: scy_ = value; break;
        case 0xFF43: scx_ = value; break;
        case 0xFF44: break;  // LY is read-only
        case 0xFF45:
            lyc_ = value;
            if (lcd_on()) lyc_flag_ = ly_reg_ == lyc_;  // the comparison only runs while on
            break;
        case 0xFF47: bgp_ = value; break;
        case 0xFF48: obp0_ = value; break;
        case 0xFF49: obp1_ = value; break;
        case 0xFF4A: wy_ = value; break;
        case 0xFF4B: wx_ = value; break;
        case 0xFF68: bcps_ = value & 0xBF; break;
        case 0xFF6A: ocps_ = value & 0xBF; break;
        case 0xFF69: write_palette_data(bg_palettes_, bcps_, value); break;
        case 0xFF6B: write_palette_data(obj_palettes_, ocps_, value); break;
        default: break;
    }
    if (lcd_on()) update_stat_line();
}

void Ppu::set_model(Model model, bool cgb_mode) {
    model_ = model;
    cgb_mode_ = cgb_mode;
    frame_.fill(blank_color());
    // Until a compatibility palette is chosen, show a DMG game in greys.
    if (model == Model::Cgb && !cgb_mode) set_compat_palettes(kDmgGreys, kDmgGreys, kDmgGreys);
}

void Ppu::set_compat_palettes(const std::array<std::uint16_t, 4>& bg, const std::array<std::uint16_t, 4>& obj0,
                              const std::array<std::uint16_t, 4>& obj1) {
    const auto store = [](std::array<std::uint8_t, 64>& ram, int palette, const std::array<std::uint16_t, 4>& colors) {
        for (std::size_t i = 0; i < 4; ++i) {
            const auto at = static_cast<std::size_t>(palette) * 8 + i * 2;
            ram[at] = static_cast<std::uint8_t>(colors[i] & 0xFF);
            ram[at + 1] = static_cast<std::uint8_t>(colors[i] >> 8);
        }
    };
    store(bg_palettes_, 0, bg);
    store(obj_palettes_, 0, obj0);
    store(obj_palettes_, 1, obj1);
}

// BCPD/OCPD write. During mode 3 the write is lost, but auto-increment still happens.
void Ppu::write_palette_data(std::array<std::uint8_t, 64>& ram, std::uint8_t& spec, std::uint8_t value) {
    if (!(mode_ == 3 && lcd_on())) ram[spec & 0x3F] = value;
    if (spec & 0x80) spec = static_cast<std::uint8_t>(0x80 | ((spec + 1) & 0x3F));
}

std::uint8_t Ppu::take_interrupts() {
    const std::uint8_t bits = interrupts_;
    interrupts_ = 0;
    return bits;
}

bool Ppu::take_hblank_started() {
    const bool started = hblank_started_;
    hblank_started_ = false;
    return started;
}

bool Ppu::take_frame_ready() {
    const bool ready = frame_ready_;
    frame_ready_ = false;
    return ready;
}

// The final colour of a BG/window pixel. CGB mode: straight from CGB palette `palette`.
// DMG and compatibility mode: through BGP to a shade first.
std::uint16_t Ppu::bg_color(int palette, std::uint8_t index) const {
    if (cgb_mode_) return palette_color(bg_palettes_, palette, index);
    const std::uint8_t shade = apply_palette(bgp_, index);
    if (model_ == Model::Dmg) return dmg_colors_[shade];
    return palette_color(bg_palettes_, 0, shade);
}

std::uint16_t Ppu::obj_color(std::uint8_t attributes, std::uint8_t index) const {
    if (cgb_mode_) return palette_color(obj_palettes_, attributes & 0x07, index);
    const bool obp1 = (attributes & 0x10) != 0;
    const std::uint8_t shade = apply_palette(obp1 ? obp1_ : obp0_, index);
    if (model_ == Model::Dmg) return dmg_colors_[shade];
    return palette_color(obj_palettes_, obp1 ? 1 : 0, shade);
}

}  // namespace ezy::gb
