#pragma once

#include <cstdint>

namespace ezy::gb {

// What the CPU needs from the outside world. The real Bus implements it; tests use a
// flat 64 KiB memory instead, so the CPU can be validated in isolation.
//
// Trade-off: a virtual call per memory access. At ~1 M accesses/second this is far
// below anything measurable, and it keeps the CPU a plain class instead of a template.
class CpuBus {
public:
    virtual ~CpuBus() = default;

    virtual std::uint8_t read(std::uint16_t addr) = 0;              // costs one M-cycle
    virtual void write(std::uint16_t addr, std::uint8_t value) = 0;  // costs one M-cycle
    virtual void tick() = 0;                                         // internal M-cycle

    // IE & IF, masked to the 5 interrupt bits.
    [[nodiscard]] virtual std::uint8_t pending_interrupts() const = 0;
    virtual void acknowledge_interrupt(std::uint8_t bit) = 0;  // clears the bit in IF
};

}  // namespace ezy::gb
