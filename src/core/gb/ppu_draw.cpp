// The pixel-FIFO renderer: what the PPU does, dot by dot, during mode 3.
//
// Model (Pan Docs "Pixel FIFO", "Mode 3 length"):
//  - Mode 3 starts with a dummy fetch. Then the fetcher reads a tile in 6 dots (tile
//    number, low byte, high byte; 2 dots each) and pushes its 8 pixels into the BG FIFO
//    on the next dot once the FIFO is empty. One pixel leaves the FIFO per dot.
//    Pan Docs puts the whole of mode 3 at 172 dots. Here the dummy fetch is 3 dots, so the
//    last pixel is out after 3 + 6 + 160 = 169, and mode 0 starts 3 dots later. Sprite
//    fetches absorb those 3 dots (kEndSlackDots), which is what Mooneye's
//    intr_2_mode0_timing_sprites needs, while hblank_ly_scx_timing-GS and lcdon_timing-GS
//    need the full 172 without sprites.
//  - The first SCX % 8 pixels of the line are thrown away (+1 dot each).
//  - When the window starts, the BG FIFO is cleared and the fetcher restarts on the window
//    map, which costs the 6 dots of a fetch.
//  - When the next pixel is where a sprite starts, output stops while the sprite's tile is
//    fetched: 6 dots, plus up to 5 more if it falls early in a background tile (only the
//    first sprite in each tile pays those). Its 8 pixels are merged into the OBJ FIFO.
//  - Registers (BGP, OBP, palettes, LCDC, SCX/SCY for each fetch) are read when used, so
//    mid-line writes show up where they happen.
#include <algorithm>

#include "gb/ppu.hpp"

