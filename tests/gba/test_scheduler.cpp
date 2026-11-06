#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "gba/bus.hpp"
#include "gba/gba.hpp"
#include "gba/scheduler.hpp"

using namespace ezy::gba;

// --- Scheduler ---

namespace {

constexpr EventType kA = EventType::PpuHDrawEnd;
constexpr EventType kB = EventType::PpuHBlankEnd;

}  // namespace

TEST_CASE("scheduler pops nothing until the event is due") {
    Scheduler s;
    s.schedule_in(kA, 100);
    CHECK(s.pending(kA));
    CHECK(s.next_event_time() == 100);
    s.advance(99);
    CHECK_FALSE(s.pop_due());
    s.advance(1);
    const auto due = s.pop_due();
    REQUIRE(due);
    CHECK(due->type == kA);
    CHECK(due->when == 100);
    CHECK_FALSE(s.pending(kA));
    CHECK_FALSE(s.pop_due());
    CHECK_FALSE(s.next_event_time());
}

TEST_CASE("scheduler: rescheduling replaces, cancel removes") {
    Scheduler s;
    s.schedule(kA, 50);
    s.schedule(kA, 80);  // replaces the 50
    s.advance(60);
    CHECK_FALSE(s.pop_due());
    s.cancel(kA);
    s.advance(100);
    CHECK_FALSE(s.pop_due());
}

TEST_CASE("scheduler pops in time order, ties by type") {
    Scheduler s;
    s.schedule(kB, 30);
    s.schedule(kA, 40);
    s.advance(50);
    auto first = s.pop_due();
    REQUIRE(first);
    CHECK(first->type == kB);
    CHECK(s.pop_due()->type == kA);

    s.schedule(kB, 60);
    s.schedule(kA, 60);
    s.advance(20);
    CHECK(s.pop_due()->type == kA);  // same time: lower type first
    CHECK(s.pop_due()->type == kB);
}

TEST_CASE("scheduler: a late event keeps its original time") {
    Scheduler s;
    s.schedule(kA, 10);
    s.advance(25);  // an instruction overran the event
    const auto due = s.pop_due();
    REQUIRE(due);
    CHECK(due->when == 10);
    CHECK(s.now() == 25);
}

// --- Bus timing (GBATEK "GBA Memory Map" waitstate table) ---

namespace {

std::uint64_t cost(Bus& bus, Scheduler& s, std::uint32_t addr, int size, Access access = kNonseq) {
    const std::uint64_t before = s.now();
    if (size == 1) (void)bus.read8(addr, access);
    if (size == 2) (void)bus.read16(addr, access);
    if (size == 4) (void)bus.read32(addr, access);
    return s.now() - before;
}

}  // namespace

TEST_CASE("bus charges waitstates per region") {
    Scheduler s;
    Bus bus(std::vector<std::uint8_t>(0x1000), {}, &s);
    CHECK(cost(bus, s, 0x03000000, 4) == 1);  // IWRAM
    CHECK(cost(bus, s, 0x00000000, 4) == 1);  // BIOS
    CHECK(cost(bus, s, 0x07000000, 4) == 1);  // OAM
    CHECK(cost(bus, s, 0x02000000, 2) == 3);  // EWRAM
    CHECK(cost(bus, s, 0x02000000, 4) == 6);
    CHECK(cost(bus, s, 0x06000000, 2) == 1);  // VRAM
    CHECK(cost(bus, s, 0x06000000, 4) == 2);
    bus.idle();
    CHECK(s.now() == 1 + 1 + 1 + 3 + 6 + 1 + 2 + 1);
}

TEST_CASE("bus ROM waitstates follow WAITCNT") {
    Scheduler s;
    Bus bus(std::vector<std::uint8_t>(0x1000), {}, &s);
    // Power-on WAITCNT = 0: WS0 N=4 S=2 -> 5/3 cycles per halfword, a word 5+3.
    CHECK(cost(bus, s, 0x08000000, 2, kNonseq) == 5);
    CHECK(cost(bus, s, 0x08000000, 2, kSeq) == 3);
    CHECK(cost(bus, s, 0x08000000, 4, kNonseq) == 8);
    CHECK(cost(bus, s, 0x08000000, 4, kSeq) == 6);
    CHECK(cost(bus, s, 0x0C000000, 2, kSeq) == 9);  // WS2 S=8
    CHECK(cost(bus, s, 0x0E000000, 1) == 5);        // SRAM N=4

    // What most games set: WAITCNT = 0x4317 -> SRAM 8, WS0 3/1, WS1 4/1, WS2 8/1, prefetch.
    bus.write16(Bus::kWaitcnt, 0x4317);
    CHECK(cost(bus, s, 0x08000000, 2, kNonseq) == 4);
    CHECK(cost(bus, s, 0x08000000, 2, kSeq) == 2);
    CHECK(cost(bus, s, 0x08000000, 4, kNonseq) == 6);
    CHECK(cost(bus, s, 0x0E000000, 1) == 9);
}

