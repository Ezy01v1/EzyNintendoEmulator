#include "gc/dsp.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "gc/ai.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kBase = Memory::mmio_block_base(MmioBlock::Dsp);

// Control register bits (Dolphin's UDSPControl).
constexpr std::uint16_t kReset = 0x001, kAssertInt = 0x002, kHalt = 0x004, kAid = 0x008, kAidMask = 0x010,
                        kAram = 0x020, kAramMask = 0x040, kDspInt = 0x080, kDspMask = 0x100, kDmaBusy = 0x200,
                        kInitCode = 0x400, kInit = 0x800, kPadding = 0xF000;
constexpr std::uint16_t kInterrupts = kAid | kAram | kDspInt, kMasks = kAidMask | kAramMask | kDspMask;
constexpr std::uint64_t kInitCodeCycles = 130 * 12;  // 130 time-base ticks (Dolphin's DSPHLE)

// Address registers: the high half keeps 10 bits, the low half drops the low 5 (Dolphin's
// WMASK_* for AR_DMA_MMADDR/ARADDR and AUDIO_DMA_START).
constexpr std::uint16_t kHighMask = 0x03FF, kLowMask = 0xFFE0;
// The other write masks (Dolphin's WMASK_AR_INFO, WMASK_AR_REFRESH, and WMASK_AR_HI_RESTRICT |
// WMASK_AR_CNT_DIR_BIT for the count's high half). AR_MODE has WMASK_NONE: writes keep it.
constexpr std::uint16_t kArInfoMask = 0x007F, kArRefreshMask = 0x07FF, kCountHighMask = 0x83FF;
constexpr std::uint32_t kAddressMask = 0x03FFFFE0;
constexpr std::uint32_t kToMainMemory = 0x80000000;
constexpr std::uint16_t kAiEnable = 0x8000, kAiBlocks = 0x7FFF;

// The DSP programs' mails (Dolphin's ROMUCode, INITUCode).
constexpr std::uint32_t kRomReady = 0x8071FEED, kInitCodeMail = 0x80544348;
constexpr std::uint32_t kUploadPrefix = 0x80F30000, kIramMmem = 0x80F3A001, kIramLength = 0x80F3A002,
                        kDramLength = 0x80F3B002, kIramAddress = 0x80F3C002, kStartVector = 0x80F3D001;

std::string hex32(std::uint32_t value) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%08X", value);
    return b;
}

[[noreturn]] void unmodeled(const std::string& what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(what + " of the unmodeled DSP interface register at offset 0x" + b, kBase + offset);
}

void set_high(std::uint32_t& reg, std::uint16_t value) { reg = (reg & 0xFFFF) | (std::uint32_t{value} << 16); }
void set_low(std::uint32_t& reg, std::uint16_t value) { reg = (reg & 0xFFFF0000) | value; }
}  // namespace

Dsp::Dsp(Scheduler& scheduler, Pi& pi, std::span<std::uint8_t> mem1, const Ai& ai)
    : sched_(scheduler),
      pi_(pi),
      mem1_(mem1),
      ai_(ai),
      aram_(kAramSize, 0),
      control_(kHalt | kInit),
      zelda_([this](std::uint32_t mail, bool interrupt) { push_mail(mail, interrupt); }) {
    start_program(Program::Rom);
}

bool Dsp::halted() const { return (control_ & kHalt) != 0; }

