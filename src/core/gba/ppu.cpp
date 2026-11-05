#include "gba/ppu.hpp"

namespace ezy::gba {

namespace {

// I/O offsets from 0x04000000.
constexpr std::uint32_t kDispcnt = 0x000;
constexpr std::uint32_t kDispstat = 0x004;
constexpr std::uint32_t kVcount = 0x006;
constexpr std::uint32_t kIf = 0x202;

// DISPCNT
constexpr std::uint16_t kModeMask = 0x0007;
constexpr std::uint16_t kPageSelect = 1U << 4;
constexpr std::uint16_t kForcedBlank = 1U << 7;
constexpr std::uint16_t kBg2Enable = 1U << 10;

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
         Scheduler* scheduler)
    : io_(io), palette_(palette), vram_(vram), scheduler_(scheduler) {
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
    if (vcount_ < kScreenHeight) render_line(vcount_);
    update_status();
    if (scheduler_ != nullptr) scheduler_->schedule(EventType::PpuHBlankEnd, when + kCyclesHBlank);
}

void Ppu::on_hblank_end(std::uint64_t when) {
    hblank_ = false;
    vcount_ = (vcount_ + 1) % kLinesPerFrame;
    if (vcount_ == kVBlankFirstLine) {
        vblank_ = true;
        frame_done_ = true;
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
    const std::uint16_t backdrop = palette16(0) & kWhite;
    const bool bg2 = (dispcnt & kBg2Enable) != 0;
    const std::uint32_t page = (dispcnt & kPageSelect) != 0 ? kPageOffset : 0;
    const auto uy = static_cast<std::uint32_t>(y);

    for (std::uint32_t x = 0; x < kScreenWidth; ++x) {
        std::uint16_t color = backdrop;
        if (bg2) {
            switch (dispcnt & kModeMask) {
                case 3:  // 240x160, direct 15-bit color
                    color = vram16((uy * kScreenWidth + x) * 2);
                    break;
                case 4:  // 240x160, 8-bit palette index, two pages
                    color = palette16(vram_[page + uy * kScreenWidth + x]);
                    break;
                case 5:  // 160x128, direct color, two pages
                    if (x < 160 && uy < 128) color = vram16(page + (uy * 160 + x) * 2);
                    break;
                default:  // tile modes 0-2: not implemented yet
                    break;
            }
        }
        row[static_cast<std::ptrdiff_t>(x)] = color & kWhite;
    }
}

}  // namespace ezy::gba
