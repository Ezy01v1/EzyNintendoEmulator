#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "gc/gekko/cpu_bus.hpp"
#include "gc/mmio.hpp"

namespace ezy::gc {

// The GameCube's physical address map, as the Gekko sees it (design, "Memoria y bus"; YAGCD,
// "Memory map"; Dolphin's HW/Memmap.cpp):
//   0x00000000-0x017FFFFF  MEM1, 24 MiB (the boot BATs show it at 0x80000000 cached and at
//                          0xC0000000 uncached)
//   0x0C000000-0x0C006FFF  the hardware registers, one MmioHandler per block (MmioBlock)
//   0x0C008000-0x0C008FFF  the write-gather pipe (writes only)
//   0xE0000000-0xE0003FFF  the locked cache, 16 KiB, only while HID2[LCE] is set
// Anything else throws BusError with the address (as does an MMIO block nobody registered),
// so an access the emulator does not model stops it instead of reading 0. Not mapped yet:
// the EFB (0x08000000) and the IPL ROM (0xFFF00000), which the HLE boot does not need.
//
// The locked cache follows HID2[LCE] (design: "cuando HID2 la activa"): the Gekko reports
// every change through set_locked_cache_enabled(), and while LCE is clear the range is
// unmapped like any other address. Its contents survive a disable. (Dolphin maps it always;
// stopping on a disabled access says more.) MEM1 is not mirrored.
class Memory final : public CpuBus {
public:
    static constexpr std::uint32_t kMem1Size = 0x01800000;
    static constexpr std::uint32_t kMmioBase = 0x0C000000;
    static constexpr std::uint32_t kMmioEnd = 0x0C007000;
    static constexpr std::uint32_t kPipeBase = 0x0C008000;
    static constexpr std::uint32_t kPipeSize = 0x1000;
    static constexpr std::uint32_t kLockedCacheBase = 0xE0000000;
    static constexpr std::uint32_t kLockedCacheSize = 0x4000;

    Memory();

    // Registers (or with nullptr, removes) the handler of a register block. Not owned.
    void map_mmio(MmioBlock block, MmioHandler* handler) { mmio_[static_cast<std::size_t>(block)] = handler; }
    // The write-gather pipe's consumer (or nullptr). Not owned.
    void set_write_gather_pipe(WriteGatherPipe* pipe) { pipe_ = pipe; }
    [[nodiscard]] static constexpr std::uint32_t mmio_block_base(MmioBlock block) {
        constexpr std::array<std::uint32_t, kMmioBlockCount> bases = {
            0x0C000000, 0x0C001000, 0x0C002000, 0x0C003000, 0x0C004000,
            0x0C005000, 0x0C006000, 0x0C006400, 0x0C006800, 0x0C006C00};
        return bases[static_cast<std::size_t>(block)];
    }

    // MEM1 as bytes in guest (big-endian) order, for bulk copies (the HLE boot, DMA).
    [[nodiscard]] std::span<std::uint8_t> mem1() { return mem1_; }

    std::uint8_t read8(std::uint32_t paddr) override;
    std::uint16_t read16(std::uint32_t paddr) override;
    std::uint32_t read32(std::uint32_t paddr) override;
    std::uint64_t read64(std::uint32_t paddr) override;
    void write8(std::uint32_t paddr, std::uint8_t value) override;
    void write16(std::uint32_t paddr, std::uint16_t value) override;
    void write32(std::uint32_t paddr, std::uint32_t value) override;
    void write64(std::uint32_t paddr, std::uint64_t value) override;
    void set_locked_cache_enabled(bool enabled) override { locked_cache_enabled_ = enabled; }
    [[nodiscard]] bool locked_cache_enabled() const { return locked_cache_enabled_; }

private:
    // The RAM a physical range of `bytes` falls in (MEM1 or the locked cache), or nullptr.
    std::uint8_t* ram(std::uint32_t paddr, std::uint32_t bytes);
    // The handler for an MMIO address, and the offset in its block. Throws BusError for an
    // address between blocks, or a block with no handler.
    MmioHandler& mmio(std::uint32_t paddr, std::uint32_t& offset);
    // Everything that is not RAM: MMIO, the pipe, or a BusError.
    std::uint64_t read_io(std::uint32_t paddr, int bytes);
    void write_io(std::uint32_t paddr, std::uint64_t value, int bytes);

    std::vector<std::uint8_t> mem1_;
    std::vector<std::uint8_t> locked_cache_;
    std::array<MmioHandler*, kMmioBlockCount> mmio_{};
    WriteGatherPipe* pipe_ = nullptr;
    bool locked_cache_enabled_ = false;  // HID2[LCE], as the Gekko last reported it
};

}  // namespace ezy::gc
