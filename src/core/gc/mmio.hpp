#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ezy::gc {

// The hardware register blocks at physical 0x0C000000 (YAGCD, "Hardware registers"; the
// design's "Memoria y bus"). Memory::mmio_block_base() gives each one's base address.
enum class MmioBlock : std::uint8_t {
    Cp,   // 0x0C000000 command processor
    Pe,   // 0x0C001000 pixel engine
    Vi,   // 0x0C002000 video interface
    Pi,   // 0x0C003000 processor interface
    Mi,   // 0x0C004000 memory interface
    Dsp,  // 0x0C005000 DSP interface
    Di,   // 0x0C006000 DVD interface
    Si,   // 0x0C006400 serial interface
    Exi,  // 0x0C006800 external interface
    Ai,   // 0x0C006C00 audio interface
    Count,
};

inline constexpr std::size_t kMmioBlockCount = static_cast<std::size_t>(MmioBlock::Count);

// What a peripheral implements to own a register block (Tasks 4-6). `offset` is from the
// block's base. The registers are 16 and 32 bits wide, so Memory throws BusError for 8-bit
// and 64-bit accesses instead of calling the handler; a handler throws BusError itself for
// an offset it does not model (never a silent "reads 0").
class MmioHandler {
public:
    virtual ~MmioHandler() = default;
    virtual std::uint16_t read16(std::uint32_t offset) = 0;
    virtual std::uint32_t read32(std::uint32_t offset) = 0;
    virtual void write16(std::uint32_t offset, std::uint16_t value) = 0;
    virtual void write32(std::uint32_t offset, std::uint32_t value) = 0;
};

// The write-gather pipe at physical 0x0C008000 (the graphics FIFO's write port, Task 6). Any
// write in its page lands here as its bytes in big-endian (memory) order, whatever its size;
// the pipe groups them in 32-byte bursts itself. Reads throw.
class WriteGatherPipe {
public:
    virtual ~WriteGatherPipe() = default;
    virtual void write(std::span<const std::uint8_t> bytes) = 0;
};

}  // namespace ezy::gc
