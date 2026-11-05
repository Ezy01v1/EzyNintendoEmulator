#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "gba/bus.hpp"
#include "gba/ppu.hpp"
#include "gba/scheduler.hpp"

using namespace ezy::gba;

namespace {

// A bus with the PPU running, driven by hand: advance time and dispatch PPU events.
struct Fixture {
    Scheduler scheduler;
    Bus bus{std::vector<std::uint8_t>(0x200), {}, &scheduler};

    Fixture() { bus.ppu().start(); }

    void run_to(std::uint64_t cycle) {
        scheduler.advance(cycle - scheduler.now());
        while (const auto e = scheduler.pop_due()) {
            if (e->type == EventType::PpuHDrawEnd) bus.ppu().on_hdraw_end(e->when);
            if (e->type == EventType::PpuHBlankEnd) bus.ppu().on_hblank_end(e->when);
        }
    }
    void run_for(std::uint64_t cycles) { run_to(scheduler.now() + cycles); }
    [[nodiscard]] std::uint16_t dispstat() const { return bus.read16(0x04000004); }
    [[nodiscard]] std::uint16_t vcount() const { return bus.read16(0x04000006); }
    [[nodiscard]] std::uint16_t pixel(int x, int y) const {
        return bus.ppu().framebuffer()[static_cast<std::size_t>(y * kScreenWidth + x)];
    }
};

}  // namespace

TEST_CASE("PPU HBlank flag follows the 960/272 split") {
    Fixture f;
    f.run_to(959);
    CHECK((f.dispstat() & 0x2) == 0);
    f.run_to(960);
    CHECK((f.dispstat() & 0x2) != 0);
    f.run_to(1231);
    CHECK((f.dispstat() & 0x2) != 0);
    CHECK(f.vcount() == 0);
    f.run_to(1232);
    CHECK((f.dispstat() & 0x2) == 0);
    CHECK(f.vcount() == 1);
}

TEST_CASE("PPU VCOUNT wraps after 227 and VBlank covers lines 160-226") {
    Fixture f;
    f.run_to(159ULL * kCyclesPerLine);
    CHECK(f.vcount() == 159);
    CHECK((f.dispstat() & 0x1) == 0);
    f.run_to(160ULL * kCyclesPerLine);
    CHECK(f.vcount() == 160);
    CHECK((f.dispstat() & 0x1) != 0);
    CHECK(f.bus.ppu().take_frame_done());
    CHECK_FALSE(f.bus.ppu().take_frame_done());
    f.run_to(227ULL * kCyclesPerLine);
    CHECK(f.vcount() == 227);
    CHECK((f.dispstat() & 0x1) == 0);  // line 227 is not flagged
    f.run_to(228ULL * kCyclesPerLine);
    CHECK(f.vcount() == 0);
}

TEST_CASE("PPU status bits are read-only, LYC and IRQ enables are writable") {
    Fixture f;
    f.run_to(960);  // in HBlank
    f.bus.write16(0x04000004, 0xFF38);  // try to clear HBlank, set enables + LYC=0xFF
    CHECK(f.dispstat() == (0xFF38 | 0x2));
    f.bus.write16(0x04000006, 0x0055);  // VCOUNT is read-only
    CHECK(f.vcount() == 0);
}

TEST_CASE("PPU interrupts fire only when enabled in DISPSTAT") {
    Fixture f;
    f.run_to(160ULL * kCyclesPerLine);
    CHECK(f.bus.interrupt_flags() == 0);  // nothing enabled yet

    f.bus.write16(0x04000004, 0x0008 | 0x0010 | 0x0020 | (5U << 8));  // VBlank, HBlank, VCount (LYC 5)
    f.run_to(228ULL * kCyclesPerLine + 5 * kCyclesPerLine);  // next frame, line 5
    CHECK((f.bus.interrupt_flags() & irq::kVCount) != 0);
    CHECK((f.dispstat() & 0x4) != 0);
    CHECK((f.bus.interrupt_flags() & irq::kHBlank) != 0);
    f.run_to(228ULL * kCyclesPerLine + 160 * kCyclesPerLine);
    CHECK((f.bus.interrupt_flags() & irq::kVBlank) != 0);
}

