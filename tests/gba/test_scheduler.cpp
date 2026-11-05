#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "gba/bus.hpp"
#include "gba/gba.hpp"
#include "gba/scheduler.hpp"

using namespace ezy::gba;

// --- Scheduler ---

namespace {

// Only FrameEnd exists so far; ordering and tie-breaking between types get tested once the
// PPU adds its events.
constexpr EventType kA = EventType::FrameEnd;

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

TEST_CASE("Gba frames are 280896 cycles and lateness does not accumulate") {
    const auto gba = Gba::create(loop_rom());
    REQUIRE(gba);
    gba->run_frame();
    CHECK(gba->frame_count() == 1);
    // B . from ROM costs a few cycles (fetches with waitstates), so the frame can only
    // overrun by less than one instruction.
    const std::uint64_t after_one = gba->scheduler().now();
    CHECK(after_one >= kCyclesPerFrame);
    CHECK(after_one < kCyclesPerFrame + 32);
    for (int i = 0; i < 9; ++i) gba->run_frame();
    CHECK(gba->frame_count() == 10);
    CHECK(gba->scheduler().now() >= 10ULL * kCyclesPerFrame);
    CHECK(gba->scheduler().now() < 10ULL * kCyclesPerFrame + 32);
}
