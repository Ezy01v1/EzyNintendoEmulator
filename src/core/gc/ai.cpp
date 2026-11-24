#include "gc/ai.hpp"

#include <cstdio>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Ai);

// AICR bits (Dolphin's AICR union).
constexpr std::uint32_t kPstat = 1u << 0, kAisfr = 1u << 1, kAiintMsk = 1u << 2, kAiint = 1u << 3,
                        kAiintVld = 1u << 4, kScReset = 1u << 5, kAidfr = 1u << 6;
constexpr std::uint32_t kStored = kPstat | kAisfr | kAiintMsk | kAiintVld | kAidfr;

[[noreturn]] void unmodeled(const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled audio interface register at offset 0x" + b, kBase + offset);
}
}  // namespace

Ai::Ai(Scheduler& scheduler, Pi& pi) : sched_(scheduler), pi_(pi) {}

unsigned Ai::dma_sample_rate() const { return (control_ & kAidfr) ? 32000 : 48000; }

unsigned Ai::stream_rate() const { return (control_ & kAisfr) ? 48000 : 32000; }

std::uint32_t Ai::samples() const {
    if (!(control_ & kPstat)) return count_base_;
    return count_base_ + static_cast<std::uint32_t>((sched_.now() - base_time_) * stream_rate() / kCpuHz);
}

void Ai::rebase() {
    count_base_ = samples();
    base_time_ = sched_.now();
}

void Ai::schedule_irq() {  // after rebase(): count_base_ is the counter now
    sched_.cancel(EventType::AiSample);
    if (!(control_ & kPstat) || timing_ <= count_base_) return;
    const std::uint64_t ahead = timing_ - count_base_;
    const std::uint64_t rate = stream_rate();
    sched_.schedule(EventType::AiSample, base_time_ + (ahead * kCpuHz + rate - 1) / rate);  // the first cycle it reads AIIT
}

void Ai::on_event() {
    rebase();
    if (count_base_ >= timing_) control_ |= kAiint;
    update_irq();
}

void Ai::update_irq() { pi_.set_cause(Pi::kAi, (control_ & kAiint) && (control_ & kAiintMsk)); }

std::uint32_t Ai::read32(std::uint32_t offset) {
    switch (offset) {
        case 0x00: return control_;
        case 0x04: return volume_;
        case 0x08: return samples();
        case 0x0C: return timing_;
        default: unmodeled("read", offset);
    }
}

void Ai::write32(std::uint32_t offset, std::uint32_t value) {
    switch (offset) {
        case 0x00:
            rebase();  // the samples so far count at the old rate and PSTAT
            control_ = (value & kStored) | (control_ & kAiint);
            if (value & kAiint) control_ &= ~kAiint;
            if (value & kScReset) count_base_ = 0;
            break;
        case 0x04: volume_ = value; return;  // accepted, no effect: no streaming audio is mixed
        case 0x08:
            rebase();
            count_base_ = value;
            break;
        case 0x0C:
            rebase();
            timing_ = value;
            break;
        default: unmodeled("write", offset);
    }
    schedule_irq();
    update_irq();
}

std::uint16_t Ai::read16(std::uint32_t offset) { unmodeled("16-bit read (the register is 32 bits)", offset); }

void Ai::write16(std::uint32_t offset, std::uint16_t) {
    unmodeled("16-bit write (the register is 32 bits)", offset);
}

}  // namespace ezy::gc
