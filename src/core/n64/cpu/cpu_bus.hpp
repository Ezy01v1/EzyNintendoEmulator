#pragma once

#include <cstdint>

namespace ezy::n64 {

// What the VR4300 sees of the system: physical, big-endian memory accesses. Address
// translation (kseg0/kseg1/TLB) happens in the CPU, so tests can use a flat array.
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

    // Extra CPU cycles for one uncached access to this physical address.
    [[nodiscard]] virtual std::uint32_t uncached_cycles(std::uint32_t paddr) const = 0;
    // The RCP's interrupt line (MI), wired to Cause.IP2.
    [[nodiscard]] virtual bool interrupt_line() const = 0;
};

}  // namespace ezy::n64