namespace ezy::gb {

namespace {

constexpr std::uint8_t kLcdcBgEnable = 0x01;
constexpr std::uint8_t kLcdcObjEnable = 0x02;
constexpr std::uint8_t kLcdcObjTall = 0x04;
constexpr std::uint8_t kLcdcBgMap = 0x08;
constexpr std::uint8_t kLcdcTileData = 0x10;
constexpr std::uint8_t kLcdcWindowEnable = 0x20;
constexpr std::uint8_t kLcdcWindowMap = 0x40;

constexpr int kDummyFetchDots = 6 - kLineOriginOffsetDots;  // = 3, see above
constexpr int kFetchDots = 6;
constexpr int kSpriteFetchDots = 6;
// After the last pixel, mode 0 starts this many dots later, less the dots spent fetching
// sprites (they absorb it). With no sprites mode 3 is 172 dots, like Pan Docs says; with
// sprites the sprite timing calibrated against intr_2_mode0_timing_sprites is unchanged.
// Mooneye hblank_ly_scx_timing-GS and lcdon_timing-GS pin the 172.
constexpr int kEndSlackDots = 3;

}  // namespace

// Mode 2: the first 10 sprites (OAM order) that overlap this line. Sprites at X = 0 or
// X >= 168 are invisible but still count toward the 10.
void Ppu::scan_oam() {
    sprite_count_ = 0;
    const int height = (lcdc_ & kLcdcObjTall) ? 16 : 8;
    for (std::size_t i = 0; i < 40 && sprite_count_ < line_sprites_.size(); ++i) {
        const int top = oam_[i * 4] - 16;
        if (ly_ < top || ly_ >= top + height) continue;
        line_sprites_[sprite_count_++] = {oam_[i * 4], oam_[i * 4 + 1], oam_[i * 4 + 2], oam_[i * 4 + 3],
                                          static_cast<std::uint8_t>(i), false};
    }
    // The fetch order is by X (equal X keeps OAM order).
    std::stable_sort(line_sprites_.begin(), line_sprites_.begin() + static_cast<std::ptrdiff_t>(sprite_count_),
                     [](const LineSprite& a, const LineSprite& b) { return a.x < b.x; });
}

void Ppu::begin_mode3() {
    scan_oam();
    if (ly_ == wy_) window_y_hit_ = true;
    bg_fifo_.clear();
    obj_fifo_.clear();
    lx_ = 0;
    discard_ = scx_ % 8;
    startup_dots_ = kDummyFetchDots;
    fetch_step_ = 0;
    fetch_tile_ = 0;
    window_active_ = false;
    window_drawn_ = false;
    sprite_dots_ = 0;
    penalty_tile_ = -1;
    sprite_stall_dots_ = 0;
    end_dots_ = 0;
}

// One fetcher dot. Steps 0-5 read the tile (the reads happen on the odd steps); at step 6
// the 8 pixels are pushed as soon as the BG FIFO is empty.
void Ppu::fetcher_dot() {
    if (fetch_step_ < kFetchDots) {
        if (fetch_step_ == 1) fetch_tile_number();
        if (fetch_step_ == 5) fetch_tile_data();
        ++fetch_step_;
        return;
    }
    if (!bg_fifo_.empty()) return;
    for (int col = 0; col < 8; ++col) {
        const int bit = (fetch_attributes_ & 0x20) ? col : 7 - col;  // X-flip
        const auto color = static_cast<std::uint8_t>((((fetch_hi_ >> bit) & 1) << 1) | ((fetch_lo_ >> bit) & 1));
        bg_fifo_.push({color, static_cast<std::uint8_t>(fetch_attributes_ & 0x07), (fetch_attributes_ & 0x80) != 0});
    }
    fetch_step_ = 0;
    ++fetch_tile_;
}

void Ppu::fetch_tile_number() {
    std::uint16_t map_addr = 0;
    if (window_active_) {
        const std::uint16_t base = (lcdc_ & kLcdcWindowMap) ? 0x9C00 : 0x9800;
        map_addr = static_cast<std::uint16_t>(base + (window_line_ / 8) * 32 + (fetch_tile_ & 31));
    } else {
        const std::uint16_t base = (lcdc_ & kLcdcBgMap) ? 0x9C00 : 0x9800;
        const int column = (scx_ / 8 + fetch_tile_) & 31;
        const int row = ((ly_ + scy_) & 0xFF) / 8;
        map_addr = static_cast<std::uint16_t>(base + row * 32 + column);
    }
    fetch_tile_index_ = read_vram_bank(0, map_addr);
    fetch_attributes_ = cgb_mode_ ? read_vram_bank(1, map_addr) : 0;
}

void Ppu::fetch_tile_data() {
    int row = window_active_ ? window_line_ % 8 : ((ly_ + scy_) & 0xFF) % 8;
    if (fetch_attributes_ & 0x40) row = 7 - row;  // Y-flip
    // LCDC.4: unsigned tile numbers from 0x8000, or signed around 0x9000.
    const std::uint16_t tile_addr =
        (lcdc_ & kLcdcTileData) ? static_cast<std::uint16_t>(0x8000 + fetch_tile_index_ * 16)
                                : static_cast<std::uint16_t>(0x9000 + static_cast<std::int8_t>(fetch_tile_index_) * 16);
    const int bank = (fetch_attributes_ & 0x08) ? 1 : 0;
    const auto addr = static_cast<std::uint16_t>(tile_addr + row * 2);
    fetch_lo_ = read_vram_bank(bank, addr);
    fetch_hi_ = read_vram_bank(bank, static_cast<std::uint16_t>(addr + 1));
}

// The next sprite that starts at the current pixel, if sprites are on.
int Ppu::sprite_at_current_x() const {
    if (!(lcdc_ & kLcdcObjEnable)) return -1;
    for (std::size_t i = 0; i < sprite_count_; ++i) {
        const LineSprite& s = line_sprites_[i];
        if (s.fetched) continue;
        // Sprites partly off the left edge (X < 8) start at pixel 0.
        if (std::max(0, s.x - 8) == lx_ && s.x < 168) return static_cast<int>(i);
    }
    return -1;
}

// Merges a fetched sprite's row into the OBJ FIFO. On DMG (and with OPRI = 1) an opaque
// pixel already there wins, which gives lower X priority. On CGB the lower OAM index wins.
void Ppu::merge_sprite(const LineSprite& sprite) {
    const int height = (lcdc_ & kLcdcObjTall) ? 16 : 8;
    // The row is taken modulo the *current* height: LCDC.2 can change after the OAM scan
    // picked the sprite with the other height (the hardware just uses the low bits).
    int row = (ly_ - (sprite.y - 16)) & (height - 1);
    if (sprite.attributes & 0x40) row = height - 1 - row;
    const std::uint8_t tile = height == 16 ? (sprite.tile & 0xFE) : sprite.tile;
    const int bank = cgb_mode_ && (sprite.attributes & 0x08) ? 1 : 0;
    const auto addr = static_cast<std::uint16_t>(0x8000 + tile * 16 + row * 2);
    const std::uint8_t lo = read_vram_bank(bank, addr);
    const std::uint8_t hi = read_vram_bank(bank, static_cast<std::uint16_t>(addr + 1));

    const int skip = lx_ - (sprite.x - 8);  // columns already off-screen on the left
    for (int col = skip; col < 8; ++col) {
        const int bit = (sprite.attributes & 0x20) ? col : 7 - col;
        const FifoObjPixel pixel{static_cast<std::uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1)),
                                 sprite.attributes, sprite.oam_index};
        const auto slot = static_cast<std::size_t>(col - skip);
        if (slot >= obj_fifo_.size()) {
            obj_fifo_.push(pixel);
            continue;
        }
        FifoObjPixel& existing = obj_fifo_.at(slot);
        const bool replace = existing.color == 0 ||
                             ((opri_ & 1) == 0 && pixel.color != 0 && pixel.oam_index < existing.oam_index);
        if (replace && pixel.color != 0) existing = pixel;
    }
}

