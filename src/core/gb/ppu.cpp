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

// Timing around the end of a line (Mooneye lcdon_timing, hblank_ly_scx_timing-GS). The
// hardware does several things one M-cycle (4 dots) before the STAT mode bits change:
//  - LY shows the next line number 4 dots before the line ends. Line 153 is special: LY
//    reads 153 for those 4 dots only (one M-cycle), and 0 for the whole of line 153.
//  - The LY=LYC comparator only catches up 3 dots before the line ends (the line origin
//    offset of ppu_draw.cpp); in between, STAT bit 2 reads 0.
//  - The PPU takes the memory buses early, see oam_readable().
constexpr int kLyEarlyDots = 4;
constexpr int kLycCompareEarlyDots = kLineOriginOffsetDots;
constexpr int kBusLockLeadDots = 4;
// Line 0 after LCD on: no OAM scan, and with the bus locks above it needs no other offset.
constexpr int kFirstLineDrawDelay = 0;
// On a CGB the mode 2 STAT source of line 144 rises one M-cycle before VBlank does
// (Mooneye misc/ppu/vblank_stat_intr-C); on a DMG both come together.
constexpr int kCgbVblankOamLeadDots = 4;

// Outside mode 3 a dot only matters at a few places in the line: dot 1 (the end of line
// 144's dot-0 mode 2 STAT source), the start of mode 3, dot
// 452 (LY changes; the CGB's early mode 2 STAT source before line 144), dot 453 (the LYC
// comparator catches up) and the end of the line. Every other dot of modes 0, 1 and 2
// leaves the state as it was, so tick() skips straight over them. (The bus locks are
// worked out from dot_ when the CPU asks, so they need no event.)
int Ppu::next_event_dot() const {
    constexpr int kEvents[] = {1, kOamScanDots + kFirstLineDrawDelay, kDotsPerLine - kLyEarlyDots,
                               kDotsPerLine - kCgbVblankOamLeadDots, kDotsPerLine - kLycCompareEarlyDots, kDotsPerLine};
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
    if (dot_ == kDotsPerLine) {
        dot_ = 0;
        first_line_ = false;
        ++ly_;
        if (ly_ == kLcdHeight) {
            interrupts_ |= interrupt::kVBlank;
            frame_ready_ = true;
        } else if (ly_ == kLinesPerFrame - 1) {
            ly_reg_ = 0;
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
    lyc_flag_ = compare_lyc();
    update_stat_line();
}

// LY=LYC. Right after LY changes the comparison reads false until the comparator catches
// up (Mooneye lcdon_timing: STAT bit 2 is clear when LY has just become equal to LYC).
bool Ppu::compare_lyc() const {
    const bool catching_up = ly_ != kLinesPerFrame - 1 && dot_ >= kDotsPerLine - kLyEarlyDots &&
                             dot_ < kDotsPerLine - kLycCompareEarlyDots;
    return !catching_up && ly_reg_ == lyc_;
}

// The PPU takes the memory buses a little before the STAT mode bits change (Mooneye
// lcdon_timing and lcdon_write_timing, both from the line start):
//  - OAM reads are locked from 4 dots before a visible line's OAM scan until mode 3 ends.
//  - OAM writes are locked during the OAM scan and mode 3, except for the 4 dots just
//    before mode 3, when the scan is over.
//  - VRAM reads are locked from 4 dots before mode 3, VRAM writes only in mode 3.
// Line 0 after LCD on has no OAM scan, so none of the early locks.
bool Ppu::oam_readable() const {
    if (!lcd_on()) return true;
    if (mode_ == 2 || mode_ == 3) return false;
    const bool next_line_visible = ly_ < kLcdHeight - 1 || ly_ == kLinesPerFrame - 1;
    return !(next_line_visible && dot_ >= kDotsPerLine - kBusLockLeadDots);
}

bool Ppu::oam_accessible() const {
    if (!lcd_on()) return true;
    return mode_ != 3 && !(mode_ == 2 && dot_ < kOamScanDots - kBusLockLeadDots);
}

bool Ppu::vram_readable() const {
    if (!vram_accessible()) return false;
    if (!lcd_on()) return true;
    return !(mode_ == 2 && dot_ >= kOamScanDots - kBusLockLeadDots);
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
                // Line 0 starts over, in mode 0 (no OAM scan), and LY=LYC is checked at once
                // (Mooneye lcdon_timing and lcdon_write_timing).
                dot_ = 0;
                mode_ = 0;
                first_line_ = true;
                lyc_flag_ = compare_lyc();
            }
            break;
        }
        case 0xFF41: stat_ = value & 0x78; break;
        case 0xFF42: scy_ = value; break;
        case 0xFF43: scx_ = value; break;
        case 0xFF44: break;  // LY is read-only
        case 0xFF45:
            lyc_ = value;
            if (lcd_on()) lyc_flag_ = compare_lyc();  // the comparison only runs while on
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
