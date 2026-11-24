#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "disc_builder.hpp"
#include "gc/ai.hpp"
#include "gc/dsp.hpp"
#include "gc/gamecube.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "run_events.hpp"

using namespace ezy::gc;

namespace {
// DSP interface registers (Dolphin's DSP.cpp), 16 bits each: the two mailboxes, the control
// register, the ARAM controller and its DMA, the AI DMA.
constexpr std::uint32_t kMailToDsp = 0x00, kMailFromDsp = 0x04, kControl = 0x0A, kArInfo = 0x12, kArMode = 0x16,
                        kArMm = 0x20, kArAr = 0x24, kArCnt = 0x28, kAiStart = 0x30, kAiControl = 0x36,
                        kAiBlocksLeft = 0x3A;
// Control register bits (Dolphin's UDSPControl).
constexpr std::uint16_t kReset = 0x001, kAssertInt = 0x002, kHalt = 0x004, kAid = 0x008, kAidMask = 0x010,
                        kAram = 0x020, kAramMask = 0x040, kDspInt = 0x080, kDmaBusy = 0x200, kInitCode = 0x400,
                        kInit = 0x800;
// AI registers (Dolphin's AudioInterface.cpp), 32 bits: control, volume, sample counter, timing.
constexpr std::uint32_t kAicr = 0x00, kAivr = 0x04, kAiscnt = 0x08, kAiit = 0x0C;
constexpr std::uint32_t kPstat = 0x01, kAiintMsk = 0x04, kAiint = 0x08, kScReset = 0x20, kAidfr = 0x40;

struct AudioRig {
    Scheduler sched;
    Pi pi;
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(Memory::kMem1Size, 0);
    Ai ai{sched, pi};
    Dsp dsp{sched, pi, ram, ai};

    void run_to(std::uint64_t cycle) {
        run_events_to(sched, cycle, [&](const Scheduler::Due& due) {
            switch (due.type) {
                case EventType::ArDma: dsp.on_aram_dma_done(); break;
                case EventType::AiDmaStart: dsp.on_ai_dma_start(); break;
                case EventType::AiDmaBlock: dsp.on_ai_dma_block(due.when); break;
                case EventType::AiSample: ai.on_event(); break;
                default: break;
            }
        });
    }
    void run_for(std::uint64_t cycles) { run_to(sched.now() + cycles); }
    std::uint16_t control() { return dsp.read16(kControl); }
    void set_control(std::uint16_t value) { dsp.write16(kControl, value); }
};
}  // namespace

