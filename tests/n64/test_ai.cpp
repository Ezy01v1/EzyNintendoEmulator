#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "n64/ai.hpp"
#include "n64/mi.hpp"
#include "n64/scheduler.hpp"

using namespace ezy::n64;

namespace {

constexpr std::uint32_t kDram = 0x00, kLen = 0x04, kControl = 0x08, kStatus = 0x0C, kDacrate = 0x10;

struct AiFixture {
    Scheduler scheduler;
    Mi mi;
    Ai ai;
    std::vector<std::uint8_t> rdram = std::vector<std::uint8_t>(0x10000, 0);

    AiFixture() {
        mi.write(0x0C, 1U << 5);  // unmask AI
        ai.write(kControl, 1, mi, scheduler, rdram);
        ai.write(kDacrate, 1105, mi, scheduler, rdram);
    }
    void finish_current() {
        scheduler.advance_to(*scheduler.next_event_time());
        const auto due = scheduler.pop_due();
        ai.on_dma_end(mi, scheduler, rdram);
        (void)due;
    }
};

}  // namespace

TEST_CASE("AI_LEN write starts the DMA and interrupts") {
    AiFixture f;
    f.ai.write(kDram, 0x1000, f.mi, f.scheduler, f.rdram);
    f.ai.write(kLen, 0x400, f.mi, f.scheduler, f.rdram);
    CHECK(f.mi.line());
    CHECK((f.ai.read(kStatus) & (1U << 30)) != 0);  // busy
    CHECK((f.ai.read(kStatus) & (1U << 31)) == 0);  // not full
    CHECK(f.scheduler.pending(EventType::AiDma));
}

TEST_CASE("second buffer waits and sets FULL") {
    AiFixture f;
    f.ai.write(kDram, 0x1000, f.mi, f.scheduler, f.rdram);
    f.ai.write(kLen, 0x400, f.mi, f.scheduler, f.rdram);
    f.ai.write(kStatus, 0, f.mi, f.scheduler, f.rdram);  // acknowledge
    CHECK_FALSE(f.mi.line());
    f.ai.write(kDram, 0x2000, f.mi, f.scheduler, f.rdram);
    f.ai.write(kLen, 0x400, f.mi, f.scheduler, f.rdram);
    CHECK((f.ai.read(kStatus) & (1U << 31)) != 0);
    CHECK_FALSE(f.mi.line());
    f.finish_current();  // the second buffer starts: interrupt again
    CHECK(f.mi.line());
    CHECK((f.ai.read(kStatus) & (1U << 31)) == 0);
    f.finish_current();
    CHECK((f.ai.read(kStatus) & (1U << 30)) == 0);  // idle
}

TEST_CASE("buffer length in cycles follows DACRATE") {
    AiFixture f;
    f.ai.write(kDram, 0x1000, f.mi, f.scheduler, f.rdram);
    f.ai.write(kLen, 0x400, f.mi, f.scheduler, f.rdram);  // 256 stereo samples
    const double rate = 48'681'812.0 / (1105 + 1);
    CHECK(f.ai.sample_rate() == static_cast<int>(rate));
    const double expected = 256 * 93'750'000.0 / rate;
    CHECK(static_cast<double>(*f.scheduler.next_event_time()) == doctest::Approx(expected).epsilon(0.001));
}

TEST_CASE("samples come out as stereo floats") {
    AiFixture f;
    f.rdram[0x1000] = 0x40, f.rdram[0x1001] = 0x00;  // left  +16384
    f.rdram[0x1002] = 0xC0, f.rdram[0x1003] = 0x00;  // right -16384
    f.ai.write(kDram, 0x1000, f.mi, f.scheduler, f.rdram);
    f.ai.write(kLen, 8, f.mi, f.scheduler, f.rdram);
    REQUIRE(f.ai.samples().size() == 4);
    CHECK(f.ai.samples()[0] == doctest::Approx(0.5f));
    CHECK(f.ai.samples()[1] == doctest::Approx(-0.5f));
    f.ai.clear_samples();
    CHECK(f.ai.samples().empty());
}

TEST_CASE("AI_LEN with DMA disabled does nothing") {
    AiFixture f;
    f.ai.write(kControl, 0, f.mi, f.scheduler, f.rdram);
    f.ai.write(kLen, 0x400, f.mi, f.scheduler, f.rdram);
    CHECK_FALSE(f.mi.line());
    CHECK_FALSE(f.scheduler.pending(EventType::AiDma));
}
