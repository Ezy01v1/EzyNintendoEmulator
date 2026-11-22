#pragma once

#include <cstdint>

namespace ezy::gc {

// What the Gekko sees of the GameCube: physical, big-endian memory accesses (values are in
// host order). BAT translation happens in the CPU, so tests can use a flat array. The CPU
// passes word, halfword and doubleword accesses at their natural alignment only; it splits
// unaligned ones into bytes. An address with nothing behind it is the bus's to report
// (by throwing GekkoError).
class CpuBus {
public:
    virtual ~CpuBus() = default;

    virtual std::uint8_t read8(std::uint32_t paddr) = 0;
    virtual std::uint16_t read16(std::uint32_t paddr) = 0;
    virtual std::uint32_t read32(std::uint32_t paddr) = 0;
    virtual std::uint64_t read64(std::uint32_t paddr) = 0;
    virtual void write8(std::uint32_t paddr, std::uint8_t value) = 0;
    virtual void write16(std::uint32_t paddr, std::uint16_t value) = 0;
    virtual void write32(std::uint32_t paddr, std::uint32_t value) = 0;
    virtual void write64(std::uint32_t paddr, std::uint64_t value) = 0;
};

}  // namespace ezy::gc
