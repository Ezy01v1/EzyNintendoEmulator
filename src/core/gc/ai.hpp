#pragma once

#include <cstdint>

#include "gc/mmio.hpp"

namespace ezy::gc {

class Pi;
class Scheduler;

// The audio interface at 0x0C006C00 (design, "Periféricos": AI). Offsets and bit layouts from
// Dolphin's Core/HW/AudioInterface.h/.cpp (master, read on 2026-11-23) and YAGCD's "Audio
// Interface". Four 32-bit registers (a 16-bit access throws, as does any other offset):
//   0x00 AICR   PSTAT 0 (the sample counter runs), AISFR 1 (streaming rate: 0 = 32 kHz,
//               1 = 48 kHz), AIINTMSK 2, AIINT 3 (write 1 clears), AIINTVLD 4 (stored; Dolphin
//               only logs it), SCRESET 5 (write 1: the counter restarts at 0; reads 0),
//               AIDFR 6 (the DSP's sample rate for the AI DMA: 0 = 48 kHz, 1 = 32 kHz; the
//               SDK's AISetDSPSampleRate(32 kHz) sets it).
//   0x04 AIVR   the streaming volume (left bits 0-7, right 8-15): stored, no effect.
//   0x08 AISCNT the streaming sample counter: counts at the AISFR rate while PSTAT is set;
//               writable (as Dolphin).
//   0x0C AIIT   interrupt timing: when the counter reaches it, AIINT rises.
// AI cause to the PI: AIINT & AIINTMSK.
//
// Disc audio streaming is off: no samples are mixed, but the counter runs, as the SDK's
// __AI_SRC_INIT waits on it to change. The rates are the nominal 32,000 and 48,000 Hz (the
// hardware's are 54 MHz / 1686 and / 1124, about 32,029 and 48,043 Hz: Dolphin's divisors).
class Ai final : public MmioHandler {
public:
    static constexpr std::uint64_t kCpuHz = 486'000'000;

    Ai(Scheduler& scheduler, Pi& pi);

    // The rate the DSP's AI DMA plays at (AIDFR), in Hz.
    [[nodiscard]] unsigned dma_sample_rate() const;

    // Handles EventType::AiSample: the counter has reached AIIT.
    void on_event();

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    [[nodiscard]] unsigned stream_rate() const;
    [[nodiscard]] std::uint32_t samples() const;  // the counter now
    void rebase();                                // folds the elapsed samples into the base
    void schedule_irq();
    void update_irq();

    Scheduler& sched_;
    Pi& pi_;
    std::uint32_t control_ = 0, volume_ = 0, timing_ = 0;
    std::uint32_t count_base_ = 0;   // the counter at base_time_
    std::uint64_t base_time_ = 0;
};

}  // namespace ezy::gc
