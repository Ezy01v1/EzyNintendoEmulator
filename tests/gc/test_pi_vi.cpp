#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "disc_builder.hpp"
#include "gc/gamecube.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/mi.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "gc/vi.hpp"

using namespace ezy::gc;

namespace {
constexpr std::uint32_t kPi = 0x0C003000, kVi = 0x0C002000;
constexpr std::uint64_t kField = 8'108'100;  // 486 MHz / 59.94 Hz

// Runs the scheduler up to `cycle`, delivering the VI's events.
void run_to(Scheduler& sched, Vi& vi, std::uint64_t cycle) {
    for (;;) {
        const auto next = sched.next_event_time();
        if (!next || *next > cycle) break;
        sched.advance_to(*next);
        while (auto due = sched.pop_due()) {
            if (due->type == EventType::ViLine) vi.on_event(due->when);
        }
    }
    sched.advance_to(cycle);
}
}  // namespace

TEST_CASE("PI raises the CPU interrupt when an enabled cause is set and clears it on acknowledge") {
    Pi pi;
    std::vector<bool> levels;
    pi.set_line_sink([&](bool l) { levels.push_back(l); });

    pi.set_cause(Pi::kDi, true);  // masked: no line
    CHECK_FALSE(pi.line());
    CHECK(levels.empty());
    pi.write32(0x04, 1u << Pi::kDi);  // enabling it raises the line
    CHECK(pi.line());
    CHECK(pi.read32(0x00) == ((1u << Pi::kDi) | (1u << 16)));  // cause plus the reset-switch state
    pi.set_cause(Pi::kDi, false);  // the peripheral acknowledges
    CHECK_FALSE(pi.line());
    CHECK(levels == std::vector<bool>{true, false});

    // The latched causes clear when the CPU writes a 1 to them; a live one ignores the write.
    pi.write32(0x04, (1u << Pi::kGpuError) | (1u << Pi::kVi));
    pi.set_cause(Pi::kGpuError, true);
    pi.set_cause(Pi::kVi, true);
    CHECK(pi.line());
    pi.write32(0x00, 1u << Pi::kGpuError);
    CHECK((pi.read32(0x00) & 1u) == 0);
    CHECK(pi.line());  // still the VI
    pi.write32(0x00, 1u << Pi::kVi);
    CHECK(pi.line());
    CHECK(pi.interrupts(Pi::kVi) == 1);
    pi.set_cause(Pi::kVi, false);
    CHECK_FALSE(pi.line());

    // 16-bit halves, the revision, the reset code, and a register nobody models.
    pi.write16(0x24, 0x1234);
    pi.write16(0x26, 0x5678);
    CHECK(pi.read32(0x24) == 0x12345678);
    CHECK(pi.read32(0x2C) == Pi::kRevision);
    pi.write32(0x14, 0x80010020);
    CHECK(pi.fifo_write_pointer() == 0x80010020);
    CHECK_THROWS_AS(pi.read32(0x40), ezy::gc::BusError);
    CHECK_THROWS_AS(pi.write32(0x2C, 0), ezy::gc::BusError);
}

TEST_CASE("VI display interrupt fires at its line and clears when written") {
    Scheduler sched;
    Pi pi;
    Vi vi(sched, pi);
    pi.write32(0x04, 1u << Pi::kVi);
    vi.write32(0x30, 0x10000000 | (100u << 16));  // DI0: enable, VCT = 100
    CHECK(vi.read32(0x30) == (0x10000000u | (100u << 16)));

    constexpr std::uint64_t kLine = 30888;  // 858 samples * 36 CPU cycles
    run_to(sched, vi, 99 * kLine - 1);  // line 100 starts 99 lines in (lines count from 1)
    CHECK_FALSE(pi.line());
    run_to(sched, vi, 99 * kLine);
    CHECK(pi.line());
    CHECK((vi.read32(0x30) & 0x80000000u) != 0);
    CHECK(vi.read16(0x2C) == 100);  // DPV: the line being scanned

    // Writing the status bit as 0 acknowledges it (the SDK clears it by writing the register
    // back without bit 31); it stays clear until the next field reaches the line.
    vi.write32(0x30, 0x10000000 | (100u << 16));
    CHECK((vi.read32(0x30) & 0x80000000u) == 0);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, kField - 1);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, kField + 99 * kLine + 1);
    CHECK(pi.line());

    // A disabled one never sets its status.
    vi.write32(0x30, 100u << 16);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, 2 * kField + 200 * kLine);
    CHECK((vi.read32(0x30) & 0x80000000u) == 0);

    // A register the VI does not model throws; so does a write to the read-only beam position.
    CHECK_THROWS_AS(vi.read32(0x80), ezy::gc::BusError);
    CHECK_THROWS_AS(vi.write16(0x2C, 1), ezy::gc::BusError);
}

