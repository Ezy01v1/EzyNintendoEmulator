#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "gc/mmio.hpp"

namespace ezy::gc {

// The memory interface at 0x0C004000 (YAGCD, "Memory Interface"; Dolphin's
// Core/HW/MemoryInterface.cpp). All registers are 16 bits (a 32-bit access is two halves,
// high half at the lower offset):
//   0x00-0x0F  protection regions 0-3, first and last page: accepted, no effect (protection
//              is not enforced, so no access ever violates it)
//   0x10       protection type: accepted, no effect
//   0x1C       interrupt mask (bits 0-3 regions, bit 4 any): stored
//   0x1E       interrupt cause: read, write 1 clears; nothing raises it
//   0x20       unknown (the SDK's MEMInit clears it): accepted, no effect
//   0x22, 0x24 PROT_ADDR_LO / PROT_ADDR_HI (Dolphin's names; the address of the last
//              violation, which never happens here): both writable, stored
// The profiling counters from 0x32, 0x5A and the rest (0x26 included) throw BusError.
// Dolphin has no logic for the mask/cause pair (plain stored halfwords); the write-1-clears
// cause here is this emulator's model and is never raised by the guest-visible machine.
// MI interrupt -> PI cause 7 (Pi::kMi) when (cause & mask) != 0; the irq sink is that hook.
class Mi final : public MmioHandler {
public:
    using IrqSink = std::function<void(bool)>;
    void set_irq_sink(IrqSink sink) { sink_ = std::move(sink); }

    // Not reachable from the guest today (protection is not enforced); a violation would
    // call this with the region bit (0-3).
    void raise(unsigned region);

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    void update_irq();

    std::array<std::uint16_t, 0x26 / 2> regs_{};  // 0x00-0x25 as halfwords
    std::uint16_t cause_ = 0;
    IrqSink sink_;
    bool irq_ = false;
};

}  // namespace ezy::gc