TEST_CASE("IF: writing 1 acknowledges an interrupt, writing 0 leaves it") {
    Fixture f;
    f.bus.request_interrupt(irq::kVBlank | irq::kHBlank);
    f.bus.write16(0x04000202, irq::kHBlank);
    CHECK(f.bus.interrupt_flags() == irq::kVBlank);
    f.bus.write16(0x04000202, 0);
    CHECK(f.bus.interrupt_flags() == irq::kVBlank);
    f.bus.write32(0x04000200, 0x00010003);  // IE=3 and ack VBlank in one word
    CHECK(f.bus.interrupt_enable() == 3);
    CHECK(f.bus.interrupt_flags() == 0);
}

TEST_CASE("KEYINPUT is active-low and read-only") {
    Fixture f;
    CHECK(f.bus.read16(0x04000130) == 0x03FF);
    f.bus.set_keys(0x0009);  // A + Start
    CHECK(f.bus.read16(0x04000130) == 0x03F6);
    f.bus.write16(0x04000130, 0);
    CHECK(f.bus.read16(0x04000130) == 0x03F6);
}

TEST_CASE("PPU mode 3 draws direct colors") {
    Fixture f;
    f.bus.write16(0x04000000, 0x0403);  // mode 3, BG2 on
    f.bus.write16(0x06000000 + (10 * 240 + 7) * 2, 0x001F);  // red at (7, 10)
    f.run_to(11ULL * kCyclesPerLine);
    CHECK(f.pixel(7, 10) == 0x001F);
    CHECK(f.pixel(8, 10) == 0x0000);
}

TEST_CASE("PPU mode 4 uses the palette and the page bit") {
    Fixture f;
    f.bus.write16(0x05000000 + 3 * 2, 0x03E0);  // palette[3] = green
    f.bus.write16(0x05000000 + 4 * 2, 0x7C00);  // palette[4] = blue
    f.bus.write8(0x06000000 + 2 * 240 + 1, 3);  // byte writes go to both halves: (0..1, 2) = 3
    f.bus.write8(0x0600A000 + 2 * 240 + 1, 4);  // page 1
    f.bus.write16(0x04000000, 0x0404);          // mode 4, page 0
    f.run_to(3ULL * kCyclesPerLine);
    CHECK(f.pixel(1, 2) == 0x03E0);
    f.bus.write16(0x04000000, 0x0414);  // page 1, from the next frame on
    f.run_to(228ULL * kCyclesPerLine + 3 * kCyclesPerLine);
    CHECK(f.pixel(1, 2) == 0x7C00);
}

TEST_CASE("PPU mode 5 is 160x128 with the backdrop around it") {
    Fixture f;
    f.bus.write16(0x05000000, 0x1234);  // backdrop
    f.bus.write16(0x06000000 + (0 * 160 + 159) * 2, 0x7FFF);
    f.bus.write16(0x04000000, 0x0405);
    f.run_to(1ULL * kCyclesPerLine);
    CHECK(f.pixel(159, 0) == 0x7FFF);
    CHECK(f.pixel(160, 0) == 0x1234);
}

TEST_CASE("PPU forced blank is white and BG2 off shows the backdrop") {
    Fixture f;
    f.bus.write16(0x05000000, 0x0421);
    f.bus.write16(0x04000000, 0x0083);  // mode 3, forced blank
    f.run_to(1ULL * kCyclesPerLine);
    CHECK(f.pixel(0, 0) == 0x7FFF);
    f.bus.write16(0x04000000, 0x0003);  // mode 3, BG2 off
    f.run_to(2ULL * kCyclesPerLine);
    CHECK(f.pixel(0, 1) == 0x0421);
}
