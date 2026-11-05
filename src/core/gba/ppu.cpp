#include "gba/ppu.hpp"

#include <algorithm>

namespace ezy::gba {

namespace {

// I/O offsets from 0x04000000.
constexpr std::uint32_t kDispcnt = 0x000;
constexpr std::uint32_t kDispstat = 0x004;
constexpr std::uint32_t kVcount = 0x006;
constexpr std::uint32_t kIf = 0x202;
constexpr std::uint32_t kBgcnt = 0x008;         // BG0CNT..BG3CNT
constexpr std::uint32_t kBgHofs = 0x010;        // BGxHOFS/VOFS, 4 bytes per BG
constexpr std::uint32_t kBgAffineBase = 0x020;  // BG2PA..BG2Y, then BG3 at +0x10

constexpr std::uint32_t kVramObjBase = 0x10000;

// DISPCNT
constexpr std::uint16_t kModeMask = 0x0007;
constexpr std::uint16_t kPageSelect = 1U << 4;
constexpr std::uint16_t kObj1D = 1U << 6;
constexpr std::uint16_t kForcedBlank = 1U << 7;
constexpr std::uint16_t kBg0Enable = 1U << 8;  // BG1-3 follow
constexpr std::uint16_t kObjEnable = 1U << 12;

// BGxCNT
constexpr std::uint16_t kBg256Colors = 1U << 7;
constexpr std::uint16_t kBgWrap = 1U << 13;  // affine BGs only

// Text BG map entry
constexpr std::uint16_t kEntryHFlip = 1U << 10;
constexpr std::uint16_t kEntryVFlip = 1U << 11;

// OBJ attributes
constexpr std::uint16_t kObjAffine = 1U << 8;          // attr 0
constexpr std::uint16_t kObjDoubleOrHidden = 1U << 9;  // attr 0: double size if affine, else hidden
constexpr std::uint16_t kObj256Colors = 1U << 13;      // attr 0
constexpr std::uint16_t kObjHFlip = 1U << 12;          // attr 1, regular sprites
constexpr std::uint16_t kObjVFlip = 1U << 13;          // attr 1, regular sprites

// DISPSTAT
constexpr std::uint16_t kStatVBlank = 1U << 0;
constexpr std::uint16_t kStatHBlank = 1U << 1;
constexpr std::uint16_t kStatVCount = 1U << 2;
constexpr std::uint16_t kStatReadOnly = kStatVBlank | kStatHBlank | kStatVCount;
constexpr std::uint16_t kStatVBlankIrq = 1U << 3;
constexpr std::uint16_t kStatHBlankIrq = 1U << 4;
constexpr std::uint16_t kStatVCountIrq = 1U << 5;

constexpr int kVBlankFirstLine = 160;
constexpr int kVBlankLastLine = 226;  // the flag is clear again on line 227

constexpr std::uint32_t kPageOffset = 0xA000;  // second frame buffer in modes 4 and 5
constexpr std::uint16_t kWhite = 0x7FFF;

}  // namespace

Ppu::Ppu(std::span<std::uint8_t> io, std::span<const std::uint8_t> palette, std::span<const std::uint8_t> vram,
         std::span<const std::uint8_t> oam, Scheduler* scheduler)
    : io_(io), palette_(palette), vram_(vram), oam_(oam), scheduler_(scheduler) {
    update_status();
}

std::uint16_t Ppu::io16(std::uint32_t offset) const {
    return static_cast<std::uint16_t>(io_[offset] | (io_[offset + 1] << 8));
}

std::uint16_t Ppu::palette16(std::uint32_t index) const {
    const std::uint32_t at = (index * 2) & 0x3FF;
    return static_cast<std::uint16_t>(palette_[at] | (palette_[at + 1] << 8));
}

std::uint16_t Ppu::vram16(std::uint32_t offset) const {
    return static_cast<std::uint16_t>(vram_[offset] | (vram_[offset + 1] << 8));
}

void Ppu::request_irq(std::uint16_t bits) {
    io_[kIf] = static_cast<std::uint8_t>(io_[kIf] | bits);
    io_[kIf + 1] = static_cast<std::uint8_t>(io_[kIf + 1] | (bits >> 8));
}

void Ppu::update_status() {
    std::uint8_t stat = io_[kDispstat] & static_cast<std::uint8_t>(~kStatReadOnly);
    if (vblank_) stat |= kStatVBlank;
    if (hblank_) stat |= kStatHBlank;
    if (vcount_match_) stat |= kStatVCount;
    io_[kDispstat] = stat;
    io_[kVcount] = static_cast<std::uint8_t>(vcount_);
    io_[kVcount + 1] = 0;
}

void Ppu::after_io_write() {
    update_status();
    check_vcount_match();
}

void Ppu::check_vcount_match() {
    const bool match = vcount_ == io_[kDispstat + 1];  // LYC is DISPSTAT bits 8-15
    // The IRQ fires when the match starts, not continuously while it holds.
    if (match && !vcount_match_ && (io16(kDispstat) & kStatVCountIrq) != 0) request_irq(irq::kVCount);
    vcount_match_ = match;
    update_status();
}

void Ppu::start() {
    if (scheduler_ != nullptr) scheduler_->schedule_in(EventType::PpuHDrawEnd, kCyclesHDraw);
    check_vcount_match();
}

void Ppu::on_hdraw_end(std::uint64_t when) {
    hblank_ = true;
    if ((io16(kDispstat) & kStatHBlankIrq) != 0) request_irq(irq::kHBlank);  // also during VBlank
    if (vcount_ < kScreenHeight) {
        render_line(vcount_);
        advance_affine_references();
    }
    update_status();
    if (scheduler_ != nullptr) scheduler_->schedule(EventType::PpuHBlankEnd, when + kCyclesHBlank);
}

void Ppu::on_hblank_end(std::uint64_t when) {
    hblank_ = false;
    vcount_ = (vcount_ + 1) % kLinesPerFrame;
    if (vcount_ == kVBlankFirstLine) {
        vblank_ = true;
        frame_done_ = true;
        for (int bg = 2; bg <= 3; ++bg) {
            reload_affine_reference(bg, false);
            reload_affine_reference(bg, true);
        }
        if ((io16(kDispstat) & kStatVBlankIrq) != 0) request_irq(irq::kVBlank);
    } else if (vcount_ == kVBlankLastLine + 1) {
        vblank_ = false;
    }
    check_vcount_match();  // also calls update_status()
    if (scheduler_ != nullptr) scheduler_->schedule(EventType::PpuHDrawEnd, when + kCyclesHDraw);
}

bool Ppu::take_frame_done() {
    const bool done = frame_done_;
    frame_done_ = false;
    return done;
}

void Ppu::render_line(int y) {
    const std::uint16_t dispcnt = io16(kDispcnt);
    const auto row = framebuffer_.begin() + y * kScreenWidth;
    if ((dispcnt & kForcedBlank) != 0) {
        std::fill(row, row + kScreenWidth, kWhite);
        return;
    }
    const int mode = dispcnt & kModeMask;

    // Which backgrounds this mode has, and of which kind.
    enum class Kind : std::uint8_t { None, Text, Affine, Bitmap };
    std::array<Kind, 4> kinds{};
    switch (mode) {
        case 0: kinds = {Kind::Text, Kind::Text, Kind::Text, Kind::Text}; break;
        case 1: kinds = {Kind::Text, Kind::Text, Kind::Affine, Kind::None}; break;
        case 2: kinds = {Kind::None, Kind::None, Kind::Affine, Kind::Affine}; break;
        case 3:
        case 4:
        case 5: kinds = {Kind::None, Kind::None, Kind::Bitmap, Kind::None}; break;
        default: break;  // modes 6 and 7 are invalid: backdrop (and sprites) only
    }

    std::array<LineBuffer, 4> bgs{};
    std::array<std::uint8_t, 4> bg_prio{};
    std::array<std::size_t, 4> order{};  // enabled BGs, sorted front to back below
    std::size_t count = 0;
    for (std::size_t bg = 0; bg < 4; ++bg) {
        bg_prio[bg] = static_cast<std::uint8_t>(io16(kBgcnt + static_cast<std::uint32_t>(bg) * 2) & 3);
        if (kinds[bg] == Kind::None || (dispcnt & (kBg0Enable << bg)) == 0) continue;
        const int index = static_cast<int>(bg);
        switch (kinds[bg]) {
            case Kind::Text: render_text_bg(index, y, bgs[bg]); break;
            case Kind::Affine: render_affine_bg(index, bgs[bg]); break;
            case Kind::Bitmap: render_bitmap_bg(mode, y, bgs[bg]); break;
            case Kind::None: break;
        }
        order[count++] = bg;
    }
    // Same priority: the lower-numbered BG is in front (stable sort keeps index order).
    std::stable_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count),
                     [&](std::size_t a, std::size_t b) { return bg_prio[a] < bg_prio[b]; });

    LineBuffer obj;
    obj.fill(kTransparent);
    std::array<std::uint8_t, kScreenWidth> obj_prio{};
    if ((dispcnt & kObjEnable) != 0) render_sprites(y, obj, obj_prio);

    const std::uint16_t backdrop = palette16(0) & kWhite;
    for (std::size_t x = 0; x < kScreenWidth; ++x) {
        std::uint16_t color = backdrop;
        int best = 4;  // the backdrop sits behind priority 3
        if (obj[x] != kTransparent) {
            color = obj[x];
            best = obj_prio[x];
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t bg = order[i];
            if (bg_prio[bg] >= best) break;  // a sprite wins ties with a BG
            if (bgs[bg][x] != kTransparent) {
                color = bgs[bg][x];
                break;
            }
        }
        row[static_cast<std::ptrdiff_t>(x)] = color;
    }
}