TEST_CASE("VI field timing gives 59.94 fields per second of CPU cycles") {
    DiscBuilder builder;
    const auto image = builder.write("vi_timing");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    for (int i = 0; i < 60; ++i) gc->run_field();
    CHECK(gc->cycles() == 60 * kField);
    CHECK(gc->stats().fields == 60);
    const double fields_per_second = 486e6 / static_cast<double>(kField);
    CHECK(fields_per_second == doctest::Approx(59.94).epsilon(0.0002));
    // The time base ran at one tick per 12 cycles.
    CHECK(gc->stats().instructions == 60 * kField);  // the synthetic program spins at 1 cycle each
}

TEST_CASE("the machine delivers the VI interrupt to the CPU through the PI") {
    DiscBuilder builder;
    const auto image = builder.write("vi_irq");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    mem.write32(kPi + 0x04, 1u << Pi::kVi);
    mem.write32(kVi + 0x30, 0x10000000 | (1u << 16));  // DI0: enable, line 1
    gc->run_field();
    gc->run_field();
    CHECK((mem.read32(kPi + 0x00) & (1u << Pi::kVi)) != 0);
    CHECK(gc->stats().interrupts[Pi::kVi] >= 1);
    mem.write32(kVi + 0x30, 0x10000000 | (1u << 16));  // acknowledge
    CHECK((mem.read32(kPi + 0x00) & (1u << Pi::kVi)) == 0);
}

TEST_CASE("XFB YUV to ARGB matches BT.601 for black, white and a color bar") {
    CHECK(yuv_to_argb(16, 128, 128) == 0xFF000000u);   // black
    CHECK(yuv_to_argb(235, 128, 128) == 0xFFFFFFFFu);  // white
    CHECK(yuv_to_argb(81, 90, 240) == 0xFFFF0000u);    // red
    CHECK(yuv_to_argb(149, 43, 21) == 0xFF00FF00u);    // green
    CHECK(yuv_to_argb(41, 240, 110) == 0xFF0000FFu);   // blue
    CHECK(yuv_to_argb(0, 128, 128) == 0xFF000000u);    // below black clamps, never wraps
    CHECK((yuv_to_argb(255, 255, 255) >> 24) == 0xFFu);
}

TEST_CASE("the XFB picture interleaves the two fields and doubles 240p lines") {
    DiscBuilder builder;
    const auto image = builder.write("xfb");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    auto ram = mem.mem1();
    constexpr std::uint32_t top = 0x00100000, bottom = 0x00200000;
    // Top field: red, bottom field: white (Y0 U Y1 V per two pixels).
    for (std::uint32_t i = 0; i < 640 * 2 * 240; i += 4) {
        const std::uint8_t red[4] = {81, 90, 81, 240}, white[4] = {235, 128, 235, 128};
        std::copy(red, red + 4, ram.begin() + top + i);
        std::copy(white, white + 4, ram.begin() + bottom + i);
    }
    CHECK(gc->xfb().argb.empty() == false);  // unconfigured: some picture, never a crash

    mem.write16(kVi + 0x00, 240 << 4);          // VTR: ACV = 240 lines per field
    mem.write16(kVi + 0x02, 0x0001);            // DCR: enabled, interlaced
    mem.write32(kVi + 0x1C, top);               // TFBL
    mem.write32(kVi + 0x24, bottom);            // BFBL
    auto picture = gc->xfb();
    REQUIRE(picture.width == 640);
    REQUIRE(picture.height == 480);
    REQUIRE(picture.argb.size() == 640u * 480u);
    CHECK(picture.argb[0] == 0xFFFF0000u);                 // row 0: the top field
    CHECK(picture.argb[640] == 0xFFFFFFFFu);               // row 1: the bottom field
    CHECK(picture.argb[640 * 2 + 639] == 0xFFFF0000u);
    CHECK(picture.argb[640 * 479 + 639] == 0xFFFFFFFFu);

    // Non-interlaced 240p: each line of the top XFB twice; the shift bit scales the address.
    mem.write16(kVi + 0x02, 0x0005);  // ENB | NIN
    mem.write32(kVi + 0x1C, (1u << 28) | (bottom >> 5));  // shifted: address = FBB << 5
    picture = gc->xfb();
    REQUIRE(picture.height == 480);
    CHECK(picture.argb[0] == 0xFFFFFFFFu);
    CHECK(picture.argb[640] == 0xFFFFFFFFu);
    CHECK(picture.argb[640 * 479] == 0xFFFFFFFFu);
}

