#pragma once

#include <cstdint>
#include <vector>

#include "n64/cpu/cpu_bus.hpp"

// 8 MB of big-endian memory at every physical address (wrapping at 8 MB). No penalties.
struct FlatBus final : ezy::n64::CpuBus {
    static constexpr std::uint32_t kSize = 8 * 1024 * 1024;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(kSize, 0);
    bool irq = false;
    std::uint32_t uncached_penalty = 0;

    [[nodiscard]] std::uint8_t& at(std::uint32_t paddr) { return mem[paddr % kSize]; }

    std::uint8_t read8(std::uint32_t paddr) override { return at(paddr); }
    std::uint16_t read16(std::uint32_t paddr) override {
        return static_cast<std::uint16_t>((read8(paddr) << 8) | read8(paddr + 1));
    }
    std::uint32_t read32(std::uint32_t paddr) override {
        return (static_cast<std::uint32_t>(read16(paddr)) << 16) | read16(paddr + 2);
    }
    std::uint64_t read64(std::uint32_t paddr) override {
        return (static_cast<std::uint64_t>(read32(paddr)) << 32) | read32(paddr + 4);
    }
    void write8(std::uint32_t paddr, std::uint8_t value) override { at(paddr) = value; }
    void write16(std::uint32_t paddr, std::uint16_t value) override {
        write8(paddr, static_cast<std::uint8_t>(value >> 8));
        write8(paddr + 1, static_cast<std::uint8_t>(value));
    }
    void write32(std::uint32_t paddr, std::uint32_t value) override {
        write16(paddr, static_cast<std::uint16_t>(value >> 16));
        write16(paddr + 2, static_cast<std::uint16_t>(value));
    }
    void write64(std::uint32_t paddr, std::uint64_t value) override {
        write32(paddr, static_cast<std::uint32_t>(value >> 32));
        write32(paddr + 4, static_cast<std::uint32_t>(value));
    }
    [[nodiscard]] std::uint32_t uncached_cycles(std::uint32_t) const override { return uncached_penalty; }
    [[nodiscard]] bool interrupt_line() const override { return irq; }
};
