#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "n64/cartridge.hpp"
#include "n64/mi.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Peripheral Interface: DMA between RDRAM and the cartridge bus (registers at 0x04600000,
// n64brew "Peripheral Interface"). The copy happens at once; PI stays busy until the
// PiDma event, which raises the PI interrupt.
class Pi {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
               const Cartridge& cart);
    void finish_dma(Mi& mi);
    [[nodiscard]] bool busy() const { return busy_; }

    template <class Ar>
    void serialize(Ar& ar) {
        ar(dram_addr_), ar(cart_addr_), ar(rd_len_), ar(wr_len_), ar(busy_), ar(interrupt_), ar(error_), ar(domain_);
    }

private:
    void copy_to_rdram(std::uint32_t length, std::span<std::uint8_t> rdram, const Cartridge& cart);
    // Duration in CPU cycles of a DMA of `length` bytes on the domain of `cart_addr`.
    [[nodiscard]] std::uint64_t dma_cycles(std::uint32_t cart_addr, std::uint32_t length) const;

    std::uint32_t dram_addr_ = 0, cart_addr_ = 0, rd_len_ = 0x7F, wr_len_ = 0x7F;
    bool busy_ = false, interrupt_ = false, error_ = false;
    std::uint8_t pad_[1] = {};
    // LAT, PWD, PGS, RLS for domain 1 then domain 2.
    std::array<std::uint32_t, 8> domain_{};
};

}  // namespace ezy::n64