TEST_CASE("a store to a hardware register does not touch RAM") {
    DiscBuilder builder;
    const auto image = builder.write("mmio_ram");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    const std::vector<std::uint8_t> before(mem.mem1().begin(), mem.mem1().end());

    mem.write32(kPi + 0x24, 0xDEADBEEF);
    mem.write32(kPi + 0x04, 0);
    mem.write16(kVi + 0x70, 0x1234);
    mem.write32(0x0C004010, 0xFFFF);  // MI protection type
    CHECK(mem.read32(kPi + 0x24) == 0xDEADBEEF);
    CHECK(mem.read16(kVi + 0x70) == 0x1234);
    CHECK(mem.read32(kPi + 0x2C) == Pi::kRevision);
    CHECK(std::equal(before.begin(), before.end(), mem.mem1().begin()));
    // The same offsets in RAM stay what they were (0x3024 is not a mirror of the register).
    CHECK(mem.read32(0x3024) == (std::uint32_t{before[0x3024]} << 24 | std::uint32_t{before[0x3025]} << 16 |
                                 std::uint32_t{before[0x3026]} << 8 | before[0x3027]));

    // Blocks the later tasks have not registered still stop the machine; so do 8-bit accesses
    // and a register in a registered block that nobody models.
    CHECK_THROWS_AS(mem.read32(0x0C006000), ezy::gc::BusError);  // DI
    CHECK_THROWS_AS(mem.write8(kPi + 0x24, 1), ezy::gc::BusError);
    CHECK_THROWS_AS(mem.read32(0x0C004030), ezy::gc::BusError);  // MI profiling counters
}

TEST_CASE("MI accepts its protection registers and masks its interrupt") {
    Mi mi;
    bool irq = false;
    mi.set_irq_sink([&](bool l) { irq = l; });
    mi.write16(0x00, 0x1234);
    CHECK(mi.read16(0x00) == 0x1234);
    mi.write32(0x04, 0xAAAA5555);
    CHECK(mi.read32(0x04) == 0xAAAA5555);
    mi.raise(1);
    CHECK_FALSE(irq);
    mi.write16(0x1C, 0x2);
    CHECK(irq);
    CHECK(mi.read16(0x1E) == 0x12);
    mi.write16(0x1E, 0x12);
    CHECK_FALSE(irq);
    CHECK(mi.read32(0x24) == 0);
    CHECK_THROWS_AS(mi.read16(0x32), ezy::gc::BusError);
}

// The real game, only on a machine that has it: the boot through the machine loop reaches
// the stop the plain CPU loop reached (the unmodeled MMCR0 SPR, Task 7), or a hardware block
// Tasks 5-6 add. Where it stops is reported, not checked.
TEST_CASE("the MKDD boot through run_field reports where it stops") {
    const std::filesystem::path path = "E:/Rom nintendo/Mario Kart Double Dash/Mario Kart - Double Dash!! (USA).nkit.iso";
    if (!std::filesystem::exists(path)) {
        MESSAGE("MKDD image not found; skipped: " << path.string());
        return;
    }
    auto gc = GameCube::create(path);
    REQUIRE(gc != nullptr);
    try {
        for (int i = 0; i < 5; ++i) gc->run_field();
        MESSAGE("MKDD ran 5 fields; pc " << gc->cpu().pc());
    } catch (const ezy::gc::GekkoError& e) {
        MESSAGE("MKDD stopped after " << gc->stats().instructions << " instructions: " << std::string(e.what()));
    }
    CHECK(gc->stats().instructions > 1000);
}