std::uint32_t Dsp::microcode_crc(std::span<const std::uint8_t> image) {
    std::uint32_t crc = 0;
    for (const std::uint8_t byte : image) {
        crc ^= byte;
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

void Dsp::raise_dsp_interrupt() {
    control_ |= kDspInt;
    update_irq();
}

void Dsp::push_mail(std::uint32_t mail, bool interrupt) {
    if (interrupt) {
        if (mails_out_.empty()) raise_dsp_interrupt();
        else mails_out_.back().interrupt_after_read = true;
    }
    mails_out_.push_back({mail, false});
}

void Dsp::update_irq() {
    // Each interrupt's mask is the bit to its left (Dolphin's UpdateInterrupts).
    pi_.set_cause(Pi::kDsp, ((control_ >> 1) & control_ & kInterrupts) != 0);
}

void Dsp::start_program(Program program) {
    program_ = program;
    mails_out_.clear();  // Dolphin's SetUCode: ClearPending()
    rom_parameter_ = 0;
    if (program == Program::Rom) push_mail(kRomReady);
    if (program == Program::InitCode) push_mail(kInitCodeMail);
}

void Dsp::boot_microcode() {
    if (std::uint64_t{upload_.iram_mmem_address} + upload_.iram_length > mem1_.size()) {
        throw BusError("DSP microcode IRAM image at " + hex32(upload_.iram_mmem_address) + " runs past MEM1",
                       kBase + 0x02);
    }
    upload_.crc = microcode_crc(mem1_.subspan(upload_.iram_mmem_address, upload_.iram_length));
    if (upload_.crc != ZeldaMicrocode::kCrc) {
        throw BusError("DSP microcode with CRC " + hex32(upload_.crc) + " is not modeled", kBase + 0x02);
    }
    microcode_ = upload_;
    start_program(Program::Microcode);  // Dolphin's BootUCode
    zelda_.start();
}

void Dsp::receive_mail(std::uint32_t mail) {
    ++mails_;
    mail_log_.push_back(mail);
    if (mail_log_.size() > kMailLogSize) mail_log_.pop_front();
    if (program_ == Program::Microcode) {
        zelda_.receive(mail);
        return;
    }
    if (program_ != Program::Rom) return;  // ROM init code: it never reads the mailbox (Dolphin: HLE ignores it too), hardware-faithful
    if (rom_parameter_ == 0) {
        if ((mail & 0xFFFF0000) == kUploadPrefix) {
            rom_parameter_ = mail;
        } else {
            push_mail(0xFEEE0000 | (mail & 0xFFFF));
        }
        return;
    }
    switch (rom_parameter_) {
        case kIramMmem: upload_.iram_mmem_address = mail; break;
        case kIramLength: upload_.iram_length = static_cast<std::uint16_t>(mail); break;
        case kDramLength: upload_.dram_length = static_cast<std::uint16_t>(mail); break;
        case kIramAddress: upload_.iram_address = static_cast<std::uint16_t>(mail); break;
        case kStartVector:
            upload_.init_vector = static_cast<std::uint16_t>(mail);
            rom_parameter_ = 0;
            boot_microcode();
            return;
        default: break;  // an unknown upload parameter: the ROM loader drops the word (Dolphin's ROMUCode); hardware behavior, not an omission
    }
    rom_parameter_ = 0;
}

void Dsp::write_control(std::uint16_t value) {
    if (value & kPadding) unmodeled("write of " + hex32(value) + " (bits 12-15)", 0x0A);
    // PIINT (kAssertInt), the CPU interrupting the DSP (the SDK's DSPAssertInt, after the first
    // mail of a command): a running microcode takes it at once, as the HLE takes mails, so it
    // is not stored and reads 0, as on hardware once the DSP has taken it. No effect otherwise:
    // the HLE does not need it to read the mails. With the ROM or the init code running it is
    // not modeled.
    if ((value & kAssertInt) && program_ != Program::Microcode) {
        unmodeled("CPU->DSP interrupt (PIINT, write " + hex32(value) + ") with no microcode running", 0x0A);
    }
    const std::uint16_t old = control_;
    std::uint16_t c = static_cast<std::uint16_t>(control_ & ~(value & kInterrupts));
    constexpr std::uint16_t kStored = kMasks | kHalt | kInit | kInitCode;
    c = static_cast<std::uint16_t>((c & ~kStored) | (value & kStored));
    control_ = c;
    if (value & kReset) {  // Dolphin: SetUCode(UCODE_ROM), and the AI DMA control cleared
        start_program(Program::Rom);
        ai_control_ = 0;  // only the control: the blocks left keep their count, as in Dolphin
        sched_.cancel(EventType::AiDmaStart);
        sched_.cancel(EventType::AiDmaBlock);
    }
    if ((old & kInit) && !(value & kInit)) {  // the DSP boots the init code from ARAM
        start_program(Program::InitCode);
        control_ |= kInitCode;
        init_code_done_ = sched_.now() + kInitCodeCycles;
    }
    update_irq();
}

void Dsp::check_aram_dma(std::uint32_t count_register) const {
    const std::uint32_t count = count_register & ~kToMainMemory;
    if (std::uint64_t{ar_mm_} + count > mem1_.size()) {
        throw BusError("ARAM DMA of " + hex32(count) + " bytes at main memory " + hex32(ar_mm_) + " runs past MEM1",
                       kBase + 0x2A);
    }
}

void Dsp::start_aram_dma() {
    check_aram_dma(ar_count_);
    const std::uint32_t count = ar_count_ & ~kToMainMemory;
    // The part below 16 MiB is ARAM; the rest is the empty expansion port. The ARAM pointer
    // is only formed when some of the range is in ARAM (never past its end).
    const std::uint32_t in_aram = ar_ar_ < kAramSize ? std::min(count, kAramSize - ar_ar_) : 0;
    std::uint8_t* const main = mem1_.data() + ar_mm_;
    if (ar_count_ & kToMainMemory) {
        if (in_aram) std::copy_n(aram_.data() + ar_ar_, in_aram, main);
        std::fill_n(main + in_aram, count - in_aram, std::uint8_t{0});
    } else if (in_aram) {
        std::copy_n(main, in_aram, aram_.data() + ar_ar_);
    }
    ar_mm_ = (ar_mm_ + count) & kAddressMask;
    ar_ar_ = (ar_ar_ + count) & kAddressMask;
    ar_count_ &= kToMainMemory;
    control_ |= kDmaBusy;
    sched_.schedule_in(EventType::ArDma, std::uint64_t{count} / 32 * kAramCyclesPer32Bytes);
}

void Dsp::on_aram_dma_done() {
    control_ = static_cast<std::uint16_t>((control_ & ~kDmaBusy) | kAram);
    update_irq();
}

std::uint64_t Dsp::ai_block_cycles() const { return 8 * Ai::kCpuHz / ai_.dma_sample_rate(); }

void Dsp::latch_ai_dma() {
    const std::uint32_t blocks = ai_control_ & kAiBlocks;
    if (std::uint64_t{ai_start_} + std::uint64_t{blocks} * 32 > mem1_.size()) {
        throw BusError("AI DMA of " + std::to_string(blocks) + " blocks at " + hex32(ai_start_) + " runs past MEM1",
                       kBase + 0x36);
    }
    ai_blocks_left_ = static_cast<std::uint16_t>(blocks);
}

void Dsp::on_ai_dma_start() {
    control_ |= kAid;
    update_irq();
}

void Dsp::on_ai_dma_block(std::uint64_t when) {
    if (!(ai_control_ & kAiEnable)) return;
    if (ai_blocks_left_ != 0) --ai_blocks_left_;  // one block played (Dolphin's UpdateAudioDMA)
    if (ai_blocks_left_ == 0) {  // the buffer is done: latch the registers again
        latch_ai_dma();
        control_ |= kAid;
        update_irq();
    }
    sched_.schedule(EventType::AiDmaBlock, when + ai_block_cycles());  // from `when`: no drift
}

std::uint16_t Dsp::read16(std::uint32_t offset) {
    switch (offset) {
        case 0x00: return static_cast<std::uint16_t>(mail_to_dsp_ >> 16);
        case 0x02: return static_cast<std::uint16_t>(mail_to_dsp_);
        case 0x04:
            if (!halted() && !mails_out_.empty()) last_mail_out_ = mails_out_.front().mail;
            return static_cast<std::uint16_t>(last_mail_out_ >> 16);
        case 0x06:
            if (!halted() && !mails_out_.empty()) {
                const OutMail taken = mails_out_.front();
                last_mail_out_ = taken.mail;
                mails_out_.pop_front();
                if (taken.interrupt_after_read) raise_dsp_interrupt();
            }
            last_mail_out_ &= 0x7FFFFFFF;  // read: no longer pending
            return static_cast<std::uint16_t>(last_mail_out_);
        case 0x0A:
            if ((control_ & kInitCode) && sched_.now() >= init_code_done_) control_ &= ~kInitCode;
            return control_;
        case 0x12: return ar_info_;
        case 0x16: return ar_mode_;
        case 0x1A: return ar_refresh_;
        case 0x20: return static_cast<std::uint16_t>(ar_mm_ >> 16);
        case 0x22: return static_cast<std::uint16_t>(ar_mm_);
        case 0x24: return static_cast<std::uint16_t>(ar_ar_ >> 16);
        case 0x26: return static_cast<std::uint16_t>(ar_ar_);
        case 0x28: return static_cast<std::uint16_t>(ar_count_ >> 16);
        case 0x2A: return static_cast<std::uint16_t>(ar_count_);
        case 0x30: return static_cast<std::uint16_t>(ai_start_ >> 16);
        case 0x32: return static_cast<std::uint16_t>(ai_start_);
        case 0x36: return ai_control_;
        case 0x3A: return ai_blocks_left_ > 0 ? static_cast<std::uint16_t>(ai_blocks_left_ - 1) : 0;
        default: unmodeled("read", offset);
    }
}

void Dsp::write16(std::uint32_t offset, std::uint16_t value) {
    switch (offset) {
        case 0x00: set_high(mail_to_dsp_, value); return;
        case 0x02:
            set_low(mail_to_dsp_, value);
            receive_mail(mail_to_dsp_);
            mail_to_dsp_ &= 0x7FFFFFFF;  // taken (Dolphin's HLE: "clear MSB to show that it is progressed")
            return;
        case 0x0A: write_control(value); return;
        // The ARAM controller's setup: accepted, no effect (ARAM is a fixed 16 MiB here).
        case 0x12: ar_info_ = value & kArInfoMask; return;
        case 0x16: return;  // AR_MODE: no bit is writable (WMASK_NONE); it keeps reading 1
        case 0x1A: ar_refresh_ = value & kArRefreshMask; return;
        case 0x20: set_high(ar_mm_, value & kHighMask); return;
        case 0x22: set_low(ar_mm_, value & kLowMask); return;
        case 0x24: set_high(ar_ar_, value & kHighMask); return;
        case 0x26: set_low(ar_ar_, value & kLowMask); return;
        case 0x28: set_high(ar_count_, value & kCountHighMask); return;
        case 0x2A:
            set_low(ar_count_, value & kLowMask);
            start_aram_dma();
            return;
        case 0x30: set_high(ai_start_, value & kHighMask); return;
        case 0x32: set_low(ai_start_, value & kLowMask); return;
        case 0x36: {
            const bool was_enabled = (ai_control_ & kAiEnable) != 0;
            const std::uint16_t old = ai_control_;
            ai_control_ = value;
            if (!was_enabled && (value & kAiEnable)) {  // only a new start latches (Dolphin)
                try {
                    latch_ai_dma();
                } catch (...) {
                    ai_control_ = old;
                    throw;
                }
                sched_.schedule_in(EventType::AiDmaStart, kAiDmaStartCycles);
                sched_.schedule_in(EventType::AiDmaBlock, ai_block_cycles());
            } else if (!(value & kAiEnable)) {
                sched_.cancel(EventType::AiDmaStart);
                sched_.cancel(EventType::AiDmaBlock);
            }
            return;
        }
        case 0x04:
        case 0x06: unmodeled("write (the DSP->CPU mailbox is read-only)", offset);
        default: unmodeled("write", offset);  // includes the read-only blocks left at 0x3A
    }
}

std::uint32_t Dsp::read32(std::uint32_t offset) {
    if (offset & 3) unmodeled("misaligned 32-bit read", offset);
    const std::uint16_t high = read16(offset);
    return (std::uint32_t{high} << 16) | read16(offset + 2);
}

void Dsp::write32(std::uint32_t offset, std::uint32_t value) {
    if (offset & 3) unmodeled("misaligned 32-bit write", offset);
    // A 32-bit write is all or nothing. Of the register pairs, only the ARAM count's can fail
    // on its low half (the DMA it starts runs past MEM1), so that one is checked before the
    // high half lands; every other pair is either always accepted or fails on its high half.
    if (offset == 0x28) {
        const std::uint32_t high = (value >> 16) & kCountHighMask;
        check_aram_dma((high << 16) | (value & kLowMask));
    }
    write16(offset, static_cast<std::uint16_t>(value >> 16));
    write16(offset + 2, static_cast<std::uint16_t>(value));
}

}  // namespace ezy::gc
