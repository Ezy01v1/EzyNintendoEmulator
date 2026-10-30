#include "gb/ppu.hpp"

#include <algorithm>

#include "gb/interrupts.hpp"

namespace ezy::gb {

namespace {

constexpr int kOamScanDots = 80;
constexpr int kDrawingDots = 172;
constexpr int kHBlankStart = kOamScanDots + kDrawingDots;  // 252

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
    } else if (dot_ < kHBlankStart) {
        new_mode = 3;
    }
    if (new_mode == 0 && mode_ == 3) render_scanline();
    mode_ = new_mode;
    update_stat_line();
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
                frame_.fill(0);
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
        default: break;
    }
    if (lcd_on()) update_stat_line();
}

std::uint8_t Ppu::take_interrupts() {
    const std::uint8_t bits = interrupts_;
    interrupts_ = 0;
    return bits;
}

bool Ppu::take_frame_ready() {
    const bool ready = frame_ready_;
    frame_ready_ = false;
    return ready;
}

// A tile is 16 bytes: 8 rows of 2 bytes (low bit-plane, high bit-plane).
// Bit 7 is the leftmost pixel.
std::uint8_t Ppu::tile_color_index(std::uint16_t tile_addr, int row, int col) const {
    const std::size_t offset = static_cast<std::size_t>(tile_addr - 0x8000 + row * 2);
    const std::uint8_t lo = vram_[offset];
    const std::uint8_t hi = vram_[offset + 1];
    const int bit = 7 - col;
    return static_cast<std::uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

// Pixel (x, y) of the 256x256 background/window plane stored at `map_base`.
std::uint8_t Ppu::bg_color_index(std::uint16_t map_base, int x, int y) const {
    const std::uint8_t tile_index = vram_[static_cast<std::size_t>(map_base - 0x8000 + (y / 8) * 32 + (x / 8))];
    // LCDC.4 selects unsigned addressing from 0x8000 or signed addressing around 0x9000.
    const std::uint16_t tile_addr =
        (lcdc_ & kLcdcTileData) ? static_cast<std::uint16_t>(0x8000 + tile_index * 16)
                                : static_cast<std::uint16_t>(0x9000 + static_cast<std::int8_t>(tile_index) * 16);
    return tile_color_index(tile_addr, y % 8, x % 8);
}

void Ppu::render_scanline() {
    std::uint8_t* line = &frame_[static_cast<std::size_t>(ly_) * kLcdWidth];
    std::array<std::uint8_t, kLcdWidth> bg_index{};  // pre-palette, for sprite priority

    if (ly_ == wy_) window_y_hit_ = true;

    // --- Background and window (on DMG, LCDC.0 turns both off: the line is blank). ---
    if (lcdc_ & kLcdcBgEnable) {
        const bool window_visible = (lcdc_ & kLcdcWindowEnable) && window_y_hit_ && wx_ <= 166;
        const std::uint16_t bg_map = (lcdc_ & kLcdcBgMap) ? 0x9C00 : 0x9800;
        const std::uint16_t win_map = (lcdc_ & kLcdcWindowMap) ? 0x9C00 : 0x9800;
        bool window_drawn = false;

        for (int x = 0; x < kLcdWidth; ++x) {
            std::uint8_t index = 0;
            if (window_visible && x + 7 >= wx_) {
                index = bg_color_index(win_map, x + 7 - wx_, window_line_);
                window_drawn = true;
            } else {
                index = bg_color_index(bg_map, (x + scx_) & 0xFF, (ly_ + scy_) & 0xFF);
            }
            bg_index[static_cast<std::size_t>(x)] = index;
            line[x] = apply_palette(bgp_, index);
        }
        if (window_drawn) ++window_line_;
    } else {
        std::fill(line, line + kLcdWidth, std::uint8_t{0});
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
    // DMG priority: smaller X wins; equal X keeps OAM order (hence stable_sort).
    std::stable_sort(sprites.begin(), sprites.begin() + static_cast<std::ptrdiff_t>(count),
                     [](const Sprite& a, const Sprite& b) { return a.x < b.x; });

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
            const std::uint8_t index = tile_color_index(static_cast<std::uint16_t>(0x8000 + tile * 16), row, col);
            if (index == 0) continue;  // transparent: a lower-priority sprite may show

            // The highest-priority opaque sprite decides this pixel, even if it is behind BG.
            const bool behind_bg = (sprite.attributes & 0x80) && bg_index[static_cast<std::size_t>(x)] != 0;
            if (!behind_bg) line[x] = apply_palette((sprite.attributes & 0x10) ? obp1_ : obp0_, index);
            break;
        }
    }
}

}  // namespace ezy::gb