TEST_CASE("ARAM DMA copies both ways and raises its interrupt") {
    AudioRig rig;
    auto& dsp = rig.dsp;
    rig.pi.write32(0x04, 1u << Pi::kDsp);
    // The ARAM controller reports itself initialized (ARInit waits on AR_MODE bit 0).
    CHECK((dsp.read16(kArMode) & 1) == 1);
    rig.set_control(kHalt | kInit | kAramMask);

    for (std::uint32_t i = 0; i < 0x100; ++i) rig.ram[0x10000 + i] = static_cast<std::uint8_t>(i * 7 + 1);

    // Main memory -> ARAM: the copy is immediate, the interrupt comes after 246 cycles per
    // 32 bytes (Dolphin's measured rate), with the busy bit up until then.
    dsp.write32(kArMm, 0x00010000);
    dsp.write32(kArAr, 0x00000400);
    dsp.write32(kArCnt, 0x00000100);  // direction 0: to ARAM, 256 bytes
    CHECK(std::equal(rig.ram.begin() + 0x10000, rig.ram.begin() + 0x10100, dsp.aram().begin() + 0x400));
    CHECK((rig.control() & kDmaBusy) != 0);
    rig.run_for(8 * 246 - 1);
    CHECK((rig.control() & kAram) == 0);
    CHECK_FALSE(rig.pi.line());
    rig.run_for(1);
    CHECK((rig.control() & kAram) != 0);
    CHECK((rig.control() & kDmaBusy) == 0);
    CHECK(rig.pi.line());
    CHECK(dsp.read32(kArMm) == 0x00010100);  // the addresses advance, the count runs out
    CHECK(dsp.read32(kArAr) == 0x00000500);
    CHECK(dsp.read32(kArCnt) == 0);
    rig.set_control(kHalt | kInit | kAramMask | kAram);  // acknowledge
    CHECK((rig.control() & kAram) == 0);
    CHECK_FALSE(rig.pi.line());

    // ARAM -> main memory (direction bit 31).
    dsp.write32(kArMm, 0x00020000);
    dsp.write32(kArAr, 0x00000400);
    dsp.write32(kArCnt, 0x80000100);
    CHECK(std::equal(rig.ram.begin() + 0x10000, rig.ram.begin() + 0x10100, rig.ram.begin() + 0x20000));
    rig.run_for(8 * 246);
    CHECK((rig.control() & kAram) != 0);

    // The registers drop the low 5 bits (Dolphin's write masks): DMAs are whole 32-byte units.
    dsp.write32(kArMm, 0x0003001F);
    CHECK(dsp.read32(kArMm) == 0x00030000);

    // Past ARAM's 16 MiB is the expansion port, with nothing on it: writes are dropped and
    // reads give zeros (what the SDK's __ARChecksize probes for, to find 16 MiB).
    dsp.write32(kArMm, 0x00010000);
    dsp.write32(kArAr, 0x01000000);
    dsp.write32(kArCnt, 0x00000020);
    rig.run_for(246);
    CHECK(std::all_of(dsp.aram().begin(), dsp.aram().begin() + 0x400, [](std::uint8_t b) { return b == 0; }));
    dsp.write32(kArMm, 0x00020000);
    dsp.write32(kArAr, 0x01000000);
    dsp.write32(kArCnt, 0x80000020);
    CHECK(std::all_of(rig.ram.begin() + 0x20000, rig.ram.begin() + 0x20020, [](std::uint8_t b) { return b == 0; }));
    rig.run_for(246);

    // Deeper in the expansion range, where __ARChecksize goes next (0x01200000, 0x01400000, up
    // to the register's top), both ways: nothing reaches ARAM, zeros come back.
    for (const std::uint32_t probe : {0x01200000u, 0x01400000u, 0x02000000u, 0x03FFFFE0u}) {
        dsp.write32(kArMm, 0x00010000);
        dsp.write32(kArAr, probe);
        dsp.write32(kArCnt, 0x00000020);
        rig.run_for(246);
        std::fill_n(rig.ram.begin() + 0x20000, 0x20, std::uint8_t{0xAA});
        dsp.write32(kArMm, 0x00020000);
        dsp.write32(kArAr, probe);
        dsp.write32(kArCnt, 0x80000020);
        CHECK(std::all_of(rig.ram.begin() + 0x20000, rig.ram.begin() + 0x20020, [](std::uint8_t b) { return b == 0; }));
        rig.run_for(246);
    }
    CHECK(std::all_of(dsp.aram().begin(), dsp.aram().begin() + 0x400, [](std::uint8_t b) { return b == 0; }));
    CHECK(std::equal(rig.ram.begin() + 0x10000, rig.ram.begin() + 0x10100, dsp.aram().begin() + 0x400));

    // A DMA across the 16 MiB line: the part below it is ARAM, the rest the empty port.
    for (std::uint32_t i = 0; i < 0x40; ++i) rig.ram[0x30000 + i] = static_cast<std::uint8_t>(0x40 + i);
    dsp.write32(kArMm, 0x00030000);
    dsp.write32(kArAr, 0x00FFFFE0);
    dsp.write32(kArCnt, 0x00000040);
    CHECK(std::equal(rig.ram.begin() + 0x30000, rig.ram.begin() + 0x30020, dsp.aram().end() - 0x20));
    CHECK(dsp.read32(kArAr) == 0x01000020);
    rig.run_for(2 * 246);
    std::fill_n(rig.ram.begin() + 0x40000, 0x40, std::uint8_t{0xAA});
    dsp.write32(kArMm, 0x00040000);
    dsp.write32(kArAr, 0x00FFFFE0);
    dsp.write32(kArCnt, 0x80000040);
    CHECK(std::equal(rig.ram.begin() + 0x30000, rig.ram.begin() + 0x30020, rig.ram.begin() + 0x40000));
    CHECK(std::all_of(rig.ram.begin() + 0x40020, rig.ram.begin() + 0x40040, [](std::uint8_t b) { return b == 0; }));
    rig.run_for(2 * 246);

    // The controller registers keep Dolphin's write masks; AR_MODE takes no write at all.
    dsp.write16(kArInfo, 0xFFFF);
    CHECK(dsp.read16(kArInfo) == 0x007F);
    dsp.write16(0x1A, 0xFFFF);
    CHECK(dsp.read16(0x1A) == 0x07FF);
    dsp.write16(kArMode, 0);
    CHECK(dsp.read16(kArMode) == 1);
    dsp.write16(kArCnt, 0xFFFF);
    CHECK(dsp.read16(kArCnt) == 0x83FF);
    dsp.write16(kArCnt, 0);

    // A main-memory side past MEM1 stops the machine, before anything is copied, and the
    // 32-bit write that would start it changes no register (not even its high half).
    dsp.write32(kArMm, Memory::kMem1Size - 0x20);
    dsp.write32(kArAr, 0);
    const std::uint32_t count_before = dsp.read32(kArCnt);
    CHECK(count_before == 0);
    CHECK_THROWS_AS(dsp.write32(kArCnt, 0x80000040), BusError);
    CHECK(dsp.read32(kArCnt) == count_before);
    CHECK((rig.control() & kDmaBusy) == 0);
    CHECK(std::all_of(rig.ram.end() - 0x20, rig.ram.end(), [](std::uint8_t b) { return b == 0; }));
}

