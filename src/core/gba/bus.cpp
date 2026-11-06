#include "gba/bus.hpp"

#include <algorithm>
#include <cstring>

namespace ezy::gba {

static_assert(std::endian::native == std::endian::little, "the bus copies little-endian values directly");

namespace {

enum Region : std::uint32_t {
    kBios = 0x00,
    kEwram = 0x02,
    kIwram = 0x03,
    kIo = 0x04,
    kPalette = 0x05,
    kVram = 0x06,
    kOam = 0x07,
    kRomFirst = 0x08,  // 0x08-0x0D: the ROM, mirrored three times (different waitstates)
    kRomLast = 0x0D,
    kSram = 0x0E,      // 0x0E-0x0F
    kSramMirror = 0x0F,
};

constexpr std::uint32_t region_of(std::uint32_t addr) { return addr >> 24; }
constexpr bool is_rom(std::uint32_t region) { return region >= kRomFirst && region <= kRomLast; }
constexpr bool is_sram(std::uint32_t region) { return region == kSram || region == kSramMirror; }

constexpr std::uint32_t kRomMask = 0x01FFFFFF;  // 32 MiB window

// VRAM is 96 KiB in a 128 KiB window: 0x18000-0x1FFFF mirrors the 32 KiB OBJ area.
constexpr std::size_t vram_offset(std::uint32_t addr) {
    std::size_t offset = addr & 0x1FFFF;
    if (offset >= 0x18000) offset -= 0x8000;
    return offset;
}

constexpr std::size_t kVramObjStart = 0x10000;  // 8-bit writes to OBJ tiles are ignored

}  // namespace

Bus::Bus(std::vector<std::uint8_t> rom, std::span<const std::uint8_t> bios, Scheduler* scheduler)
    : rom_(std::move(rom)),
      bios_(kBiosSize, 0),
      ewram_(kEwramSize, 0),
      vram_(kVramSize, 0),
      backup_(detect_backup_type(rom_)),
      scheduler_(scheduler),
      ppu_(io_, palette_, vram_, oam_, scheduler),
      timers_(io_, scheduler),
      apu_(io_, scheduler) {
    timers_.set_overflow_hook(this);
    std::copy_n(bios.begin(), std::min(bios.size(), kBiosSize), bios_.begin());
    set_keys(0);
    // Reset values: BG2/BG3 PA and PD are 1.0 (identity), so the bitmap modes, which go
    // through BG2's affine transform, show the frame buffer unscaled.
    for (const std::uint32_t at : {0x020U, 0x026U, 0x030U, 0x036U}) io_[at + 1] = 0x01;
    io_[0x089] = 0x02;  // SOUNDBIAS = 0x200: the DAC's midpoint
}

namespace {

constexpr std::uint32_t kIoIf = 0x202;
constexpr std::uint32_t kIoKeyinput = 0x130;
constexpr std::uint32_t kIoKeycnt = 0x132;
constexpr std::uint16_t kIrqKeypad = 1U << 12;
constexpr std::uint32_t kIoHaltcnt = 0x301;
constexpr std::uint32_t kIoTimerFirst = 0x100;
constexpr std::uint32_t kIoTimerLast = 0x10F;
constexpr std::uint32_t kIoDmaBase = 0x0B0;  // channel i at kIoDmaBase + 12 * i
constexpr std::uint32_t kDmaStride = 12;
constexpr std::uint32_t kIoPpuStatusFirst = 0x004;  // DISPSTAT
constexpr std::uint32_t kIoPpuStatusLast = 0x007;   // VCOUNT high byte
constexpr std::uint32_t kIoBg2Ref = 0x028;  // BG2X, BG2Y (BG3X/Y follow 0x10 later)
constexpr std::uint32_t kIoBg3Ref = 0x038;
constexpr std::uint32_t kIoSoundFirst = 0x060;  // SOUND1CNT_L
constexpr std::uint32_t kIoSoundLast = 0x0A7;   // FIFO_B

constexpr bool overlaps(std::size_t offset, std::size_t size, std::uint32_t first, std::uint32_t last) {
    return offset <= last && offset + size > first;
}

}  // namespace

void Bus::request_interrupt(std::uint16_t bits) {
    io_[kIoIf] = static_cast<std::uint8_t>(io_[kIoIf] | bits);
    io_[kIoIf + 1] = static_cast<std::uint8_t>(io_[kIoIf + 1] | (bits >> 8));
}

std::uint16_t Bus::interrupt_enable() const { return static_cast<std::uint16_t>(io_[0x200] | (io_[0x201] << 8)); }
std::uint16_t Bus::interrupt_flags() const { return static_cast<std::uint16_t>(io_[kIoIf] | (io_[kIoIf + 1] << 8)); }

void Bus::set_keys(std::uint16_t pressed) {
    keys_pressed_ = pressed & 0x03FF;
    const auto keyinput = static_cast<std::uint16_t>(~keys_pressed_ & 0x03FF);
    io_[kIoKeyinput] = static_cast<std::uint8_t>(keyinput);
    io_[kIoKeyinput + 1] = static_cast<std::uint8_t>(keyinput >> 8);
    check_keypad_interrupt();
}

// KEYCNT: bits 0-9 select keys, bit 14 enables the IRQ, bit 15 picks AND (all selected
// keys held) over OR (any of them). Checked whenever the keys or KEYCNT change.
void Bus::check_keypad_interrupt() {
    const auto keycnt = static_cast<std::uint16_t>(io_[kIoKeycnt] | (io_[kIoKeycnt + 1] << 8));
    if ((keycnt & 0x4000) == 0) return;
    const std::uint16_t selected = keycnt & 0x03FF;
    const std::uint16_t held = keys_pressed_ & selected;
    const bool fire = (keycnt & 0x8000) != 0 ? (selected != 0 && held == selected) : held != 0;
    if (fire) request_interrupt(kIrqKeypad);
}

template <typename Self>
auto Bus::resolve(Self& self, std::uint32_t addr, std::size_t& offset) {
    using Byte = std::conditional_t<std::is_const_v<Self>, const std::uint8_t, std::uint8_t>;
    using Span = std::span<Byte>;
    const std::uint32_t region = region_of(addr);
    switch (region) {
        case kBios:
            if (addr >= kBiosSize) return Span{};
            offset = addr;
            return Span{self.bios_};
        case kEwram: offset = addr & (kEwramSize - 1); return Span{self.ewram_};
        case kIwram: offset = addr & (kIwramSize - 1); return Span{self.iwram_};
        case kIo:
            if ((addr & 0x00FFFFFF) >= kIoSize) return Span{};  // only 0x04000000-0x040003FF exists
            offset = addr & (kIoSize - 1);
            return Span{self.io_};
        case kPalette: offset = addr & (kPaletteSize - 1); return Span{self.palette_};
        case kVram: offset = vram_offset(addr); return Span{self.vram_};
        case kOam: offset = addr & (kOamSize - 1); return Span{self.oam_};
        default:
            if (is_rom(region)) {
                offset = addr & kRomMask;
                if (offset >= self.rom_.size()) return Span{};
                return Span{self.rom_};
            }
            return Span{};  // the save chip (0x0E-0x0F) is handled by read()/write() directly
    }
}

template <typename T>
T Bus::read(std::uint32_t addr) const {
    addr &= ~static_cast<std::uint32_t>(sizeof(T) - 1);
    const std::uint32_t region = region_of(addr);

    if (is_sram(region)) {
        // 8-bit bus: wider reads see the same byte in every lane.
        const std::uint8_t byte = backup_.read8(addr & (kSramSize - 1));
        return static_cast<T>(byte * static_cast<std::uint32_t>(sizeof(T) == 1 ? 0x01 : sizeof(T) == 2 ? 0x0101 : 0x01010101));
    }
    // Untimed reads only peek at the EEPROM's serial output; CPU and DMA reads advance it.
    if (is_eeprom_address(addr)) return static_cast<T>(backup_.eeprom_peek());

    std::size_t offset = 0;
    const auto memory = resolve(*this, addr, offset);
    if (memory.empty()) {
        if (is_rom(region)) {
            // Past the end of the ROM, the cartridge bus returns the halfword address.
            const auto half = [](std::uint32_t a) { return ((a & kRomMask) >> 1) & 0xFFFF; };
            if constexpr (sizeof(T) == 1) return static_cast<T>(half(addr) >> (8 * (addr & 1)));
            if constexpr (sizeof(T) == 2) return static_cast<T>(half(addr));
            if constexpr (sizeof(T) == 4) return static_cast<T>(half(addr) | (half(addr + 2) << 16));
        }
        return 0;  // unmapped: real open bus (last prefetched opcode) comes with the CPU
    }
    T value = 0;
    std::memcpy(&value, memory.data() + offset, sizeof(T));
    if (region == kIo && overlaps(offset, sizeof(T), kIoTimerFirst, kIoTimerLast)) {
        // TMxCNT_L reads the live counter, not the stored reload value.
        for (std::size_t b = 0; b < sizeof(T); ++b) {
            const std::size_t at = offset + b;
            if (at < kIoTimerFirst || at > kIoTimerLast || (at - kIoTimerFirst) % 4 >= 2) continue;
            const auto counter = timers_.counter(static_cast<int>((at - kIoTimerFirst) / 4));
            const auto byte = static_cast<std::uint8_t>(counter >> (8 * ((at - kIoTimerFirst) % 2)));
            value = static_cast<T>((value & ~(static_cast<T>(0xFF) << (8 * b))) | (static_cast<T>(byte) << (8 * b)));
        }
    }
    return value;
}

template <typename T>
void Bus::write(std::uint32_t addr, T value) {
    addr &= ~static_cast<std::uint32_t>(sizeof(T) - 1);
    const std::uint32_t region = region_of(addr);
    if (is_eeprom_address(addr)) {
        backup_.eeprom_write(static_cast<std::uint16_t>(value));  // bit 0 is the serial input
        return;
    }
    if (region == kBios || is_rom(region)) return;  // read-only

    if (is_sram(region)) {
        // 8-bit bus: only one byte reaches the chip.
        backup_.write8(addr & (kSramSize - 1), static_cast<std::uint8_t>(value));
        return;
    }

    if constexpr (sizeof(T) == 1) {
        // Palette and VRAM have a 16-bit bus: a byte write stores the byte in both halves.
        // OAM and the VRAM OBJ area ignore byte writes entirely.
        if (region == kOam) return;
        if (region == kPalette || region == kVram) {
            if (region == kVram && vram_offset(addr) >= kVramObjStart) return;
            write<std::uint16_t>(addr, static_cast<std::uint16_t>(value * 0x0101));
            return;
        }
    }

    std::size_t offset = 0;
    const auto memory = resolve(*this, addr, offset);
    if (memory.empty()) return;

    if (region != kIo) {
        std::memcpy(memory.data() + offset, &value, sizeof(T));
        return;
    }
    // I/O: plain storage, plus the registers with side effects.
    const std::array<std::uint8_t, 2> old_if = {io_[kIoIf], io_[kIoIf + 1]};
    const std::array<std::uint16_t, 4> old_dma = {dma_control(0), dma_control(1), dma_control(2), dma_control(3)};
    std::memcpy(memory.data() + offset, &value, sizeof(T));
    for (std::uint32_t b = kIoIf; b <= kIoIf + 1; ++b) {
        // IF: writing 1 to a bit acknowledges (clears) that interrupt.
        if (overlaps(offset, sizeof(T), b, b)) io_[b] = static_cast<std::uint8_t>(old_if[b - kIoIf] & ~io_[b]);
    }
    if (overlaps(offset, sizeof(T), kIoKeyinput, kIoKeyinput + 1)) set_keys(keys_pressed_);  // read-only
    if (overlaps(offset, sizeof(T), kIoKeycnt, kIoKeycnt + 1)) check_keypad_interrupt();
    if (overlaps(offset, sizeof(T), kIoPpuStatusFirst, kIoPpuStatusLast)) ppu_.after_io_write();
    for (std::uint32_t r = 0; r < 4; ++r) {
        // Writing BGxX/BGxY reloads that internal reference point at once.
        const std::uint32_t at = (r < 2 ? kIoBg2Ref : kIoBg3Ref) + (r % 2) * 4;
        if (overlaps(offset, sizeof(T), at, at + 3)) ppu_.reload_affine_reference(2 + static_cast<int>(r / 2), r % 2 == 1);
    }
    if (overlaps(offset, sizeof(T), kIoHaltcnt, kIoHaltcnt)) halted_ = true;
    if (overlaps(offset, sizeof(T), kIoTimerFirst, kIoTimerLast)) timers_.after_io_write(offset, sizeof(T));
    if (overlaps(offset, sizeof(T), kIoSoundFirst, kIoSoundLast)) apu_.after_io_write(offset, sizeof(T));
    for (int i = 0; i < 4; ++i) {
        const std::uint32_t control = kIoDmaBase + static_cast<std::uint32_t>(i) * kDmaStride + 10;
        if (overlaps(offset, sizeof(T), control, control + 1)) after_dma_control_write(i, old_dma[static_cast<std::size_t>(i)]);
    }
}

std::uint8_t Bus::read8(std::uint32_t addr) const { return read<std::uint8_t>(addr); }
std::uint16_t Bus::read16(std::uint32_t addr) const { return read<std::uint16_t>(addr); }
std::uint32_t Bus::read32(std::uint32_t addr) const { return read<std::uint32_t>(addr); }

void Bus::write8(std::uint32_t addr, std::uint8_t value) { write(addr, value); }
void Bus::write16(std::uint32_t addr, std::uint16_t value) { write(addr, value); }
void Bus::write32(std::uint32_t addr, std::uint32_t value) { write(addr, value); }

// --- Timing ---

namespace {

// WAITCNT first-access (N) waitstates for SRAM and the three ROM windows: 4, 3, 2, 8.
constexpr std::array<std::uint32_t, 4> kWaitN = {4, 3, 2, 8};
// Second-access (S) waitstates per ROM window, for WAITCNT bit = 0 / 1.
constexpr std::array<std::array<std::uint32_t, 2>, 3> kWaitS = {{{2, 1}, {4, 1}, {8, 1}}};

}  // namespace

std::uint32_t Bus::access_cycles(std::uint32_t addr, std::uint32_t size, Access access) const {
    const std::uint32_t region = region_of(addr);
    const bool wide = size == 4;
    switch (region) {
        case kEwram: return wide ? 6 : 3;  // 16-bit bus, 2 waitstates per halfword
        case kPalette:
        case kVram: return wide ? 2 : 1;   // 16-bit bus, no waitstates
        default: break;
    }
    const auto waitcnt = static_cast<std::uint32_t>(io_[0x204] | (io_[0x205] << 8));
    if (is_rom(region)) {
        const std::uint32_t window = (region - kRomFirst) / 2;  // WS0, WS1, WS2
        const std::uint32_t n = kWaitN[(waitcnt >> (2 + window * 3)) & 3];
        const std::uint32_t s = kWaitS[window][(waitcnt >> (4 + window * 3)) & 1];
        const std::uint32_t first = 1 + ((access & kSeq) != 0 ? s : n);
        // 16-bit bus: a word is two halfword accesses, the second always sequential.
        return wide ? first + 1 + s : first;
    }
    if (is_sram(region)) return 1 + kWaitN[waitcnt & 3];
    return 1;  // BIOS, IWRAM, I/O, OAM (32-bit, no waitstates) and unmapped space
}

std::uint8_t Bus::read8(std::uint32_t addr, Access access) {
    charge(access_cycles(addr, 1, access));
    return read<std::uint8_t>(addr);
}

std::uint16_t Bus::read16(std::uint32_t addr, Access access) {
    charge(access_cycles(addr, 2, access));
    if (is_eeprom_address(addr)) return backup_.eeprom_read();
    return read<std::uint16_t>(addr);
}

bool Bus::is_eeprom_address(std::uint32_t addr) const {
    // 0x0D000000-0x0DFFFFFF, or only its last 256 bytes when the ROM is bigger than 16 MiB
    // (the rest of 0x0D is then ROM).
    if (!backup_.is_eeprom() || region_of(addr) != 0x0D) return false;
    return rom_.size() <= 16 * 1024 * 1024 || (addr & 0x00FFFFFF) >= 0x00FFFF00;
}

std::uint32_t Bus::read32(std::uint32_t addr, Access access) {
    charge(access_cycles(addr, 4, access));
    return read<std::uint32_t>(addr);
}

void Bus::write8(std::uint32_t addr, std::uint8_t value, Access access) {
    charge(access_cycles(addr, 1, access));
    write(addr, value);
}

void Bus::write16(std::uint32_t addr, std::uint16_t value, Access access) {
    charge(access_cycles(addr, 2, access));
    write(addr, value);
}

void Bus::write32(std::uint32_t addr, std::uint32_t value, Access access) {
    charge(access_cycles(addr, 4, access));
    write(addr, value);
}

// --- DMA ---

namespace {

constexpr std::uint16_t kDmaRepeat = 1U << 9;
constexpr std::uint16_t kDmaWord = 1U << 10;
constexpr std::uint16_t kDmaIrq = 1U << 14;
constexpr std::uint16_t kDmaEnable = 1U << 15;

// Address masks: DMA0 can only reach internal memory; DMA3 may also write the cartridge.
constexpr std::array<std::uint32_t, 4> kDmaSrcMask = {0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF};
constexpr std::array<std::uint32_t, 4> kDmaDstMask = {0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF};

}  // namespace

std::uint16_t Bus::dma_control(int index) const {
    const std::uint32_t at = kIoDmaBase + static_cast<std::uint32_t>(index) * kDmaStride + 10;
    return static_cast<std::uint16_t>(io_[at] | (io_[at + 1] << 8));
}

void Bus::after_dma_control_write(int index, std::uint16_t old_control) {
    const std::uint16_t control = dma_control(index);
    if ((old_control & kDmaEnable) != 0 || (control & kDmaEnable) == 0) return;  // only 0 -> 1 arms it

    // Latch source, destination and count.
    const std::uint32_t base = kIoDmaBase + static_cast<std::uint32_t>(index) * kDmaStride;
    const auto u32 = [&](std::uint32_t at) {
        return static_cast<std::uint32_t>(io_[at] | (io_[at + 1] << 8) | (io_[at + 2] << 16) | (io_[at + 3] << 24));
    };
    DmaChannel& ch = dma_[static_cast<std::size_t>(index)];
    const std::uint32_t align = (control & kDmaWord) != 0 ? ~3U : ~1U;
    ch.src = u32(base) & kDmaSrcMask[static_cast<std::size_t>(index)] & align;
    ch.dst = u32(base + 4) & kDmaDstMask[static_cast<std::size_t>(index)] & align;
    const std::uint32_t count = io_[base + 8] | (io_[base + 9] << 8);
    const std::uint32_t max = index == 3 ? 0x10000 : 0x4000;
    ch.count = (count & (max - 1)) == 0 ? max : (count & (max - 1));

    if (static_cast<DmaTiming>((control >> 12) & 3) == DmaTiming::Immediate) run_dma(index);
}

// Channels with the given timing run in priority order (DMA0 first).
void Bus::trigger_dma(DmaTiming timing) {
    for (int i = 0; i < 4; ++i) {
        const std::uint16_t control = dma_control(i);
        if ((control & kDmaEnable) != 0 && static_cast<DmaTiming>((control >> 12) & 3) == timing) run_dma(i);
    }
}

// Runs a whole transfer at once (the CPU is stalled during it anyway), charging every
// access plus 2 cycles of start-up. Special timing on DMA 1/2 is the sound FIFO refill
// (trigger_fifo_dma); on DMA 3 (video capture) it isn't emulated, so it never fires.
void Bus::run_dma(int index, bool sound_fifo) {
    if (dma_running_) return;  // a DMA writing DMA registers doesn't start another one
    dma_running_ = true;
    DmaChannel& ch = dma_[static_cast<std::size_t>(index)];
    const std::uint16_t control = dma_control(index);
    const bool word = sound_fifo || (control & kDmaWord) != 0;
    const std::uint32_t unit = word ? 4 : 2;
    const auto step = [unit](std::uint32_t mode) -> std::int32_t {
        switch (mode) {
            case 0: case 3: return static_cast<std::int32_t>(unit);   // increment (3: + reload)
            case 1: return -static_cast<std::int32_t>(unit);          // decrement
            default: return 0;                                         // fixed
        }
    };
    const std::int32_t dst_step = sound_fifo ? 0 : step((control >> 5) & 3);
    if (sound_fifo) ch.count = 4;  // 16 bytes, whatever the count register says
    const std::int32_t src_step = step((control >> 7) & 3);

    // EEPROM requests are 9/17 (read) or 73/81 (write) halfwords long: that tells its size.
    if (index == 3 && is_eeprom_address(ch.dst)) backup_.eeprom_dma_hint(ch.count);

    charge(2);
    Access access = static_cast<Access>(kDma | kNonseq);
    for (std::uint32_t i = 0; i < ch.count; ++i) {
        if (word) {
            write32(ch.dst, read32(ch.src, access), access);
        } else {
            write16(ch.dst, read16(ch.src, access), access);
        }
        ch.src = static_cast<std::uint32_t>(static_cast<std::int32_t>(ch.src) + src_step);
        ch.dst = static_cast<std::uint32_t>(static_cast<std::int32_t>(ch.dst) + dst_step);
        access = static_cast<Access>(kDma | kSeq);
    }

    if ((control & kDmaIrq) != 0) request_interrupt(static_cast<std::uint16_t>(1U << (8 + index)));
    const auto timing = static_cast<DmaTiming>((control >> 12) & 3);
    const std::uint32_t base = kIoDmaBase + static_cast<std::uint32_t>(index) * kDmaStride;
    if ((control & kDmaRepeat) != 0 && timing != DmaTiming::Immediate) {
        // Repeat: stay armed with a fresh count, and reload the destination if asked to.
        const std::uint32_t count = io_[base + 8] | (io_[base + 9] << 8);
        const std::uint32_t max = index == 3 ? 0x10000 : 0x4000;
        ch.count = (count & (max - 1)) == 0 ? max : (count & (max - 1));
        if (((control >> 5) & 3) == 3) {
            const std::uint32_t dad = static_cast<std::uint32_t>(io_[base + 4] | (io_[base + 5] << 8) |
                                                                 (io_[base + 6] << 16) | (io_[base + 7] << 24));
            ch.dst = dad & kDmaDstMask[static_cast<std::size_t>(index)] & (word ? ~3U : ~1U);
        }
    } else {
        io_[base + 11] = static_cast<std::uint8_t>(io_[base + 11] & ~(kDmaEnable >> 8));  // done: clear enable
    }
    dma_running_ = false;
}

void Bus::on_timer_overflow(int index) {
    if (index > 1) return;  // only timers 0 and 1 clock the FIFOs
    const unsigned refill = apu_.on_timer_overflow(index);
    if ((refill & 1U) != 0) trigger_fifo_dma(0);
    if ((refill & 2U) != 0) trigger_fifo_dma(1);
}

// FIFO A/B is refilled by whichever of DMA 1/2 is armed with special timing and points at it.
void Bus::trigger_fifo_dma(int fifo) {
    const std::uint32_t fifo_addr = 0x040000A0 + static_cast<std::uint32_t>(fifo) * 4;
    for (int i = 1; i <= 2; ++i) {
        const std::uint16_t control = dma_control(i);
        if ((control & kDmaEnable) == 0 || static_cast<DmaTiming>((control >> 12) & 3) != DmaTiming::Special) continue;
        if (dma_[static_cast<std::size_t>(i)].dst != fifo_addr) continue;
        run_dma(i, /*sound_fifo=*/true);
        return;
    }
}

}  // namespace ezy::gba
