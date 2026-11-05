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

Bus::Bus(std::vector<std::uint8_t> rom, std::span<const std::uint8_t> bios)
    : rom_(std::move(rom)),
      bios_(kBiosSize, 0),
      ewram_(kEwramSize, 0),
      vram_(kVramSize, 0),
      sram_(kSramSize, 0xFF) {  // erased flash/SRAM reads back as 0xFF
    std::copy_n(bios.begin(), std::min(bios.size(), kBiosSize), bios_.begin());
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
    std::memcpy(memory.data() + offset, &value, sizeof(T));
}

std::uint8_t Bus::read8(std::uint32_t addr) const { return read<std::uint8_t>(addr); }
std::uint16_t Bus::read16(std::uint32_t addr) const { return read<std::uint16_t>(addr); }
std::uint32_t Bus::read32(std::uint32_t addr) const { return read<std::uint32_t>(addr); }

void Bus::write8(std::uint32_t addr, std::uint8_t value) { write(addr, value); }
void Bus::write16(std::uint32_t addr, std::uint16_t value) { write(addr, value); }
void Bus::write32(std::uint32_t addr, std::uint32_t value) { write(addr, value); }

}  // namespace ezy::gba