TEST_CASE("bus without a scheduler is untimed") {
    Bus bus(std::vector<std::uint8_t>(0x1000));
    (void)bus.read32(0x08000000, kNonseq);  // must not crash
    CHECK(bus.access_cycles(0x08000000, 4, kNonseq) == 8);
}

// --- Gba ---

namespace {

// A ROM whose entry point is `B .` (an infinite loop), with a parseable header.
std::vector<std::uint8_t> loop_rom() {
    std::vector<std::uint8_t> rom(0x200, 0);
    rom[0] = 0xFE;
    rom[1] = 0xFF;
    rom[2] = 0xFF;
    rom[3] = 0xEA;  // B .
    return rom;
}

}  // namespace

TEST_CASE("Gba boots without a BIOS in the state the BIOS leaves") {
    const auto gba = Gba::create(loop_rom());
    REQUIRE(gba);
    const Arm7tdmi& cpu = gba->cpu();
    CHECK(cpu.mode() == Mode::System);
    CHECK_FALSE(cpu.thumb());
    CHECK(cpu.reg(15) == 0x08000008);  // pipeline filled from the entry point
    CHECK(cpu.reg(13) == 0x03007F00);
    CHECK(cpu.banked(kBankSvc, 5) == 0x03007FE0);
    CHECK(cpu.banked(kBankIrq, 5) == 0x03007FA0);
}

TEST_CASE("Gba frames end at VBlank, 280896 cycles apart, without accumulating lateness") {
    const auto gba = Gba::create(loop_rom());
    REQUIRE(gba);
    // The first frame starts at line 0, so it ends when line 160 starts.
    gba->run_frame();
    constexpr std::uint64_t kFirst = 160ULL * kCyclesPerLine;
    CHECK(gba->frame_count() == 1);
    CHECK(gba->bus().ppu().vcount() == 160);
    CHECK(gba->scheduler().now() >= kFirst);
    CHECK(gba->scheduler().now() < kFirst + 32);  // overrun: less than one instruction
    for (int i = 0; i < 9; ++i) gba->run_frame();
    CHECK(gba->frame_count() == 10);
    CHECK(gba->scheduler().now() >= kFirst + 9ULL * kCyclesPerFrame);
    CHECK(gba->scheduler().now() < kFirst + 9ULL * kCyclesPerFrame + 32);
}

TEST_CASE("ROM prefetch buffer hides waitstates of sequential opcode fetches") {
    const auto code_seq = static_cast<Access>(kCode | kSeq);
    const auto code_nonseq = static_cast<Access>(kCode | kNonseq);
    Scheduler s;
    Bus bus(std::vector<std::uint8_t>(0x1000), {}, &s);

    // Without prefetch (WAITCNT = 0x0317): every fetch pays WS0's S = 1 waitstate.
    bus.write16(Bus::kWaitcnt, 0x0317);
    CHECK(cost(bus, s, 0x08000000, 2, code_nonseq) == 4);
    for (int i = 0; i < 4; ++i) bus.idle();
    CHECK(cost(bus, s, 0x08000002, 2, code_seq) == 2);

    // With prefetch: the first fetch restarts the buffer, which fills while the CPU is
    // busy elsewhere (2 cycles per halfword); buffered halfwords then take 1 cycle.
    bus.write16(Bus::kWaitcnt, 0x4317);
    CHECK(cost(bus, s, 0x08000100, 2, code_nonseq) == 4);
    for (int i = 0; i < 4; ++i) bus.idle();                  // 2 halfwords prefetched
    CHECK(cost(bus, s, 0x08000102, 2, code_seq) == 1);
    CHECK(cost(bus, s, 0x08000104, 2, code_seq) == 1);
    CHECK(cost(bus, s, 0x08000106, 2, code_seq) == 1);       // finished during those fetches
    // Buffer empty, 0x108 half fetched: the CPU waits only for the rest.
    CHECK(cost(bus, s, 0x08000108, 2, code_seq) == 1);
    // An ARM fetch needs two halfwords.
    for (int i = 0; i < 4; ++i) bus.idle();
    CHECK(cost(bus, s, 0x0800010A, 4, code_seq) == 1);

    // A branch (non-matching address) misses and pays the normal N cost again.
    CHECK(cost(bus, s, 0x08000200, 2, code_nonseq) == 4);
    // A data load from ROM stops the prefetcher.
    for (int i = 0; i < 4; ++i) bus.idle();
    CHECK(cost(bus, s, 0x08000800, 2, kNonseq) == 4);
    CHECK(cost(bus, s, 0x08000202, 2, code_seq) == 2);       // the buffer was discarded: S cost

    // Accesses to other regions also let the buffer fill.
    CHECK(cost(bus, s, 0x03000000, 4, kNonseq) == 1);
    CHECK(cost(bus, s, 0x02000000, 4, kNonseq) == 6);         // 3 halfwords' worth
    CHECK(cost(bus, s, 0x08000204, 2, code_seq) == 1);
    CHECK(cost(bus, s, 0x08000206, 2, code_seq) == 1);
    CHECK(cost(bus, s, 0x08000208, 2, code_seq) == 1);
}
