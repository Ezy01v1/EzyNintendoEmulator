#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "gba/cpu/cpu_bus.hpp"
#include "gba/scheduler.hpp"

namespace ezy::gba {

// Region sizes (GBATEK, "GBA Memory Map").
inline constexpr std::size_t kBiosSize = 16 * 1024;
inline constexpr std::size_t kEwramSize = 256 * 1024;
inline constexpr std::size_t kIwramSize = 32 * 1024;
inline constexpr std::size_t kIoSize = 0x400;
inline constexpr std::size_t kPaletteSize = 1024;
inline constexpr std::size_t kVramSize = 96 * 1024;
inline constexpr std::size_t kOamSize = 1024;
inline constexpr std::size_t kSramSize = 64 * 1024;

// The GBA memory bus: maps the 32-bit address space onto each memory region, with the
// mirroring the hardware does inside each region.
//
// Accesses are aligned here (read16 ignores bit 0, read32 bits 0-1), which is what the
// memory itself sees. What the CPU does with a misaligned load (e.g. LDR rotating the
// word) is CPU behaviour; see rotate_misaligned_word().
//
// Two access paths:
// - read8/16/32(addr) and write8/16/32(addr, value): untimed, for tests, DMA set-up and
//   the debugger.
// - The CpuBus overrides (with an Access argument): what the CPU uses. Each one also
//   charges the access's cost in cycles to the Scheduler, using the waitstate table
//   (GBATEK "GBA Memory Map") and WAITCNT for the cartridge.
//
// Not modelled yet: the ROM prefetch buffer, the non-sequential penalty at 128 KiB ROM
// boundaries, I/O register side effects (plain storage for now), BIOS read protection,
// real open-bus values outside the ROM.
class Bus final : public CpuBus {
public:
    static constexpr std::uint32_t kWaitcnt = 0x04000204;

    // `bios` may be empty (no BIOS loaded): reads from it then return 0. Without a
    // scheduler, CPU accesses are simply not timed.
    explicit Bus(std::vector<std::uint8_t> rom, std::span<const std::uint8_t> bios = {},
                 Scheduler* scheduler = nullptr);

    [[nodiscard]] std::uint8_t read8(std::uint32_t addr) const;
    [[nodiscard]] std::uint16_t read16(std::uint32_t addr) const;
    [[nodiscard]] std::uint32_t read32(std::uint32_t addr) const;

    void write8(std::uint32_t addr, std::uint8_t value);
    void write16(std::uint32_t addr, std::uint16_t value);
    void write32(std::uint32_t addr, std::uint32_t value);

    // --- CpuBus: timed CPU accesses ---
    std::uint8_t read8(std::uint32_t addr, Access access) override;
    std::uint16_t read16(std::uint32_t addr, Access access) override;
    std::uint32_t read32(std::uint32_t addr, Access access) override;
    void write8(std::uint32_t addr, std::uint8_t value, Access access) override;
    void write16(std::uint32_t addr, std::uint16_t value, Access access) override;
    void write32(std::uint32_t addr, std::uint32_t value, Access access) override;
    void idle() override { charge(1); }

    // Cycles one access costs: 1 plus the region's waitstates. `size` is 1, 2 or 4.
    [[nodiscard]] std::uint32_t access_cycles(std::uint32_t addr, std::uint32_t size, Access access) const;

private:
    void charge(std::uint32_t cycles) {
        if (scheduler_ != nullptr) scheduler_->advance(cycles);
    }

    template <typename T>
    [[nodiscard]] T read(std::uint32_t addr) const;
    template <typename T>
    void write(std::uint32_t addr, T value);

    // Backing storage for a byte address (and the offset into it), or an empty span if
    // unmapped. One implementation for const and non-const access.
    template <typename Self>
    [[nodiscard]] static auto resolve(Self& self, std::uint32_t addr, std::size_t& offset);

    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> bios_;
    std::vector<std::uint8_t> ewram_;
    std::array<std::uint8_t, kIwramSize> iwram_{};
    std::array<std::uint8_t, kIoSize> io_{};
    std::array<std::uint8_t, kPaletteSize> palette_{};
    std::vector<std::uint8_t> vram_;
    std::array<std::uint8_t, kOamSize> oam_{};
    std::vector<std::uint8_t> sram_;
    Scheduler* scheduler_ = nullptr;
};

// LDR from a misaligned address reads the aligned word and rotates it right by
// 8 * (addr & 3), so the addressed byte ends up in bits 0-7 (ARM7TDMI behaviour).
[[nodiscard]] constexpr std::uint32_t rotate_misaligned_word(std::uint32_t aligned_word, std::uint32_t addr) {
    return std::rotr(aligned_word, static_cast<int>((addr & 3) * 8));
}

}  // namespace ezy::gba