TEST_CASE("AI DMA consumes blocks at 32 kHz and raises its interrupt") {
    AudioRig rig;
    auto& dsp = rig.dsp;
    rig.pi.write32(0x04, 1u << Pi::kDsp);
    rig.ai.write32(kAicr, kAidfr);  // the DSP's samples at 32 kHz (AIDFR = 1, the SDK's setting)
    CHECK(rig.ai.dma_sample_rate() == 32000);
    rig.set_control(kHalt | kInit | kAidMask);

    // 4 blocks of 32 bytes (8 stereo 16-bit samples each) from 0x00100000. One block lasts
    // 8 / 32000 s = 121,500 CPU cycles. Dolphin raises AID 200 cycles after the start (the
    // registers latched, the next buffer may be queued) and again at every reload.
    constexpr std::uint64_t kBlock = 121500;
    dsp.write32(kAiStart, 0x00100000);
    dsp.write16(kAiControl, 0x8000 | 4);
    const std::uint64_t t0 = rig.sched.now();
    CHECK(dsp.read16(kAiBlocksLeft) == 3);  // zero-based, as Dolphin
    rig.run_to(t0 + 199);
    CHECK((rig.control() & kAid) == 0);
    rig.run_to(t0 + 200);
    CHECK((rig.control() & kAid) != 0);
    CHECK(rig.pi.line());
    rig.set_control(kHalt | kInit | kAidMask | kAid);
    CHECK_FALSE(rig.pi.line());

    rig.run_to(t0 + kBlock - 1);
    CHECK(dsp.read16(kAiBlocksLeft) == 3);
    rig.run_to(t0 + kBlock);
    CHECK(dsp.read16(kAiBlocksLeft) == 2);
    rig.run_to(t0 + 3 * kBlock);
    CHECK(dsp.read16(kAiBlocksLeft) == 0);
    rig.run_to(t0 + 4 * kBlock - 1);
    CHECK((rig.control() & kAid) == 0);
    rig.run_to(t0 + 4 * kBlock);  // the buffer is used up: reload from the registers, AID
    CHECK((rig.control() & kAid) != 0);
    CHECK(dsp.read16(kAiBlocksLeft) == 3);
    rig.set_control(kHalt | kInit | kAidMask | kAid);

    // Stopping it stops the blocks and the interrupts.
    dsp.write16(kAiControl, 4);
    rig.run_to(t0 + 12 * kBlock);
    CHECK((rig.control() & kAid) == 0);
    CHECK_FALSE(rig.sched.pending(EventType::AiDmaBlock));

    // At 48 kHz (AIDFR = 0) a block lasts 81,000 cycles.
    rig.ai.write32(kAicr, 0);
    CHECK(rig.ai.dma_sample_rate() == 48000);
    dsp.write16(kAiControl, 0x8000 | 2);
    const std::uint64_t t1 = rig.sched.now();
    rig.run_to(t1 + 200);
    rig.set_control(kHalt | kInit | kAidMask | kAid);
    rig.run_to(t1 + 2 * 81000 - 1);
    CHECK((rig.control() & kAid) == 0);
    rig.run_to(t1 + 2 * 81000);
    CHECK((rig.control() & kAid) != 0);
    dsp.write16(kAiControl, 0);

    // A buffer reaching past MEM1 stops the machine when it is started.
    dsp.write32(kAiStart, Memory::kMem1Size - 0x20);
    CHECK_THROWS_AS(dsp.write16(kAiControl, 0x8000 | 2), BusError);
    CHECK_THROWS_AS(dsp.write16(kAiBlocksLeft, 0), BusError);

    // A DSP reset stops the AI DMA by clearing its control register only: the blocks left keep
    // their count, as in Dolphin (its DSP_CONTROL write clears AudioDMAControl.Hex alone).
    dsp.write32(kAiStart, 0x00100000);
    dsp.write16(kAiControl, 0x8000 | 4);
    rig.set_control(kHalt | kInit | kAidMask | kReset);
    CHECK(dsp.read16(kAiControl) == 0);
    CHECK(dsp.read16(kAiBlocksLeft) == 3);
    CHECK_FALSE(rig.sched.pending(EventType::AiDmaStart));
    CHECK_FALSE(rig.sched.pending(EventType::AiDmaBlock));
}

