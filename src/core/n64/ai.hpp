#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "n64/cartridge.hpp"
#include "n64/mi.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

// Audio Interface (registers at 0x04500000, n64brew "Audio Interface"): plays buffers of
// 16-bit big-endian stereo samples from RDRAM through a two-entry FIFO. A write to AI_LEN
// queues a buffer; a buffer raises the AI interrupt when it *starts* playing, so a game
// refills the FIFO while the previous buffer plays. Sample rate = VI clock / (DACRATE + 1).
class Ai {
public:
    void set_region(Region region) { region_ = region; }

    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler,
               std::span<const std::uint8_t> rdram);
    void on_dma_end(Mi& mi, Scheduler& scheduler, std::span<const std::uint8_t> rdram);

    [[nodiscard]] int sample_rate() const;
    [[nodiscard]] std::span<const float> samples() const { return samples_; }
    void clear_samples() { samples_.clear(); }

    template <class Ar>
    void serialize(Ar& ar) {
        ar(dram_addr_), ar(control_), ar(dacrate_), ar(bitrate_), ar(fifo_), ar(count_), ar(region_);
    }

private:
    struct Buffer {
        std::uint32_t addr = 0;
        std::uint32_t length = 0;
    };
    void start(const Buffer& buffer, Mi& mi, Scheduler& scheduler, std::span<const std::uint8_t> rdram);

    std::uint32_t dram_addr_ = 0, control_ = 0, dacrate_ = 0, bitrate_ = 0;
    std::array<Buffer, 2> fifo_{};  // [0] playing, [1] queued
    std::uint32_t count_ = 0;       // buffers in the FIFO
    Region region_ = Region::Ntsc;
    std::uint8_t pad_[3] = {};
    std::vector<float> samples_;  // produced since the frontend last took them
};

}  // namespace ezy::n64
