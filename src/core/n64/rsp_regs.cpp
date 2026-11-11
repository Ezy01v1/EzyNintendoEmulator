#include "n64/rsp_regs.hpp"

#include <atomic>
#include <new>

namespace ezy::n64 {

namespace { std::atomic<RspMode> selected_mode{RspMode::Interpreter}; }
RspMode default_rsp_mode() { return selected_mode.load(std::memory_order_relaxed); }
void set_default_rsp_mode(RspMode mode) { selected_mode.store(mode,std::memory_order_relaxed); }
RspRegs::RspRegs(RspMode mode) {
    if(mode==RspMode::Jit) {
        try { jit_=std::make_unique<RspJit>(); }
        catch(const std::bad_alloc&) { /* Interpreter remains usable. */ }
    }
}
RspRegs::~RspRegs()=default;

void RspRegs::jit_vector(void* opaque,std::uint32_t word) noexcept {
    auto& rsp=*static_cast<RspRegs*>(opaque);
    if((word>>26)==0x12) rsp.vector_.execute(word,rsp.gpr_[(word>>16)&31]);
    else rsp.vector_.access_memory(word,rsp.gpr_[(word>>21)&31],std::span(rsp.mem_).first<0x1000>());
}

void RspRegs::write_memory(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    bool imem = false;
    for (const auto byte : bytes) {
        imem |= (address & 0x1000) != 0;
        mem_[address & 0x1FFF] = byte;
        ++address;
    }
    if (imem) invalidate_jit();
}

namespace {

constexpr std::uint32_t kDmaBusy = 1U << 2, kDmaFull = 1U << 3;
constexpr std::uint32_t kLengthMask = 0xFF8, kCountMask = 0xFF000, kSkipMask = 0xFF800000;
constexpr std::uint32_t kDramMask = 0xFFFFFF;

}  // namespace

std::uint32_t RspRegs::read_sp(std::uint32_t offset) {
    const std::uint32_t reg = offset & 0x1F;
    if (offset >= 0x40000) return reg == 0 ? sp_pc_ : 0;
    const bool busy = dma_.direction != DmaDirection::Idle;
    const bool full = pending_.direction != DmaDirection::Idle;
    switch (reg) {
        case 0x00: return dma_.sp_address;
        case 0x04: return dma_.dram_address;
        case 0x08: case 0x0C: return dma_.length;
        case 0x10: return sp_status_ | (busy ? kDmaBusy : 0) | (full ? kDmaFull : 0);
        case 0x14: return full;
        case 0x18: return busy;
        case 0x1C: {          // SP_SEMAPHORE: reading takes it
            const std::uint32_t value = semaphore_;
            semaphore_ = 1;
            return value;
        }
        default: return 0;
    }
}

void RspRegs::write_sp(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler, std::uint64_t when) {
    const std::uint32_t reg = offset & 0x1F;
    if (offset >= 0x40000) {
        if (reg == 0) {
            sp_pc_ = value & kPcMask;
            next_pc_ = (sp_pc_ + 4) & kPcMask;
        }
        return;
    }
    switch (reg) {
        case 0x00: pending_.sp_address = value & 0x1FF8; break;
        case 0x04: pending_.dram_address = value & (kDramMask & ~7U); break;
        case 0x08: case 0x0C:
            pending_.length = value & (kLengthMask | kCountMask | kSkipMask);
            pending_.direction = reg == 0x08 ? DmaDirection::ToSp : DmaDirection::ToDram;
            start_dma(scheduler, when);
            break;
        case 0x10: write_status(value, mi, scheduler, when); break;
        case 0x1C: semaphore_ = 0; break;
        default: break;
    }
}

void RspRegs::write_status(std::uint32_t value, Mi& mi, Scheduler& scheduler, std::uint64_t when) {
    const bool was_halted = (sp_status_ & kHalt) != 0;
    const auto apply_pair = [this, value](unsigned shift, std::uint32_t flag) {
        const std::uint32_t command = (value >> shift) & 3;
        if (command == 1) sp_status_ &= ~flag;
        if (command == 2) sp_status_ |= flag;
    };
    apply_pair(0, kHalt);
    if (value & (1U << 2)) sp_status_ &= ~kBroke;
    const std::uint32_t interrupt = (value >> 3) & 3;
    if (interrupt == 1) mi.clear(MiInterrupt::Sp);
    if (interrupt == 2) mi.raise(MiInterrupt::Sp);
    apply_pair(5, kSingleStep);
    apply_pair(7, kInterruptOnBreak);
    for (unsigned signal = 0; signal < 8; ++signal) apply_pair(9 + 2 * signal, 1U << (7 + signal));

    if (sp_status_ & kHalt) {
        scheduler.cancel(EventType::RspStep);
    } else if (was_halted) {
        short_step_ = true;
        scheduler.schedule(EventType::RspStep, when + 2);
    }
}

void RspRegs::start_dma(Scheduler& scheduler, std::uint64_t when) {
    if (dma_.direction != DmaDirection::Idle || pending_.direction == DmaDirection::Idle) return;
    dma_ = pending_;
    pending_.direction = DmaDirection::Idle;
    schedule_dma_row(scheduler, when);
}

void RspRegs::schedule_dma_row(Scheduler& scheduler, std::uint64_t when) const {
    // Approximate bus timing: three RSP cycles per 8 bytes, rounded up to CPU cycles.
    const std::uint32_t beats = ((dma_.length & kLengthMask) >> 3) + 1;
    scheduler.schedule(EventType::SpDma, when + (beats * 9 + 1) / 2);
}

void RspRegs::finish_dma_row(Scheduler& scheduler, std::span<std::uint8_t> rdram, std::uint64_t when) {
    if (dma_.direction == DmaDirection::Idle) return;
    const std::uint32_t bytes = (dma_.length & kLengthMask) + 8;
    const std::uint32_t bank = dma_.sp_address & 0x1000;
    if (dma_.direction == DmaDirection::ToSp && bank != 0) invalidate_jit();
    for (std::uint32_t i = 0; i < bytes; ++i) {
        std::uint8_t& sp = mem_[bank | ((dma_.sp_address + i) & 0xFFF)];
        const std::uint32_t dram = (dma_.dram_address + i) & kDramMask;
        if (dma_.direction == DmaDirection::ToSp) {
            sp = dram < rdram.size() ? rdram[dram] : 0;
        } else if (dram < rdram.size()) {
            rdram[dram] = sp;
        }
    }
    dma_.sp_address = bank | ((dma_.sp_address + bytes) & 0xFFF);
    dma_.dram_address = (dma_.dram_address + bytes) & kDramMask;
    if (dma_.length & kCountMask) {
        dma_.length -= 1U << 12;
        dma_.dram_address = (dma_.dram_address + (dma_.length >> 20)) & kDramMask;
        schedule_dma_row(scheduler, when);
    } else {
        dma_.length = (dma_.length & kSkipMask) | kLengthMask;
        dma_.direction = DmaDirection::Idle;
        start_dma(scheduler, when);
    }
}

}  // namespace ezy::n64