void Ppu::reload_affine_reference(int bg, bool y) {
    const std::uint32_t at = kBgAffineBase + static_cast<std::uint32_t>(bg - 2) * 0x10 + (y ? 0xC : 0x8);
    const std::uint32_t raw = io16(at) | (static_cast<std::uint32_t>(io16(at + 2)) << 16);
    const auto value = static_cast<std::int32_t>(raw << 4) >> 4;  // 28-bit signed
    (y ? affine_y_ : affine_x_)[static_cast<std::size_t>(bg - 2)] = value;
}

void Ppu::advance_affine_references() {
    for (std::size_t i = 0; i < 2; ++i) {
        const std::uint32_t base = kBgAffineBase + static_cast<std::uint32_t>(i) * 0x10;
        affine_x_[i] += static_cast<std::int16_t>(io16(base + 2));  // PB: x step per line
        affine_y_[i] += static_cast<std::int16_t>(io16(base + 6));  // PD: y step per line
    }
}

void Ppu::render_text_bg(int bg, int y, LineBuffer& out) const {
    const auto ubg = static_cast<std::uint32_t>(bg);
    const std::uint16_t cnt = io16(kBgcnt + ubg * 2);
    const std::uint32_t char_base = ((cnt >> 2) & 3U) * 0x4000;
    const std::uint32_t screen_base = ((cnt >> 8) & 31U) * 0x800;
    const bool bpp8 = (cnt & kBg256Colors) != 0;
    const std::uint32_t size = cnt >> 14;
    const std::uint32_t width = (size & 1) != 0 ? 512 : 256;
    const std::uint32_t height = (size & 2) != 0 ? 512 : 256;
    const std::uint32_t hofs = io16(kBgHofs + ubg * 4) & 0x1FF;
    const std::uint32_t vofs = io16(kBgHofs + ubg * 4 + 2) & 0x1FF;
    const std::uint32_t py = (static_cast<std::uint32_t>(y) + vofs) & (height - 1);

    for (std::uint32_t x = 0; x < kScreenWidth; ++x) {
        const std::uint32_t px = (x + hofs) & (width - 1);
        // The map is made of 32x32-entry screen blocks: left to right, then top to bottom.
        std::uint32_t block = px >= 256 ? 1 : 0;
        if (py >= 256) block += width == 512 ? 2 : 1;
        const std::uint32_t entry_at = screen_base + block * 0x800 + (((py & 255) / 8) * 32 + (px & 255) / 8) * 2;
        const std::uint16_t entry = vram16(entry_at & 0xFFFE);
        std::uint32_t tx = px & 7;
        std::uint32_t ty = py & 7;
        if ((entry & kEntryHFlip) != 0) tx = 7 - tx;
        if ((entry & kEntryVFlip) != 0) ty = 7 - ty;
        const std::uint32_t tile = entry & 0x3FFU;

        std::uint32_t index = 0;
        if (bpp8) {
            const std::uint32_t at = char_base + tile * 64 + ty * 8 + tx;
            if (at < kVramObjBase) index = vram_[at];  // BG tiles can't reach into OBJ VRAM
        } else {
            const std::uint32_t at = char_base + tile * 32 + ty * 4 + tx / 2;
            if (at < kVramObjBase) index = (vram_[at] >> ((tx & 1) * 4)) & 0xFU;
            if (index != 0) index += static_cast<std::uint32_t>(entry >> 12) * 16;
        }
        out[x] = index == 0 ? kTransparent : static_cast<std::uint16_t>(palette16(index) & kWhite);
    }
}

