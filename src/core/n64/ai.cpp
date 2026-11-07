#include "n64/ai.hpp"

namespace ezy::n64 {

namespace {

constexpr double kCpuHz = 93'750'000.0;
constexpr double kViClockNtsc = 48'681'812.0;
constexpr double kViClockPal = 49'656'530.0;
constexpr std::uint32_t kStatusFull = 1U << 31, kStatusBusy = 1U << 30;

}  // namespace

int Ai::sample_rate() const {
    const double clock = region_ == Region::Pal ? kViClockPal : kViClockNtsc;
    return static_cast<int>(clock / (static_cast<double>(dacrate_ & 0x3FFF) + 1.0));
}

std::uint32_t Ai::read(std::uint32_t offset) const {
    switch (offset & 0x1F) {
        case 0x04: return count_ > 0 ? fifo_[0].length : 0;  // AI_LEN (whole buffer, not the remainder)
        case 0x0C: return (count_ >= 2 ? kStatusFull | 1U : 0U) | (count_ >= 1 ? kStatusBusy : 0U);
        default: return 0;
    }
}

void Ai::write(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler,
               std::span<const std::uint8_t> rdram) {
    switch (offset & 0x1F) {
        case 0x00: dram_addr_ = value & 0x00FFFFF8; return;
        case 0x04: {
            const Buffer buffer{dram_addr_, value & 0x3FFF8};
            if (!(control_ & 1) || buffer.length == 0 || count_ >= 2) return;
            if (count_ == 0) {
                fifo_[0] = buffer;
                count_ = 1;
                start(buffer, mi, scheduler, rdram);
            } else {
                fifo_[1] = buffer;
                count_ = 2;
            }
            return;
        }
        case 0x08: control_ = value & 1; return;
        case 0x0C: mi.clear(MiInterrupt::Ai); return;  // any write acknowledges
        case 0x10: dacrate_ = value & 0x3FFF; return;
        case 0x14: bitrate_ = value & 0xF; return;
        default: return;
    }
}

void Ai::start(const Buffer& buffer, Mi& mi, Scheduler& scheduler, std::span<const std::uint8_t> rdram) {
    mi.raise(MiInterrupt::Ai);
    const std::uint32_t frames = buffer.length / 4;
    for (std::uint32_t i = 0; i < frames * 2; ++i) {
        const std::size_t a = buffer.addr + i * 2;
        const auto sample = a + 1 < rdram.size() ? static_cast<std::int16_t>((rdram[a] << 8) | rdram[a + 1]) : 0;
        samples_.push_back(static_cast<float>(sample) / 32768.0f);
    }
    const double cycles = static_cast<double>(frames) * kCpuHz / (static_cast<double>(sample_rate()));
    scheduler.schedule_in(EventType::AiDma, static_cast<std::uint64_t>(cycles));
}

void Ai::on_dma_end(Mi& mi, Scheduler& scheduler, std::span<const std::uint8_t> rdram) {
    if (count_ == 0) return;
    if (count_ == 2) {
        fifo_[0] = fifo_[1];
        count_ = 1;
        start(fifo_[0], mi, scheduler, rdram);
    } else {
        count_ = 0;
    }
}

}  // namespace ezy::n64