// One dot of mode 3. Returns true once the 160th pixel is out (mode 3 is over).
bool Ppu::draw_dot() {
    if (end_dots_ > 0) return --end_dots_ == 0;  // all 160 pixels are out
    if (startup_dots_ > 0) {
        --startup_dots_;
        return false;
    }

    // A sprite fetch in progress: the BG side waits.
    if (sprite_dots_ > 0) {
        if (--sprite_dots_ == 0) {
            LineSprite& sprite = line_sprites_[static_cast<std::size_t>(fetching_sprite_)];
            merge_sprite(sprite);
            sprite.fetched = true;
        }
        return false;
    }

    // The window starts when its left edge reaches the next pixel.
    if (!window_active_ && (lcdc_ & kLcdcWindowEnable) && window_y_hit_ && wx_ <= 166 && lx_ + 7 >= wx_) {
        window_active_ = true;
        window_drawn_ = true;
        bg_fifo_.clear();
        fetch_step_ = 0;
        fetch_tile_ = 0;
        // The window has no fine scroll of its own: drop the BG's SCX % 8 discard, and only
        // discard what is off the left edge when WX < 7.
        discard_ = (lx_ == 0 && wx_ < 7) ? 7 - wx_ : 0;
    }

    if (discard_ == 0) {
        if (const int sprite = sprite_at_current_x(); sprite >= 0) {
            // The sprite fetch needs the BG fetcher to have pixels ready first. It can start
            // on the same dot the fetcher pushes them.
            if (bg_fifo_.empty()) {
                fetcher_dot();
                if (bg_fifo_.empty()) return false;
            }
            // Where the sprite's left edge falls within a background (or window) tile. This
            // uses the sprite's own X, so sprites partly off the left edge differ too.
            const int sprite_x = line_sprites_[static_cast<std::size_t>(sprite)].x;
            // (OAM X is the screen X + 8, which doesn't change anything modulo 8. The window
            // case adds 256 to stay positive.)
            const int plane_x = window_active_ ? sprite_x + 256 - wx_ - 1 : sprite_x + scx_;
            const int x_in_tile = plane_x & 7;
            const int tile = plane_x / 8;
            int extra = 0;
            if (tile != penalty_tile_) {
                extra = std::max(0, 5 - x_in_tile);
                penalty_tile_ = tile;
            }
            fetching_sprite_ = sprite;
            sprite_stall_dots_ += kSpriteFetchDots + extra;
            sprite_dots_ = kSpriteFetchDots + extra - 1;  // this dot is the first one
            if (sprite_dots_ == 0) {
                merge_sprite(line_sprites_[static_cast<std::size_t>(sprite)]);
                line_sprites_[static_cast<std::size_t>(sprite)].fetched = true;
            }
            return false;
        }
    }

    fetcher_dot();
    if (bg_fifo_.empty()) return false;

    const FifoBgPixel bg = bg_fifo_.pop();
    if (discard_ > 0) {
        --discard_;
        return false;
    }
    const FifoObjPixel obj = obj_fifo_.empty() ? FifoObjPixel{} : obj_fifo_.pop();
    frame_[static_cast<std::size_t>(ly_) * kLcdWidth + static_cast<std::size_t>(lx_)] = mix_pixel(bg, obj);
    if (++lx_ < kLcdWidth) return false;
    end_dots_ = std::max(0, kEndSlackDots - sprite_stall_dots_);
    return end_dots_ == 0;
}

// Final colour from a BG pixel and a (possibly transparent) sprite pixel.
std::uint16_t Ppu::mix_pixel(const FifoBgPixel& bg, const FifoObjPixel& obj) const {
    // On DMG (and in compatibility mode) LCDC.0 blanks BG and window; in CGB mode it only
    // takes away their priority over sprites.
    const bool bg_on = cgb_mode_ || (lcdc_ & kLcdcBgEnable);
    const std::uint8_t bg_index = bg_on ? bg.color : 0;
    const bool obj_on = (lcdc_ & kLcdcObjEnable) && obj.color != 0;
    if (obj_on) {
        const bool bg_opaque = bg_index != 0;
        const bool behind_bg = cgb_mode_ ? (lcdc_ & kLcdcBgEnable) && bg_opaque && ((obj.attributes & 0x80) || bg.priority)
                                         : (obj.attributes & 0x80) && bg_opaque;
        if (!behind_bg) return obj_color(obj.attributes, obj.color);
    }
    return bg_color(bg_on ? bg.palette : 0, bg_index);
}

}  // namespace ezy::gb
