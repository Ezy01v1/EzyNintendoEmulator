#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "disc_builder.hpp"
#include "gc/dsp.hpp"
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

    // Dolphin's Init(): the reset switch released (bit 16) and the VI cause set.
    CHECK(pi.read32(0x00) == ((1u << 16) | (1u << Pi::kVi)));
    pi.write32(0x00, 1u << Pi::kVi);  // any cause clears when the CPU writes a 1
    CHECK(pi.read32(0x00) == (1u << 16));

    pi.set_cause(Pi::kDi, true);  // masked: no line
    CHECK_FALSE(pi.line());
    CHECK(levels.empty());
    pi.write32(0x04, 1u << Pi::kDi);  // enabling it raises the line
    CHECK(pi.line());
    CHECK(pi.read32(0x00) == ((1u << Pi::kDi) | (1u << 16)));
    pi.set_cause(Pi::kDi, false);  // the peripheral acknowledges
    CHECK_FALSE(pi.line());
    CHECK(levels == std::vector<bool>{true, false});

    // The CPU acknowledges by writing a 1 to the bit (a live cause comes back at the
    // peripheral's next set_cause).
    pi.set_cause(Pi::kDi, true);
    CHECK(pi.line());
    pi.write32(0x00, 1u << Pi::kDi);
    CHECK_FALSE(pi.line());
    CHECK(pi.interrupts(Pi::kDi) == 2);

    // The mask only holds bits 0-13; the reset switch state is not an interrupt.
    pi.write32(0x04, 0xFFFFFFFF);
    CHECK(pi.read32(0x04) == 0x3FFF);
    CHECK_FALSE(pi.line());

    // The revision is the literal Flipper rev C, the reset code is stored, the FIFO
    // pointers are physical and 32-byte aligned, a 16-bit write is invalid, as is an
    // unmodeled register.
    CHECK(pi.read32(0x2C) == 0x246500B1u);
    pi.write32(0x24, 0x12345678);
    CHECK(pi.read32(0x24) == 0x12345678);
    CHECK(pi.read16(0x26) == 0x5678);
    pi.write32(0x14, 0x80010029);
    CHECK(pi.fifo_write_pointer() == 0x00010020);
    pi.write32(0x0C, 0xFFFFFFFF);
    CHECK(pi.fifo_base() == 0x1FFFFFE0);
    CHECK_THROWS_AS(pi.write16(0x24, 1), ezy::gc::BusError);
    CHECK_THROWS_AS(pi.read32(0x40), ezy::gc::BusError);
    CHECK_THROWS_AS(pi.write32(0x2C, 0), ezy::gc::BusError);
}

TEST_CASE("VI starts as Dolphin's Preset() and checks line 1 at the start of the first field") {
    Scheduler sched;
    Pi pi;
    Vi vi(sched, pi);
    CHECK(vi.read16(0x00) == 6);  // VTR: EQU
    CHECK(vi.read16(0x02) == 1);  // DCR: ENB
    CHECK(vi.read32(0x30) == (((0x1000u | 263u) << 16) | 430u));  // DI0: line 263, HCT 430, enabled
    // DI1 = line 1, HCT 1 (the first half-line): pending from the very first half-line.
    CHECK(vi.read32(0x34) == (((0x8000u | 0x1000u | 1u) << 16) | 1u));
    CHECK(vi.read16(0x48) == ((40 << 8) | 40));  // STD = WPL = 40
}

