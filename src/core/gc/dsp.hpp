#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "gc/mmio.hpp"

namespace ezy::gc {

class Ai;
class Pi;
class Scheduler;

// The DSP interface at 0x0C005000 (design, "Periféricos": Interfaz DSP): the two mailboxes,
// the control register, ARAM (16 MiB) and its DMA, the AI DMA, and a minimal HLE of the DSP
// itself. Offsets, bit layouts and timings from Dolphin's Core/HW/DSP.cpp and DSPHLE/
// (DSPHLE.cpp, MailHandler.cpp, UCodes/ROM.cpp, UCodes/INIT.cpp; master, read on 2026-11-23)
// and YAGCD's "DSP Interface". The registers are 16 bits; a 32-bit access at a 4-aligned
// offset is the high half at the offset, then the low half (Dolphin's ReadToSmaller/
// WriteToSmaller). Any offset not listed throws BusError.
//   0x00/0x02 mailbox CPU->DSP, high/low. Writing the low half sends the mail; the HLE takes
//             it at once, so bit 15 of the high half reads 0 again (Dolphin's HLE).
//   0x04/0x06 mailbox DSP->CPU, read-only. A mail is pending while bit 15 of the high half is
//             set; reading the low half takes it (Dolphin's CMailHandler). While the DSP is
//             halted, mails stay queued and the reads repeat the last one, its bit 15 clear.
//   0x0A      control: RES 0 (reset: done at once, reads 0; also stops the AI DMA, as
//             Dolphin: only its control register, the blocks left keep their count), PIINT 1
//             (the CPU interrupts the DSP: part of a microcode's protocol, Task 7, throws),
//             HALT 2, AID 3, AID mask 4, ARINT 5, ARINT mask 6, DSPINT 7,
//             DSPINT mask 8 (the interrupt bits clear when written 1), DMA busy 9 (read-only:
//             an ARAM DMA runs), INITCODE 10, INIT 11. Bits 12-15 throw. Reset value: HALT |
//             INIT (0x0804, Dolphin's DSPHLE::Initialize). DSP cause to the PI: any of the
//             three interrupt bits with its mask (the bit to its left).
//   0x12 AR_INFO (write mask 0x007F), 0x1A AR_REFRESH (0x07FF; 156 at reset): stored, no
//   effect (ARAM is always 16 MiB here). 0x16 AR_MODE reads 1 (the ARAM controller is ready,
//   which ARInit waits on); no bit of it is writable (Dolphin's WMASK_NONE).
//   0x20/0x22 ARAM DMA main-memory address (high masked 0x03FF, low 0xFFE0), 0x24/0x26 the
//             ARAM address (same masks), 0x28/0x2A the count (bit 31: 1 = ARAM -> main
//             memory; the high half keeps 0x83FF, the low half drops bits 0-4). Writing 0x2A
//             (or 0x28 as a 32-bit write, all or nothing) starts the DMA: the copy is
//             immediate, the addresses advance and the count drops to 0; the busy bit stays up
//             and ARINT comes (count / 32) * 246 CPU cycles later (Dolphin's hardware
//             measurement; EventType::ArDma). ARAM addresses from 16 MiB on are the expansion
//             port, with nothing on it: writes are dropped and reads give zeros (Dolphin's
//             empty HSP; the SDK's __ARChecksize probes 0x01000000+ to find the size, so a stop
//             there would break every game). A main-memory range past MEM1 throws.
//   0x30/0x32 AI DMA start address (masks as above), 0x36 AI DMA control: bit 15 enable,
//             bits 0-14 the length in 32-byte blocks, 0x3A blocks left (read-only, zero-based
//             as Dolphin). Enabling latches start and length; 200 cycles later AID rises
//             (Dolphin: "for now we will just push it to 200 cycles"; EventType::AiDmaStart).
//             One block (8 stereo samples) is consumed every 8 samples of the AI's DMA rate
//             (121,500 cycles at 32 kHz; EventType::AiDmaBlock); when the buffer runs out the
//             registers are latched again and AID rises. A buffer past MEM1 throws when it is
//             latched. The samples are not played (no audio output in this sub-project).
//
// The DSP HLE (Dolphin's DSPHLE): a reset starts the DSP ROM, which queues 0x8071FEED (no
// interrupt: __DSP_boot_task polls for it). Clearing INIT (the SDK's __OSInitAudioSystem)
// runs the init code the CPU DMAed to ARAM: INITCODE reads 1 for 130 time-base ticks (1,560
// CPU cycles) and the code queues 0x80544348 (Dolphin's INITUCode). Either program's mail
// queue is emptied when it starts. The ROM answers a mail outside its upload protocol with
// 0xFEEE0000 | the mail's low half, and takes the SDK's microcode upload (__DSP_boot_task):
// 0x80F3A001 then the main-memory address of IRAM, 0x80F3C002 the IRAM address, 0x80F3A002 the
// IRAM length, 0x80F3B002 the DRAM length, 0x80F3D001 the start vector, which boots it (see
// microcode()). From then on every mail is only logged: the microcode's protocol is Task 7's.
class Dsp final : public MmioHandler {
public:
    static constexpr std::uint32_t kAramSize = 0x01000000;
    static constexpr std::uint64_t kAramCyclesPer32Bytes = 246;
    static constexpr std::uint64_t kAiDmaStartCycles = 200;
    static constexpr std::size_t kMailLogSize = 256;

