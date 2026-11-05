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
      sram_(kSramSize, 0xFF),  // erased flash/SRAM reads back as 0xFF
      scheduler_(scheduler),
      ppu_(io_, palette_, vram_, scheduler) {
    std::copy_n(bios.begin(), std::min(bios.size(), kBiosSize), bios_.begin());
    set_keys(0);
}

namespace {

constexpr std::uint32_t kIoIf = 0x202;
constexpr std::uint32_t kIoKeyinput = 0x130;
constexpr std::uint32_t kIoHaltcnt = 0x301;
constexpr std::uint32_t kIoPpuStatusFirst = 0x004;  // DISPSTAT
constexpr std::uint32_t kIoPpuStatusLast = 0x007;   // VCOUNT high byte

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
            if (is_sram(region)) {
                offset = addr & (kSramSize - 1);
                return Span{self.sram_};
            }
            return Span{};
    }
}

template <typename T>
T Bus::read(std::uint32_t addr) const {
    addr &= ~static_cast<std::uint32_t>(sizeof(T) - 1);
    const std::uint32_t region = region_of(addr);

    if (is_sram(region)) {
        // 8-bit bus: wider reads see the same byte in every lane.
        const std::uint8_t byte = sram_[addr & (kSramSize - 1)];
        return static_cast<T>(byte * static_cast<std::uint32_t>(sizeof(T) == 1 ? 0x01 : sizeof(T) == 2 ? 0x0101 : 0x01010101));
    }

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
    return value;
}

template <typename T>
void Bus::write(std::uint32_t addr, T value) {
    addr &= ~static_cast<std::uint32_t>(sizeof(T) - 1);
    const std::uint32_t region = region_of(addr);
    if (region == kBios || is_rom(region)) return;  // read-only

    if (is_sram(region)) {
        // 8-bit bus: only one byte reaches the chip.
        sram_[addr & (kSramSize - 1)] = static_cast<std::uint8_t>(value);
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
    std::memcpy(memory.data() + offset, &value, sizeof(T));
    for (std::uint32_t b = kIoIf; b <= kIoIf + 1; ++b) {
        // IF: writing 1 to a bit acknowledges (clears) that interrupt.
        if (overlaps(offset, sizeof(T), b, b)) io_[b] = static_cast<std::uint8_t>(old_if[b - kIoIf] & ~io_[b]);
    }
    if (overlaps(offset, sizeof(T), kIoKeyinput, kIoKeyinput + 1)) set_keys(keys_pressed_);  // read-only
    if (overlaps(offset, sizeof(T), kIoPpuStatusFirst, kIoPpuStatusLast)) ppu_.after_io_write();
    if (overlaps(offset, sizeof(T), kIoHaltcnt, kIoHaltcnt)) halted_ = true;
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
    return read<std::uint16_t>(addr);
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

}  // namespace ezy::gba
