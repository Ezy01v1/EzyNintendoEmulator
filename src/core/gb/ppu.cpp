#include "gb/ppu.hpp"

#include <algorithm>

#include "gb/interrupts.hpp"

namespace ezy::gb {

namespace {

constexpr int kOamScanDots = 80;
constexpr int kDrawingDots = 172;  // minimum mode 3 length

constexpr std::uint8_t kLcdcBgEnable = 0x01;
constexpr std::uint8_t kLcdcObjEnable = 0x02;
constexpr std::uint8_t kLcdcObjTall = 0x04;
constexpr std::uint8_t kLcdcBgMap = 0x08;
constexpr std::uint8_t kLcdcTileData = 0x10;
constexpr std::uint8_t kLcdcWindowEnable = 0x20;
constexpr std::uint8_t kLcdcWindowMap = 0x40;
constexpr std::uint8_t kLcdcOn = 0x80;

constexpr std::uint8_t apply_palette(std::uint8_t palette, std::uint8_t color_index) {
    return static_cast<std::uint8_t>((palette >> (color_index * 2)) & 0x03);
}

struct Sprite {
    int y;  // screen y of the top row (OAM y - 16)
    int x;  // screen x of the left column (OAM x - 8)
    std::uint8_t tile;
    std::uint8_t attributes;
};

}  // namespace

void Ppu::tick() {
    if (!lcd_on()) return;

    dot_ += 4;
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

    int new_mode = 0;
    if (ly_ >= kLcdHeight) {
        new_mode = 1;
    } else if (dot_ < kOamScanDots) {
        new_mode = 2;
    } else {
        if (mode_ == 2) hblank_start_ = kOamScanDots + mode3_length();
        if (dot_ < hblank_start_) new_mode = 3;
    }
    if (new_mode == 0 && mode_ == 3) {
        render_scanline();
        hblank_started_ = true;
    }
    mode_ = new_mode;
    update_stat_line();
}

int Ppu::mode3_length() const {
    int length = kDrawingDots + scx_ % 8;
    const bool window = (lcdc_ & kLcdcWindowEnable) && (window_y_hit_ || ly_ == wy_) && wx_ <= 166;
    if (window) length += 6;
    if (lcdc_ & kLcdcObjEnable) {
        const int height = (lcdc_ & kLcdcObjTall) ? 16 : 8;
        int count = 0;
        for (std::size_t i = 0; i < 40 && count < 10; ++i) {
            const int y = oam_[i * 4] - 16;
            if (ly_ < y || ly_ >= y + height) continue;
            ++count;
            // 6 dots to fetch the sprite, plus up to 5 more waiting for the BG fetcher,
            // depending on where the sprite falls within a background tile.
            const int offset = (oam_[i * 4 + 1] + scx_) % 8;
            length += 6 + std::max(0, 5 - offset);
        }
    }
    return length;
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

// A tile is 16 bytes: 8 rows of 2 bytes (low bit-plane, high bit-plane).
// Bit 7 is the leftmost pixel.
std::uint8_t Ppu::tile_color_index(int bank, std::uint16_t tile_addr, int row, int col) const {
    const std::size_t offset = vram_index(bank, static_cast<std::uint16_t>(tile_addr + row * 2));
    const std::uint8_t lo = vram_[offset];
    const std::uint8_t hi = vram_[offset + 1];
    const int bit = 7 - col;
    return static_cast<std::uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

// Pixel (x, y) of the 256x256 background/window plane stored at `map_base`.
// In CGB mode, VRAM bank 1 holds an attribute byte for each map entry: bits 0-2 palette,
// bit 3 tile bank, bit 5 X-flip, bit 6 Y-flip, bit 7 BG-over-OBJ priority.
Ppu::BgPixel Ppu::bg_pixel(std::uint16_t map_base, int x, int y) const {
    const auto map_addr = static_cast<std::uint16_t>(map_base + (y / 8) * 32 + (x / 8));
    const std::uint8_t tile_index = read_vram_bank(0, map_addr);
    const std::uint8_t attributes = cgb_mode_ ? read_vram_bank(1, map_addr) : 0;
    // LCDC.4 selects unsigned addressing from 0x8000 or signed addressing around 0x9000.
    const std::uint16_t tile_addr =
        (lcdc_ & kLcdcTileData) ? static_cast<std::uint16_t>(0x8000 + tile_index * 16)
                                : static_cast<std::uint16_t>(0x9000 + static_cast<std::int8_t>(tile_index) * 16);
    const int row = (attributes & 0x40) ? 7 - y % 8 : y % 8;
    const int col = (attributes & 0x20) ? 7 - x % 8 : x % 8;
    return {tile_color_index((attributes & 0x08) ? 1 : 0, tile_addr, row, col), attributes};
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

void Ppu::render_scanline() {
    std::uint16_t* line = &frame_[static_cast<std::size_t>(ly_) * kLcdWidth];
    std::array<std::uint8_t, kLcdWidth> bg_index{};       // pre-palette, for sprite priority
    std::array<std::uint8_t, kLcdWidth> bg_attributes{};  // CGB: bit 7 = BG over sprites

    if (ly_ == wy_) window_y_hit_ = true;

    // --- Background and window. On DMG (and in compatibility mode) LCDC.0 turns both off
    // and the line is blank; in CGB mode it only removes their priority over sprites. ---
    if (cgb_mode_ || (lcdc_ & kLcdcBgEnable)) {
        const bool window_visible = (lcdc_ & kLcdcWindowEnable) && window_y_hit_ && wx_ <= 166;
        const std::uint16_t bg_map = (lcdc_ & kLcdcBgMap) ? 0x9C00 : 0x9800;
        const std::uint16_t win_map = (lcdc_ & kLcdcWindowMap) ? 0x9C00 : 0x9800;
        bool window_drawn = false;

        for (int x = 0; x < kLcdWidth; ++x) {
            BgPixel pixel{};
            if (window_visible && x + 7 >= wx_) {
                pixel = bg_pixel(win_map, x + 7 - wx_, window_line_);
                window_drawn = true;
            } else {
                pixel = bg_pixel(bg_map, (x + scx_) & 0xFF, (ly_ + scy_) & 0xFF);
            }
            bg_index[static_cast<std::size_t>(x)] = pixel.index;
            bg_attributes[static_cast<std::size_t>(x)] = pixel.attributes;
            line[x] = bg_color(pixel.attributes & 0x07, pixel.index);
        }
        if (window_drawn) ++window_line_;
    } else {
        std::fill(line, line + kLcdWidth, bg_color(0, 0));
    }

    // --- Sprites ---
    if (!(lcdc_ & kLcdcObjEnable)) return;

    const int height = (lcdc_ & kLcdcObjTall) ? 16 : 8;
    std::array<Sprite, 10> sprites{};
    std::size_t count = 0;
    // OAM scan: the first 10 sprites (in OAM order) that overlap this line.
    for (std::size_t i = 0; i < 40 && count < sprites.size(); ++i) {
        const int y = oam_[i * 4] - 16;
        if (ly_ >= y && ly_ < y + height) {
            sprites[count++] = {y, oam_[i * 4 + 1] - 8, oam_[i * 4 + 2], oam_[i * 4 + 3]};
        }
    }
    // DMG priority (OPRI = 1): smaller X wins; equal X keeps OAM order (hence stable_sort).
    // CGB priority (OPRI = 0): OAM order alone.
    if (opri_ & 1) {
        std::stable_sort(sprites.begin(), sprites.begin() + static_cast<std::ptrdiff_t>(count),
                         [](const Sprite& a, const Sprite& b) { return a.x < b.x; });
    }

    for (int x = 0; x < kLcdWidth; ++x) {
        for (std::size_t s = 0; s < count; ++s) {
            const Sprite& sprite = sprites[s];
            if (x < sprite.x || x >= sprite.x + 8) continue;

            int col = x - sprite.x;
            int row = ly_ - sprite.y;
            if (sprite.attributes & 0x20) col = 7 - col;
            if (sprite.attributes & 0x40) row = height - 1 - row;
            const std::uint8_t tile = height == 16 ? (sprite.tile & 0xFE) : sprite.tile;
            // Row 8-15 of a tall sprite falls naturally into the next tile (+16 bytes).
            const int bank = cgb_mode_ && (sprite.attributes & 0x08) ? 1 : 0;
            const std::uint8_t index =
                tile_color_index(bank, static_cast<std::uint16_t>(0x8000 + tile * 16), row, col);
            if (index == 0) continue;  // transparent: a lower-priority sprite may show

            // The highest-priority opaque sprite decides this pixel, even if it is behind BG.
            // CGB mode: with LCDC.0 clear sprites always win; otherwise either the sprite's or
            // the map entry's priority bit puts a non-zero BG pixel in front.
            const auto px = static_cast<std::size_t>(x);
            const bool bg_opaque = bg_index[px] != 0;
            const bool behind_bg =
                cgb_mode_ ? (lcdc_ & kLcdcBgEnable) && bg_opaque && ((sprite.attributes | bg_attributes[px]) & 0x80)
                          : (sprite.attributes & 0x80) && bg_opaque;
            if (!behind_bg) line[x] = obj_color(sprite.attributes, index);
            break;
        }
    }
}

}  // namespace ezy::gb