void Ppu::render_affine_bg(int bg, LineBuffer& out) const {
    const auto ubg = static_cast<std::uint32_t>(bg);
    const std::uint16_t cnt = io16(kBgcnt + ubg * 2);
    const std::uint32_t char_base = ((cnt >> 2) & 3U) * 0x4000;
    const std::uint32_t screen_base = ((cnt >> 8) & 31U) * 0x800;
    const bool wrap = (cnt & kBgWrap) != 0;
    const auto size = static_cast<std::int32_t>(128U << (cnt >> 14));  // 128..1024 pixels
    const auto tiles_per_row = static_cast<std::uint32_t>(size / 8);
    const std::uint32_t base = kBgAffineBase + (ubg - 2) * 0x10;
    const std::int32_t pa = static_cast<std::int16_t>(io16(base));
    const std::int32_t pc = static_cast<std::int16_t>(io16(base + 4));
    std::int32_t fx = affine_x_[ubg - 2];
    std::int32_t fy = affine_y_[ubg - 2];

    for (std::uint32_t x = 0; x < kScreenWidth; ++x, fx += pa, fy += pc) {
        out[x] = kTransparent;
        std::int32_t tx = fx >> 8;  // arithmetic shift: floor of the 8-bit fraction
        std::int32_t ty = fy >> 8;
        if (wrap) {
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            continue;
        }
        const auto utx = static_cast<std::uint32_t>(tx);
        const auto uty = static_cast<std::uint32_t>(ty);
        const std::uint32_t entry_at = screen_base + (uty / 8) * tiles_per_row + utx / 8;  // 1 byte per entry
        if (entry_at >= kVramObjBase) continue;
        const std::uint32_t at = char_base + vram_[entry_at] * 64U + (uty & 7) * 8 + (utx & 7);  // always 256 colors
        if (at >= kVramObjBase) continue;
        const std::uint8_t index = vram_[at];
        if (index != 0) out[x] = palette16(index) & kWhite;
    }
}