TEST_CASE("AI sample counter counts while playing and raises AIINT at its timing register") {
    AudioRig rig;
    auto& ai = rig.ai;
    rig.pi.write32(0x04, 1u << Pi::kAi);
    ai.write32(kAivr, 0x0000FF80);
    CHECK(ai.read32(kAivr) == 0x0000FF80);
    CHECK(ai.read32(kAiscnt) == 0);

    // 100 samples at 32 kHz (AISFR = 0) are exactly 1,518,750 CPU cycles.
    ai.write32(kAiit, 100);
    ai.write32(kAicr, kPstat | kAiintMsk);
    rig.run_for(1518749);
    CHECK(ai.read32(kAiscnt) == 99);
    CHECK((ai.read32(kAicr) & kAiint) == 0);
    CHECK_FALSE(rig.pi.line());
    rig.run_for(1);
    CHECK(ai.read32(kAiscnt) == 100);
    CHECK((ai.read32(kAicr) & kAiint) != 0);
    CHECK(rig.pi.line());
    ai.write32(kAicr, kPstat | kAiintMsk | kAiint);  // acknowledge
    CHECK_FALSE(rig.pi.line());

    // Stopped, it holds; SCRESET zeroes it.
    ai.write32(kAicr, kAiintMsk);
    const std::uint32_t held = ai.read32(kAiscnt);
    rig.run_for(1000000);
    CHECK(ai.read32(kAiscnt) == held);
    ai.write32(kAicr, kAiintMsk | kScReset);
    CHECK(ai.read32(kAiscnt) == 0);
    CHECK((ai.read32(kAicr) & kScReset) == 0);
    CHECK_THROWS_AS(ai.read16(kAicr), BusError);
    CHECK_THROWS_AS(ai.read32(0x10), BusError);
}

