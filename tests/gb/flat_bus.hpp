#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>

#include "gb/cpu_bus.hpp"

namespace ezy::gb::test {

// 64 KiB of plain RAM with an M-cycle counter: lets us test the CPU without the real
// memory map. Interrupts are driven directly through `ie` / `if_`.
class FlatBus final : public CpuBus {
public:
    std::array<std::uint8_t, 0x10000> memory{};
    std::uint8_t ie = 0;
    std::uint8_t if_ = 0;
    int m_cycles = 0;

    std::uint8_t read(std::uint16_t addr) override {
        ++m_cycles;
        return memory[addr];
    }
    void write(std::uint16_t addr, std::uint8_t value) override {
        ++m_cycles;
        memory[addr] = value;
        if (addr == 0xFFFF) ie = value;  // IE is a register, as on the real bus
    }
    void tick() override { ++m_cycles; }

    [[nodiscard]] std::uint8_t pending_interrupts() const override { return ie & if_ & 0x1F; }
    void acknowledge_interrupt(std::uint8_t bit) override { if_ &= static_cast<std::uint8_t>(~bit); }

    void load(std::uint16_t addr, std::initializer_list<std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes) memory[addr++] = b;
    }
};

}  // namespace ezy::gb::test