TEST_CASE("VI display interrupt fires at its line, once per frame, and clears when written") {
    Scheduler sched;
    Pi pi;
    Vi vi(sched, pi);
    vi.write32(0x34, 0);  // DI1 off (it starts pending); DI0's preset line 263 is overwritten below
    pi.write32(0x00, 1u << Pi::kVi);
    pi.write32(0x04, 1u << Pi::kVi);
    CHECK_FALSE(pi.line());
    vi.write32(0x30, 0x10000000 | (100u << 16));  // DI0: enabled, VCT = 100, HCT = 0
    CHECK(vi.read32(0x30) == (0x10000000u | (100u << 16)));

    constexpr std::uint64_t kLine = 30888;  // 858 samples * 36 CPU cycles
    run_to(sched, vi, 99 * kLine - 1);      // line 100 starts 99 lines in (lines count from 1)
    CHECK_FALSE(pi.line());
    run_to(sched, vi, 99 * kLine);
    CHECK(pi.line());
    CHECK((vi.read32(0x30) & 0x80000000u) != 0);
    CHECK(vi.read16(0x2C) == 100);  // DPV: the line being scanned

    // The SDK acknowledges by writing the register back with bit 31 clear. The register is
    // per frame: line 100 comes once every two fields, not every field.
    vi.write32(0x30, 0x10000000 | (100u << 16));
    CHECK((vi.read32(0x30) & 0x80000000u) == 0);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, kField + 99 * kLine + 1);  // the second field's 100th line is line 363
    CHECK_FALSE(pi.line());
    run_to(sched, vi, 2 * kField + 99 * kLine);  // the next frame
    CHECK(pi.line());
    vi.write32(0x30, 0);  // acknowledge and disable DI0 for the rest of the test
    CHECK_FALSE(pi.line());

    // Lines past 263 exist (the frame has 525): DI2 at line 300, and DPV reaches 525.
    const std::uint64_t frame2 = 2 * kField;
    vi.write32(0x38, 0x10000000 | (300u << 16));
    run_to(sched, vi, frame2 + 299 * kLine - 1);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, frame2 + 299 * kLine);
    CHECK(pi.line());
    CHECK(vi.read16(0x2C) == 300);
    vi.write32(0x38, 0);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, frame2 + 524 * kLine);
    CHECK(vi.read16(0x2C) == 525);

    // HCT above the half-line width selects the second half-line of the line.
    const std::uint64_t frame3 = 2 * frame2;
    vi.write32(0x3C, ((0x1000u | 200u) << 16) | 430u);  // DI3: enabled, line 200, HCT 430 > HLW 429
    run_to(sched, vi, frame3 + 199 * kLine + Vi::kCyclesPerHalfLine - 1);
    CHECK_FALSE(pi.line());
    run_to(sched, vi, frame3 + 199 * kLine + Vi::kCyclesPerHalfLine);
    CHECK(pi.line());
    vi.write32(0x3C, 0);

    // The status sets even when the interrupt is not enabled; it only gates the PI line.
    vi.write32(0x30, 150u << 16);
    run_to(sched, vi, 2 * frame3 + 149 * kLine);
    CHECK((vi.read32(0x30) & 0x80000000u) != 0);
    CHECK_FALSE(pi.line());
    vi.write32(0x30, 0x80000000u | 0x10000000u | (150u << 16));  // the high half is stored as written
    CHECK(pi.line());

    // DCR.RST clears the four DI registers (INT included) and is not stored.
    vi.write16(0x02, 0x0003);
    CHECK(vi.read16(0x02) == 0x0001);
    CHECK(vi.read32(0x30) == 0);
    CHECK(vi.read32(0x38) == 0);
    CHECK_FALSE(pi.line());

    // A register the VI does not model throws; so does a write to the read-only beam position.
    CHECK_THROWS_AS(vi.read32(0x80), ezy::gc::BusError);
    CHECK_THROWS_AS(vi.write16(0x2C, 1), ezy::gc::BusError);
}

TEST_CASE("VI field timing gives 59.94 fields per second of CPU cycles") {
    DiscBuilder builder;
    const auto image = builder.write("vi_timing");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    const std::uint64_t tb_before = gc->cpu().state().tb;
    constexpr int fields = 3;  // odd: the time base's remainder is carried across fields
    for (int i = 0; i < fields; ++i) gc->run_field();
    CHECK(gc->cycles() == fields * kField);
    CHECK(gc->stats().fields == fields);
    // The machine's own clock, in seconds at 486 MHz: 59.94 fields per second.
    const double seconds = static_cast<double>(gc->cycles()) / 486e6;
    CHECK(fields / seconds == doctest::Approx(59.94).epsilon(0.0002));
    // The time base ticked once per 12 CPU cycles, remainder carried.
    CHECK(gc->cpu().state().tb - tb_before == fields * kField / 12);
    CHECK(gc->stats().instructions == fields * kField);  // the synthetic program spins at 1 cycle each
}

TEST_CASE("the VI stops on a video format whose timing is not modeled") {
    DiscBuilder builder;
    const auto image = builder.write("vi_format");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    const auto expect_stop = [&](auto&& write, const char* word) {
        try {
            write();
            FAIL("expected a BusError");
        } catch (const ezy::gc::BusError& e) {
            CHECK(std::string(e.what()).find(word) != std::string::npos);
        }
    };
    // DCR.FMT: PAL, MPAL and debug (NTSC = 0 is the only one modeled).
    for (std::uint16_t fmt = 1; fmt <= 3; ++fmt)
        expect_stop([&] { mem.write16(kVi + 0x02, static_cast<std::uint16_t>(0x0001 | (fmt << 8))); }, "FMT");
    expect_stop([&] { mem.write16(kVi + 0x02, 0x0005); }, "NIN");   // non-interlaced (263-line fields)
    expect_stop([&] { mem.write16(kVi + 0x6C, 0x0001); }, "VICLK");  // 54 MHz (progressive)
    // A refused write changes nothing; NTSC interlaced and VICLK = 0 are accepted.
    CHECK(mem.read16(kVi + 0x02) == 0x0001);
    CHECK(mem.read16(kVi + 0x6C) == 0);
    mem.write16(kVi + 0x02, 0x0001);
    mem.write16(kVi + 0x6C, 0x0000);
    // A 32-bit write whose low half is refused does not apply its high half.
    mem.write16(kVi + 0x00, 0x0006);
    CHECK_THROWS_AS(mem.write32(kVi + 0x00, (240u << 4 << 16) | 0x0005), ezy::gc::BusError);
    CHECK(mem.read16(kVi + 0x00) == 0x0006);
}

