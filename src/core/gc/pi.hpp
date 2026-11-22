#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "gc/mmio.hpp"

namespace ezy::gc {

// The processor interface at 0x0C003000 (YAGCD, "Processor Interface"; Dolphin's
// Core/HW/ProcessorInterface.cpp). It gathers the interrupt causes the peripherals raise and
// drives the Gekko's external interrupt line:  line = (cause & mask) != 0.
//
// Registers (offsets from the block, all 32 bits; a 16-bit access takes one half of one):
//   0x00 INTSR   interrupt cause. Reads the live causes plus bit 16 (reset-switch state, 1 =
//                button released, as the SDK's OSGetResetButtonState reads it). Writing 1 to
//                bit 0 (GPU error) or bit 1 (reset switch), the two latched causes, clears
//                them; every other cause is the live level of its peripheral, which the
//                peripheral clears (Pi::set_cause(bit, false)), so writing it does nothing.
//   0x04 INTMR   interrupt mask, bits 0-13 (a bit above 13 is not stored).
//   0x0C/10/14   the CP FIFO's base, end and write pointer (Task 6 uses them): stored as written.
//   0x18         FIFO reset: accepted, no effect.
//   0x24         reset code: stored.
//   0x2C         console revision, reads kRevision (Dolphin returns 0x20000000, and the SDK's
//                OSGetConsoleType reads its top nibble: 2 = retail hardware).
// Any other offset throws BusError.
//
// Tasks 5-6: every peripheral raises its cause with Pi::set_cause(Pi::kDi, level) and so on.
class Pi final : public MmioHandler {
public:
    // Cause bits (YAGCD / Dolphin's INT_CAUSE_*).
    static constexpr unsigned kGpuError = 0, kRsw = 1, kDi = 2, kSi = 3, kExi = 4, kAi = 5, kDsp = 6, kMi = 7,
                              kVi = 8, kPeToken = 9, kPeFinish = 10, kCp = 11, kDebug = 12, kHsp = 13,
                              kRswState = 16;
    static constexpr std::uint32_t kMaskableBits = 0x3FFF;  // bits 0-13
    static constexpr std::uint32_t kRevision = 0x20000000;
    static constexpr std::size_t kCauseCount = 17;  // bit numbers 0..16, for the per-cause counters

    // Called with the new level whenever (cause & mask) != 0 changes (the Gekko's external
    // interrupt line).
    using LineSink = std::function<void(bool)>;
    void set_line_sink(LineSink sink) { sink_ = std::move(sink); }

    // A peripheral raises (level true) or clears one cause (0-13; anything else throws
    // std::invalid_argument). A rising edge is counted for Stats.
    void set_cause(unsigned bit, bool level);

    [[nodiscard]] std::uint32_t cause() const { return cause_ | (1u << kRswState); }
    [[nodiscard]] std::uint32_t mask() const { return mask_; }
    [[nodiscard]] bool line() const { return line_; }
    [[nodiscard]] std::uint64_t interrupts(unsigned bit) const { return counts_.at(bit); }

    [[nodiscard]] std::uint32_t fifo_base() const { return fifo_base_; }
    [[nodiscard]] std::uint32_t fifo_end() const { return fifo_end_; }
    [[nodiscard]] std::uint32_t fifo_write_pointer() const { return fifo_wptr_; }
    // The write-gather pipe advances the pointer (Task 6).
    void set_fifo_write_pointer(std::uint32_t value) { fifo_wptr_ = value; }
    [[nodiscard]] std::uint32_t reset_code() const { return reset_code_; }

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    void update_line();

    std::uint32_t cause_ = 0;  // bits 0-13
    std::uint32_t mask_ = 0;
    std::uint32_t fifo_base_ = 0, fifo_end_ = 0, fifo_wptr_ = 0;
    std::uint32_t reset_code_ = 0;
    bool line_ = false;
    std::array<std::uint64_t, kCauseCount> counts_{};
    LineSink sink_;
};

}  // namespace ezy::gc