void Ppu::render_bitmap_bg(int mode, int y, LineBuffer& out) const {
    const std::uint32_t page = (io16(kDispcnt) & kPageSelect) != 0 ? kPageOffset : 0;
    const auto uy = static_cast<std::uint32_t>(y);
    for (std::uint32_t x = 0; x < kScreenWidth; ++x) {
        std::uint16_t color = kTransparent;
        switch (mode) {
            case 3:  // 240x160, direct 15-bit color
                color = vram16((uy * kScreenWidth + x) * 2) & kWhite;
                break;
            case 4: {  // 240x160, 8-bit palette index, two pages; index 0 is transparent
                const std::uint8_t index = vram_[page + uy * kScreenWidth + x];
                if (index != 0) color = palette16(index) & kWhite;
                break;
            }
            case 5:  // 160x128, direct color, two pages
                if (x < 160 && uy < 128) color = vram16(page + (uy * 160 + x) * 2) & kWhite;
                break;
            default:
                break;
        }
        out[x] = color;
    }
}

void Ppu::render_sprites(int y, LineBuffer& color, std::array<std::uint8_t, kScreenWidth>& priority) const {
    // [shape][size] -> {width, height} (GBATEK "OBJ Attributes").
    static constexpr std::array<std::array<std::array<int, 2>, 4>, 3> kSizes = {{
        {{{8, 8}, {16, 16}, {32, 32}, {64, 64}}},  // square
        {{{16, 8}, {32, 8}, {32, 16}, {64, 32}}},  // horizontal
        {{{8, 16}, {8, 32}, {16, 32}, {32, 64}}},  // vertical
    }};
    const std::uint16_t dispcnt = io16(kDispcnt);
    const bool one_d = (dispcnt & kObj1D) != 0;
    const bool bitmap_mode = (dispcnt & kModeMask) >= 3;
    const auto oam16 = [this](std::uint32_t at) {
        return static_cast<std::uint16_t>(oam_[at] | (oam_[at + 1] << 8));
    };

    for (std::uint32_t i = 0; i < 128; ++i) {
        const std::uint16_t a0 = oam16(i * 8);
        const std::uint16_t a1 = oam16(i * 8 + 2);
        const std::uint16_t a2 = oam16(i * 8 + 4);
        const bool affine = (a0 & kObjAffine) != 0;
        const bool double_size = (a0 & kObjDoubleOrHidden) != 0;
        if (!affine && double_size) continue;  // bit 9 hides a regular sprite
        if (((a0 >> 10) & 3U) >= 2) continue;  // OBJ window (needs windows) or prohibited
        const std::uint32_t shape = a0 >> 14U;
        if (shape == 3) continue;
        const auto [w, h] = kSizes[shape][a1 >> 14U];
        const int box_w = affine && double_size ? w * 2 : w;
        const int box_h = affine && double_size ? h * 2 : h;

        const int line = (y - (a0 & 0xFF)) & 0xFF;  // Y wraps around at 256
        if (line >= box_h) continue;
        int sx = a1 & 0x1FF;
        if (sx >= kScreenWidth) sx -= 512;  // 9-bit signed X

        const bool bpp8 = (a0 & kObj256Colors) != 0;
        const std::uint32_t tile = a2 & 0x3FFU;
        if (bitmap_mode && tile < 512) continue;  // the bitmap uses the lower half of OBJ VRAM
        const auto prio = static_cast<std::uint8_t>((a2 >> 10) & 3U);
        const std::uint32_t palette_bank = a2 >> 12U;

        // Affine parameters: group n sits in the 4th halfword of OAM entries 4n..4n+3.
        std::int32_t pa = 0x100;
        std::int32_t pb = 0;
        std::int32_t pc = 0;
        std::int32_t pd = 0x100;
        if (affine) {
            const std::uint32_t group = ((a1 >> 9) & 31U) * 32;
            pa = static_cast<std::int16_t>(oam16(group + 6));
            pb = static_cast<std::int16_t>(oam16(group + 14));
            pc = static_cast<std::int16_t>(oam16(group + 22));
            pd = static_cast<std::int16_t>(oam16(group + 30));
        }
        const std::uint32_t tile_step = bpp8 ? 2 : 1;  // tile numbers count 32-byte units
        const std::uint32_t row_stride = one_d ? static_cast<std::uint32_t>(w / 8) * tile_step : 32;

        for (int bx = 0; bx < box_w; ++bx) {
            const int x = sx + bx;
            if (x < 0 || x >= kScreenWidth) continue;
            int tx = bx;
            int ty = line;
            if (affine) {
                // Map from the box centre into the sprite's texture.
                const int dx = bx - box_w / 2;
                const int dy = line - box_h / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                if ((a1 & kObjHFlip) != 0) tx = w - 1 - tx;
                if ((a1 & kObjVFlip) != 0) ty = h - 1 - ty;
            }
            const auto utx = static_cast<std::uint32_t>(tx);
            const auto uty = static_cast<std::uint32_t>(ty);
            const std::uint32_t tile_index = (tile + (uty / 8) * row_stride + (utx / 8) * tile_step) & 0x3FFU;
            const std::uint32_t in_tile = bpp8 ? (uty & 7) * 8 + (utx & 7) : (uty & 7) * 4 + (utx & 7) / 2;
            const std::uint8_t byte = vram_[kVramObjBase + ((tile_index * 32 + in_tile) & 0x7FFFU)];
            std::uint32_t index = bpp8 ? byte : (byte >> ((utx & 1) * 4)) & 0xFU;
            if (index == 0) continue;
            if (!bpp8) index += palette_bank * 16;
            const auto ux = static_cast<std::size_t>(x);
            // Lowest priority value wins; on a tie the lower OAM index (drawn first) keeps it.
            if (color[ux] == kTransparent || prio < priority[ux]) {
                color[ux] = palette16(256 + index) & kWhite;
                priority[ux] = prio;
            }
        }
    }
}

}  // namespace ezy::gba