    // What __DSP_boot_task uploaded (Dolphin's ROMUCode fields).
    struct MicrocodeUpload {
        std::uint32_t iram_mmem_address = 0;  // where the IRAM image is in main memory
        std::uint16_t iram_address = 0, iram_length = 0, dram_length = 0, init_vector = 0;
    };

    // `mem1` is MEM1 in guest order; raises Pi::kDsp on `pi`; the AI DMA plays at `ai`'s rate.
    Dsp(Scheduler& scheduler, Pi& pi, std::span<std::uint8_t> mem1, const Ai& ai);

    // Handlers of EventType::ArDma, AiDmaStart and AiDmaBlock (due at `when`).
    void on_aram_dma_done();
    void on_ai_dma_start();
    void on_ai_dma_block(std::uint64_t when);

    [[nodiscard]] std::span<std::uint8_t> aram() { return aram_; }
    [[nodiscard]] std::span<const std::uint8_t> aram() const { return aram_; }
    // Mails CPU->DSP since the boot (for Stats), the last kMailLogSize of them, oldest first,
    // and the microcode the ROM booted, if any.
    [[nodiscard]] std::uint64_t mails_received() const { return mails_; }
    [[nodiscard]] const std::deque<std::uint32_t>& mail_log() const { return mail_log_; }
    [[nodiscard]] const std::optional<MicrocodeUpload>& microcode() const { return microcode_; }

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    enum class Program : std::uint8_t { Rom, InitCode, Microcode };

    void write_control(std::uint16_t value);
    void start_program(Program program);
    void receive_mail(std::uint32_t mail);
    void push_mail(std::uint32_t mail) { mails_out_.push_back(mail); }
    // Throws if an ARAM DMA with this count register would run past MEM1.
    void check_aram_dma(std::uint32_t count_register) const;
    void start_aram_dma();
    void latch_ai_dma();  // loads the AI DMA's block count (throws if the buffer leaves MEM1)
    [[nodiscard]] std::uint64_t ai_block_cycles() const;
    void update_irq();
    [[nodiscard]] bool halted() const;

    Scheduler& sched_;
    Pi& pi_;
    std::span<std::uint8_t> mem1_;
    const Ai& ai_;
    std::vector<std::uint8_t> aram_;

    std::uint16_t control_;
    std::uint64_t init_code_done_ = 0;  // when INITCODE reads 0 again

    // Mailboxes.
    std::uint32_t mail_to_dsp_ = 0;
    std::deque<std::uint32_t> mails_out_;  // DSP -> CPU, oldest first
    std::uint32_t last_mail_out_ = 0;
    std::uint64_t mails_ = 0;
    std::deque<std::uint32_t> mail_log_;

    // The HLE.
    Program program_ = Program::Rom;
    std::uint32_t rom_parameter_ = 0;  // the upload mail whose value comes next (0: none)
    MicrocodeUpload upload_;
    std::optional<MicrocodeUpload> microcode_;

    // ARAM controller and DMA.
    std::uint16_t ar_info_ = 0, ar_refresh_ = 156;
    static constexpr std::uint16_t ar_mode_ = 1;  // read-only: the ARAM controller is ready
    std::uint32_t ar_mm_ = 0, ar_ar_ = 0, ar_count_ = 0;

    // AI DMA.
    std::uint32_t ai_start_ = 0;
    std::uint16_t ai_control_ = 0;
    std::uint16_t ai_blocks_left_ = 0;
};

}  // namespace ezy::gc
