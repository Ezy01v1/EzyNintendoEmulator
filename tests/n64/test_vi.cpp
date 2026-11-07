#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "n64/mi.hpp"
#include "n64/scheduler.hpp"
#include "n64/vi.hpp"

using namespace ezy::n64;

namespace {

constexpr std::uint32_t kCtrl = 0x00, kOrigin = 0x04, kWidth = 0x08, kVIntr = 0x0C, kVCurrent = 0x10, kVSync = 0x18,
                        kHVideo = 0x24, kVVideo = 0x28, kXScale = 0x30, kYScale = 0x34;

struct ViFixture {
    Scheduler scheduler;
    Mi mi;
    Vi vi;

    explicit ViFixture(Region region = Region::Ntsc) {
        vi.set_region(region);
        vi.start(scheduler);
    }
    // Dispatches VI events until `pred` holds or `limit` events ran; returns the count.
    template <typename P>
    int run_until(P pred, int limit = 2000) {
        for (int n = 0; n < limit; ++n) {
            if (pred()) return n;
            scheduler.advance_to(*scheduler.next_event_time());
            const auto due = scheduler.pop_due();
            vi.on_line(mi, scheduler, due->when);
        }
        return -1;
    }
};

}  // namespace

TEST_CASE("V_CURRENT reaching V_INTR raises the VI interrupt") {
    ViFixture f;
    f.vi.write(kVIntr, 10);
    f.mi.write(0x0C, 1U << 7);  // unmask VI
    REQUIRE(f.run_until([&] { return f.vi.read(kVCurrent) == 8; }) >= 0);
    CHECK_FALSE(f.mi.line());
    f.run_until([&] { return f.vi.read(kVCurrent) == 10; });
    CHECK(f.mi.line());
}

TEST_CASE("writing V_CURRENT acknowledges the interrupt") {
    ViFixture f;
    f.vi.write(kVIntr, 4);
    f.mi.write(0x0C, 1U << 7);
    f.run_until([&] { return f.mi.line(); });
    f.vi.write(kVCurrent, 0, f.mi);
    CHECK_FALSE(f.mi.line());
}

TEST_CASE("NTSC field lasts 93.75 MHz / 59.94 Hz cycles") {
    ViFixture f;
    f.vi.write(kVSync, 525);
    f.run_until([&] { return f.vi.take_vblank(); });
    const std::uint64_t start = f.scheduler.now();
    f.run_until([&] { return f.vi.take_vblank(); });
    const double expected = 93'750'000.0 / (60.0 / 1.001);
    CHECK(static_cast<double>(f.scheduler.now() - start) == doctest::Approx(expected).epsilon(0.0001));
}

TEST_CASE("PAL field runs at 50 Hz") {
    ViFixture f(Region::Pal);
    f.vi.write(kVSync, 625);
    f.run_until([&] { return f.vi.take_vblank(); });
    const std::uint64_t start = f.scheduler.now();
    f.run_until([&] { return f.vi.take_vblank(); });
    CHECK(static_cast<double>(f.scheduler.now() - start) == doctest::Approx(93'750'000.0 / 50.0).epsilon(0.0001));
}

TEST_CASE("image decodes RGBA5551 from VI_ORIGIN") {
    Vi vi;
    std::vector<std::uint8_t> rdram(0x10000, 0);
    // A 4x2 framebuffer at 0x1000, stride 4: pixel (1, 1) is pure red, (2, 0) pure blue.
    rdram[0x1000 + (1 * 4 + 1) * 2] = 0xF8;
    rdram[0x1000 + (1 * 4 + 1) * 2 + 1] = 0x01;
    rdram[0x1000 + 2 * 2] = 0x00;
    rdram[0x1000 + 2 * 2 + 1] = 0x3F;
    vi.write(kCtrl, 2);
    vi.write(kOrigin, 0x1000);
    vi.write(kWidth, 4);
    vi.write(kHVideo, (100 << 16) | 104);   // 4 screen pixels
    vi.write(kVVideo, (20 << 16) | 24);     // 2 lines (in half-lines)
    vi.write(kXScale, 0x400);
    vi.write(kYScale, 0x400);
    const Vi::Image img = vi.image(rdram);
    REQUIRE(img.width == 4);
    REQUIRE(img.height == 2);
    CHECK(img.argb[1 * 4 + 1] == 0xFFFF0000);
    CHECK(img.argb[2] == 0xFF0000FF);
    CHECK(img.argb[0] == 0xFF000000);
}

TEST_CASE("image decodes RGBA8888") {
    Vi vi;
    std::vector<std::uint8_t> rdram(0x10000, 0);
    rdram[0x2000 + 4] = 0x12, rdram[0x2000 + 5] = 0x34, rdram[0x2000 + 6] = 0x56, rdram[0x2000 + 7] = 0xFF;
    vi.write(kCtrl, 3);
    vi.write(kOrigin, 0x2000);
    vi.write(kWidth, 2);
    vi.write(kHVideo, (100 << 16) | 102);
    vi.write(kVVideo, (20 << 16) | 22);
    vi.write(kXScale, 0x400);
    vi.write(kYScale, 0x400);
    const Vi::Image img = vi.image(rdram);
    REQUIRE(img.width == 2);
    CHECK(img.argb[1] == 0xFF123456);
}

TEST_CASE("blank type gives an empty image") {
    Vi vi;
    std::vector<std::uint8_t> rdram(0x100, 0);
    vi.write(kCtrl, 0);
    CHECK(vi.image(rdram).width == 0);
    CHECK(vi.image(rdram).argb.empty());
}
