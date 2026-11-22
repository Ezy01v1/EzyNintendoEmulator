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
// Registers (offsets from the block, all 32 bits; a 16-bit READ takes one half of one, a
// 16-bit write throws, as in Dolphin where it is an invalid access):
//   0x00 INTSR   interrupt cause. Writing 1 to a bit clears it (Dolphin: cause &= ~val), any
//                bit; a live cause is set again by its peripheral's next Pi::set_cause(bit,
//                true), so a peripheral whose condition still holds must call it again.
//                Starts as RST_BUTTON (bit 16, reset switch released) | VI, as Dolphin's Init().
//   0x04 INTMR   interrupt mask, bits 0-13 (deviation: Dolphin stores all bits; bit 16 is the
//                switch state, not an interrupt, so it is not maskable here).
//   0x0C/10/14   the CP FIFO's base, end and write pointer (Task 6 uses them): masked with
//                0x1FFFFFE0, physical and 32-byte aligned, as Dolphin does (its
//                GetPhysicalAddressMask(GC) & 0xFFFFFFE0).
//   0x18         FIFO reset: accepted, no effect (Dolphin resets the pipe and the CP FIFO:
//                Task 6).
//   0x24         reset code: stored.
//   0x2C         the Flipper revision, reads kRevision = FLIPPER_REV_C (0x246500B1 in
//                Dolphin's ProcessorInterface.cpp); read-only.
// Any other offset throws BusError. (Sources: YAGCD "Processor Interface"; Dolphin
// Core/HW/ProcessorInterface.cpp, read on 2026-11-22.)
//
// Tasks 5-6: every peripheral raises its cause with Pi::set_cause(Pi::kDi, level) and so on.
class Pi final : public MmioHandler {
public:
    // Cause bits (YAGCD / Dolphin's INT_CAUSE_*).
    static constexpr unsigned kGpuError = 0, kRsw = 1, kDi = 2, kSi = 3, kExi = 4, kAi = 5, kDsp = 6, kMi = 7,
                              kVi = 8, kPeToken = 9, kPeFinish = 10, kCp = 11, kDebug = 12, kHsp = 13,
                              kRswState = 16;
    static constexpr std::uint32_t kMaskableBits = 0x3FFF;  // bits 0-13
    static constexpr std::uint32_t kRevision = 0x246500B1;  // FLIPPER_REV_C
    static constexpr std::uint32_t kFifoMask = 0x1FFFFFE0;
    static constexpr std::size_t kCauseCount = 17;  // bit numbers 0..16, for the per-cause counters

    // Called with the new level whenever (cause & mask) != 0 changes (the Gekko's external
    // interrupt line).
    using LineSink = std::function<void(bool)>;
    void set_line_sink(LineSink sink) { sink_ = std::move(sink); }

    // A peripheral raises (level true) or clears one cause (0-13; anything else throws
    // std::invalid_argument). A rising edge is counted for Stats.
    void set_cause(unsigned bit, bool level);

    [[nodiscard]] std::uint32_t cause() const { return cause_; }
    [[nodiscard]] std::uint32_t mask() const { return mask_; }
    [[nodiscard]] bool line() const { return line_; }
    [[nodiscard]] std::uint64_t interrupts(unsigned bit) const { return counts_.at(bit); }

    [[nodiscard]] std::uint32_t fifo_base() const { return fifo_base_; }
    [[nodiscard]] std::uint32_t fifo_end() const { return fifo_end_; }
    [[nodiscard]] std::uint32_t fifo_write_pointer() const { return fifo_wptr_; }
    // The write-gather pipe advances the pointer (Task 6).
    void set_fifo_write_pointer(std::uint32_t value) { fifo_wptr_ = value & kFifoMask; }
    [[nodiscard]] std::uint32_t reset_code() const { return reset_code_; }

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    void update_line();

    std::uint32_t cause_ = (1u << kRswState) | (1u << kVi);  // Dolphin's Init(): RST_BUTTON | VI
    std::uint32_t mask_ = 0;
    std::uint32_t fifo_base_ = 0, fifo_end_ = 0, fifo_wptr_ = 0;
    std::uint32_t reset_code_ = 0;
    bool line_ = false;
    std::array<std::uint64_t, kCauseCount> counts_{};
    LineSink sink_;
};

}  // namespace ezy::gc