TEST_CASE("DSP HLE answers the boot handshake mails") {
    AudioRig rig;
    auto& dsp = rig.dsp;
    // At reset the DSP is halted with DSPInit set (Dolphin's DSPHLE::Initialize).
    CHECK(rig.control() == (kHalt | kInit));

    // The SDK's __OSInitAudioSystem (OSInit), step by step (MKDD's own SDK, doldecomp/mkdd).
    dsp.write16(kArInfo, 0x43);
    rig.set_control(0x8AC);
    rig.set_control(rig.control() | kReset);
    CHECK((rig.control() & kReset) == 0);       // the reset is done at once
    dsp.write16(kMailToDsp, 0);
    CHECK((dsp.read32(kMailFromDsp) & 0x80000000u) == 0);  // halted: the ROM's mail is held back
    for (int i = 0; i < 2; ++i) {                       // 32 bytes of init code to ARAM 0, twice
        dsp.write32(kArMm, 0x01000000);
        dsp.write32(kArAr, 0);
        dsp.write32(kArCnt, 0x20);
        rig.run_for(246);
        CHECK((rig.control() & kAram) != 0);
        rig.set_control(rig.control());  // writing it back acknowledges ARINT
        CHECK((rig.control() & kAram) == 0);
    }
    rig.set_control(rig.control() & ~kInit);  // the DSP runs the init code from ARAM...
    CHECK((rig.control() & kInitCode) != 0);   // ...busy for 130 time-base ticks (Dolphin)
    rig.run_for(130 * 12);
    CHECK((rig.control() & kInitCode) == 0);
    rig.set_control(rig.control() & ~kHalt);
    CHECK(dsp.read16(kMailFromDsp) == 0x8054);  // the init code's mail, 0x80544348 (Dolphin)
    CHECK(dsp.read16(kMailFromDsp + 2) == 0x4348);
    CHECK((dsp.read16(kMailFromDsp) & 0x8000) == 0);  // read: no longer pending
    rig.set_control(rig.control() | kHalt);
    rig.set_control(0x8AC);
    rig.set_control(rig.control() | kReset);
    CHECK((rig.control() & kReset) == 0);

    // The SDK's DSPInit: DSPInit set, interrupts left alone, then unhalted. The ROM greets.
    rig.set_control(static_cast<std::uint16_t>((rig.control() & ~0xA8) | kInit));
    rig.set_control(static_cast<std::uint16_t>(rig.control() & ~0xAC));
    CHECK(dsp.read32(kMailFromDsp) == 0x8071FEEDu);
    CHECK((dsp.read32(kMailFromDsp) & 0x80000000u) == 0);
    CHECK((rig.control() & kDspInt) == 0);  // polled: the ROM raises no interrupt

    // Mail the ROM does not expect: it answers 0xFEEE0000 | the low half.
    dsp.write32(kMailToDsp, 0x12345678);
    CHECK((dsp.read32(kMailToDsp) & 0x80000000u) == 0);  // taken at once
    CHECK(dsp.read32(kMailFromDsp) == 0xFEEE5678u);

    // __DSP_boot_task: the microcode upload, each mail taken before the next.
    const std::uint32_t upload[] = {0x80F3A001, 0x00345600, 0x80F3C002, 0x0000, 0x80F3A002, 0x1F20,
                                    0x80F3B002, 0x0000,     0x80F3D001, 0x0010};
    for (const std::uint32_t mail : upload) {
        dsp.write32(kMailToDsp, mail);
        CHECK((dsp.read32(kMailToDsp) & 0x80000000u) == 0);
    }
    REQUIRE(dsp.microcode().has_value());
    const auto ucode = *dsp.microcode();
    CHECK(ucode.iram_mmem_address == 0x00345600);
    CHECK(ucode.iram_address == 0x0000);
    CHECK(ucode.iram_length == 0x1F20);
    CHECK(ucode.dram_length == 0);
    CHECK(ucode.init_vector == 0x0010);
    CHECK(dsp.mails_received() == 11);
    REQUIRE(dsp.mail_log().size() == 11);
    CHECK(dsp.mail_log().front() == 0x12345678u);
    CHECK(dsp.mail_log().back() == 0x0010u);
    CHECK((dsp.read32(kMailFromDsp) & 0x80000000u) == 0);  // the ROM answers nothing to the upload

    // The CPU->DSP interrupt belongs to the microcode's protocol (Task 7): not modeled yet.
    CHECK_THROWS_AS(rig.set_control(rig.control() | kAssertInt), BusError);
    CHECK_THROWS_AS(dsp.write16(kMailFromDsp, 0), BusError);
    CHECK_THROWS_AS(dsp.read16(0x10), BusError);
}

TEST_CASE("the machine maps the DSP, AI, SI and EXI and routes their events") {
    DiscBuilder builder;
    const auto image = builder.write("audio_machine");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    constexpr std::uint32_t kDspBase = 0x0C005000, kSiBase = 0x0C006400, kAiBase = 0x0C006C00;

    mem.write16(kDspBase + kControl, kHalt | kInit | kAramMask | kAidMask);
    mem.write32(kDspBase + kArMm, 0x00010000);
    mem.write32(kDspBase + kArAr, 0);
    mem.write32(kDspBase + kArCnt, 0x20);
    mem.write32(kAiBase + kAicr, kAidfr);
    mem.write32(kDspBase + kAiStart, 0x00100000);
    mem.write16(kDspBase + kAiControl, 0x8001);
    PadState pad;
    pad.buttons = 0x0100;
    gc->set_pad(pad);
    mem.write32(kSiBase + 0x00, 0x00400300);
    mem.write32(kSiBase + 0x30, (10u << 16) | 0x80);
    mem.write32(kDspBase + kMailToDsp, 0xCAFE0001);
    mem.write32(0x0C003004, 1u << Pi::kDsp);

    gc->run_field();
    const std::uint16_t control = mem.read16(kDspBase + kControl);
    CHECK((control & kAram) != 0);
    CHECK((control & kAid) != 0);
    CHECK((mem.read32(0x0C003000) & (1u << Pi::kDsp)) != 0);
    CHECK((mem.read32(kSiBase + 0x38) & (1u << 29)) != 0);  // RDST0: the pad was polled
    CHECK((mem.read32(kSiBase + 0x04) >> 16) == (0x0100u | 0x0080u));
    CHECK((mem.read32(0x0C006800) & (1u << 12)) == 0);  // EXI mapped: card slot A empty (EXT)
    CHECK(gc->stats().dsp_mails == 1);
}
