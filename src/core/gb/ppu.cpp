#include "gb/ppu.hpp"


#include "gb/interrupts.hpp"

namespace ezy::gb {

namespace {

constexpr int kOamScanDots = 80;

constexpr std::uint8_t apply_palette(std::uint8_t palette, std::uint8_t color_index) {
    return static_cast<std::uint8_t>((palette >> (color_index * 2)) & 0x03);
}

}  // namespace

void Ppu::tick(int dots) {
    if (!lcd_on()) return;
    for (int i = 0; i < dots; ++i) step_dot();
}

void Ppu::step_dot() {
    ++dot_;
    if (dot_ == kDotsPerLine) {
        dot_ = 0;
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
        new_mode = 2;
    } else if (mode_ == 2) {
        new_mode = 3;
        begin_mode3();
    } else if (mode_ == 3 && draw_dot()) {
        new_mode = 0;
        hblank_started_ = true;
        if (window_drawn_) ++window_line_;
    }
    if (new_mode != mode_) {
        mode_ = new_mode;
        update_stat_line();
    } else if (dot_ == 0) {
        update_stat_line();  // LY changed: LY=LYC may have changed
    }
}

void Ppu::update_stat_line() {
    const bool line = ((stat_ & 0x40) && ly_ == lyc_) || ((stat_ & 0x08) && mode_ == 0) ||
                      ((stat_ & 0x10) && mode_ == 1) || ((stat_ & 0x20) && mode_ == 2);
    if (line && !stat_line_) interrupts_ |= interrupt::kStat;
    stat_line_ = line;
}

std::uint8_t Ppu::read_register(std::uint16_t addr) const {
    switch (addr) {
        case 0xFF40: return lcdc_;
        case 0xFF41: {
            const int mode = lcd_on() ? mode_ : 0;
            const int coincidence = ly_ == lyc_ ? 0x04 : 0;
            return static_cast<std::uint8_t>(0x80 | stat_ | coincidence | mode);
        }
        case 0xFF42: return scy_;
        case 0xFF43: return scx_;
        case 0xFF44: return ly_;
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
                dot_ = 0;
                mode_ = 0;
                window_y_hit_ = false;
                window_line_ = 0;
                frame_.fill(blank_color());
            } else if (!was_on && lcd_on()) {
                mode_ = 2;
            }
            break;
        }
        case 0xFF41: stat_ = value & 0x78; break;
        case 0xFF42: scy_ = value; break;
        case 0xFF43: scx_ = value; break;
        case 0xFF44: break;  // LY is read-only
        case 0xFF45: lyc_ = value; break;
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