TEST_CASE("the machine delivers the VI interrupt to the CPU through the PI") {
    DiscBuilder builder;
    const auto image = builder.write("vi_irq");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    mem.write32(kVi + 0x34, 0);  // DI1 starts pending
    mem.write32(kPi + 0x00, 1u << Pi::kVi);
    mem.write32(kPi + 0x04, 1u << Pi::kVi);
    CHECK_FALSE(gc->cpu().state().external_interrupt);
    mem.write32(kVi + 0x30, 0x10000000 | (1u << 16));  // DI0: enable, line 1
    gc->run_field();
    gc->run_field();
    CHECK((mem.read32(kPi + 0x00) & (1u << Pi::kVi)) != 0);
    CHECK(gc->cpu().state().external_interrupt);  // the line reached the Gekko
    CHECK(gc->stats().interrupts[Pi::kVi] >= 1);
    mem.write32(kVi + 0x30, 0x10000000 | (1u << 16));  // acknowledge
    CHECK((mem.read32(kPi + 0x00) & (1u << Pi::kVi)) == 0);
    CHECK_FALSE(gc->cpu().state().external_interrupt);
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

TEST_CASE("the XFB picture interleaves the two fields") {
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

    // The shift bit scales the address (non-interlaced 240p is refused: see the format test).
    mem.write32(kVi + 0x1C, (1u << 28) | (bottom >> 5));  // shifted: address = FBB << 5
    picture = gc->xfb();
    CHECK(picture.argb[0] == 0xFFFFFFFFu);  // the top field now reads the white XFB

    // CLRPOFF clears the shift; the bottom register uses the top register's POFF.
    mem.write32(kVi + 0x1C, (1u << 28) | (1u << 29) | (bottom >> 5));
    CHECK(gc->vi().xfb_top() == (bottom >> 5));
    mem.write32(kVi + 0x1C, (1u << 28) | (top >> 5));
    mem.write32(kVi + 0x24, bottom >> 5);  // no POFF of its own
    CHECK(gc->vi().xfb_top() == top);
    CHECK(gc->vi().xfb_bottom() == bottom);
}

TEST_CASE("the XFB picture takes its line stride from STD (one interleaved double-field buffer)") {
    DiscBuilder builder;
    const auto image = builder.write("xfb_stride");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    auto ram = mem.mem1();
    constexpr std::uint32_t fb = 0x00100000;
    // The SDK's 480i double-field XFB: both fields in one 640x480 buffer, 1280 bytes per
    // picture line, so a field line is 2560 bytes apart (STD = 2 * WPL); bottom = top + 1280.
    for (std::uint32_t row = 0; row < 480; ++row) {
        const std::uint8_t red[4] = {81, 90, 81, 240}, white[4] = {235, 128, 235, 128};
        const std::uint8_t* c = (row & 1) ? white : red;
        for (std::uint32_t i = 0; i < 1280; i += 4) std::copy(c, c + 4, ram.begin() + fb + row * 1280 + i);
    }
    mem.write16(kVi + 0x00, 240 << 4);
    mem.write16(kVi + 0x02, 0x0001);
    mem.write16(kVi + 0x48, (40 << 8) | 80);  // WPL = 40, STD = 80
    mem.write32(kVi + 0x1C, fb);
    mem.write32(kVi + 0x24, fb + 1280);
    const auto picture = gc->xfb();
    REQUIRE(picture.height == 480);
    for (int row : {0, 2, 100, 478}) CHECK(picture.argb[static_cast<std::size_t>(row) * 640 + 5] == 0xFFFF0000u);
    for (int row : {1, 3, 101, 479}) CHECK(picture.argb[static_cast<std::size_t>(row) * 640 + 634] == 0xFFFFFFFFu);
}

TEST_CASE("the XFB picture is WPL * 16 pixels wide (MKDD's 608-pixel XFB)") {
    DiscBuilder builder;
    const auto image = builder.write("xfb_wpl");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    auto& mem = gc->memory();
    auto ram = mem.mem1();
    constexpr std::uint32_t fb = 0x00100000;
    // A 608x448 XFB as MKDD's EFB copy writes it (stride 1216 bytes), red, followed by white
    // bytes that a 640-pixel line would wrongly show at the end of the last line.
    const std::uint8_t red[4] = {81, 90, 81, 240}, white[4] = {235, 128, 235, 128};
    for (std::uint32_t i = 0; i < 448 * 1216; i += 4) std::copy(red, red + 4, ram.begin() + fb + i);
    for (std::uint32_t i = 0; i < 64; i += 4) std::copy(white, white + 4, ram.begin() + fb + 448 * 1216 + i);
    mem.write16(kVi + 0x00, 224 << 4);
    mem.write16(kVi + 0x02, 0x0001);
    mem.write16(kVi + 0x48, (38 << 8) | 76);  // WPL = 38 (608 pixels), STD = 76
    mem.write32(kVi + 0x1C, fb);
    mem.write32(kVi + 0x24, fb + 1216);
    const auto picture = gc->xfb();
    CHECK(picture.width == 608);
    REQUIRE(picture.height == 448);
    REQUIRE(picture.argb.size() == 608u * 448u);
    CHECK(picture.argb.back() == 0xFFFF0000u);  // the last line ends red, at pixel 607
    CHECK(picture.argb[447u * 608u] == 0xFFFF0000u);
    // A line wider than 640 pixels (WPL > 40, e.g. 720 = 45) is not modeled: the write stops
    // with the address and leaves the register as it was.
    CHECK_THROWS_AS(mem.write16(kVi + 0x48, (45 << 8) | 90), BusError);
    CHECK_THROWS_AS(mem.write32(kVi + 0x48, ((41u << 8) | 82u) << 16), BusError);
    CHECK(mem.read16(kVi + 0x48) == ((38 << 8) | 76));
    mem.write16(kVi + 0x48, (40 << 8) | 80);  // 640 is fine
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
    mem.write16(0x0C004010, 0xFFFF);  // MI protection type
    CHECK(mem.read32(kPi + 0x24) == 0xDEADBEEF);
    CHECK(mem.read16(kVi + 0x70) == 0x1234);
    CHECK(mem.read32(kPi + 0x2C) == 0x246500B1u);
    CHECK(std::equal(before.begin(), before.end(), mem.mem1().begin()));
    // The same offsets in RAM stay what they were (0x3024 is not a mirror of the register).
    CHECK(mem.read32(0x3024) == (std::uint32_t{before[0x3024]} << 24 | std::uint32_t{before[0x3025]} << 16 |
                                 std::uint32_t{before[0x3026]} << 8 | before[0x3027]));

    // An address between the register blocks and the pipe still stops the machine; so do
    // 8-bit accesses and a register in a registered block that nobody models.
    CHECK_THROWS_AS(mem.read32(0x0C007000), ezy::gc::BusError);  // past the AI block, before the pipe
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
    mi.write16(0x24, 0xBEEF);  // PROT_ADDR_HI is writable
    CHECK(mi.read16(0x24) == 0xBEEF);
    CHECK_THROWS_AS(mi.read16(0x26), ezy::gc::BusError);
    CHECK_THROWS_AS(mi.read16(0x12), ezy::gc::BusError);
    CHECK_THROWS_AS(mi.read16(0x32), ezy::gc::BusError);
}

// The real game, only on a machine that has it. Since Task 7 the boot runs into the game's
// main loop (docs/progress/2026-11-21-gamecube-mkdd-boot.md); the first 5 fields (40 million
// instructions, a few seconds in Debug) must run without a stop and boot the DSP microcode
// this HLE knows. Where a regression stops it is reported.
TEST_CASE("the MKDD boot through run_field runs its first fields and boots its DSP microcode") {
    const std::filesystem::path path = "E:/Rom nintendo/Mario Kart Double Dash/Mario Kart - Double Dash!! (USA).nkit.iso";
    if (!std::filesystem::exists(path)) {
        MESSAGE("MKDD image not found; skipped: " << path.string());
        return;
    }
    auto gc = GameCube::create(path);
    REQUIRE(gc != nullptr);
    bool stopped = false;
    try {
        for (int i = 0; i < 5; ++i) gc->run_field();
    } catch (const ezy::gc::GekkoError& e) {
        stopped = true;
        MESSAGE("MKDD stopped after " << gc->stats().instructions << " instructions: " << std::string(e.what()));
    }
    CHECK_FALSE(stopped);
    CHECK(gc->stats().fields == 5);
    REQUIRE(gc->dsp().microcode().has_value());
    CHECK(gc->dsp().microcode()->crc == 0x2FCDF1EC);  // JAudio's "Zelda" microcode (dsp_zelda.hpp)
}
