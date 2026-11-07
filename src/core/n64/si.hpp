#pragma once

#include <cstdint>
#include <span>

#include "n64/mi.hpp"
#include "n64/pif.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Serial Interface: 64-byte DMA between RDRAM and PIF RAM (registers at 0x04800000,
// n64brew "Serial Interface"). The copy happens at once; SI stays busy until the SiDma
// event, which raises the SI interrupt.
class Si {
public:
    // Approximate: a 64-byte transfer over the PIF's serial bus takes tens of
    // microseconds; none of our test ROMs measure it.
    static constexpr std::uint64_t kDmaCycles = 6000;

    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler, std::span<std::uint8_t> rdram,
               Pif& pif);
    void finish_dma(Mi& mi);

    template <class Ar>
    void serialize(Ar& ar) {
        ar(dram_addr_), ar(busy_), ar(interrupt_);
    }

private:
    std::uint32_t dram_addr_ = 0;
    bool busy_ = false, interrupt_ = false;
};

}  // namespace ezy::n64
