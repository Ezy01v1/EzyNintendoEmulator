#include <doctest/doctest.h>

#include "gb/timer.hpp"

using ezy::gb::Timer;

TEST_CASE("DIV is the upper byte of the counter and resets on write") {
    Timer timer;
    timer.write(0xFF04, 0x12);
    CHECK(timer.read(0xFF04) == 0);
    for (int i = 0; i < 64; ++i) timer.tick();  // 64 M-cycles = 256 T-cycles
    CHECK(timer.read(0xFF04) == 1);
}

TEST_CASE("TIMA counts at the selected rate only when enabled") {
    Timer timer;
    timer.write(0xFF04, 0);
    timer.write(0xFF07, 0x01);  // 262144 Hz (every 4 M-cycles) but disabled
    for (int i = 0; i < 16; ++i) timer.tick();
    CHECK(timer.read(0xFF05) == 0);

    timer.write(0xFF04, 0);
    timer.write(0xFF07, 0x05);  // enabled
    for (int i = 0; i < 16; ++i) timer.tick();
    CHECK(timer.read(0xFF05) == 4);
}

TEST_CASE("TIMA overflow reloads TMA and requests an interrupt") {
    Timer timer;
    timer.write(0xFF04, 0);
    timer.write(0xFF06, 0xF0);
    timer.write(0xFF05, 0xFF);
    timer.write(0xFF07, 0x05);
    CHECK_FALSE(timer.take_interrupt());
    for (int i = 0; i < 5; ++i) timer.tick();  // overflow, then one cycle of delay
    CHECK(timer.read(0xFF05) == 0xF0);
    CHECK(timer.take_interrupt());
    CHECK_FALSE(timer.take_interrupt());  // reported only once
}

TEST_CASE("writing DIV while the selected bit is high bumps TIMA (falling edge)") {
    Timer timer;
    timer.write(0xFF04, 0);
    timer.write(0xFF07, 0x05);  // bit 3
    timer.tick();
    timer.tick();  // counter = 8: bit 3 is high
    timer.write(0xFF04, 0);
    CHECK(timer.read(0xFF05) == 1);
}

namespace {

// TIMA = 0xFF at the fastest rate (+1 every 4 M-cycles), TMA = 0x42: the next increment
// overflows. Returns after the M-cycle in which TIMA overflowed.
Timer timer_at_overflow() {
    Timer timer;
    timer.write(0xFF04, 0);
    timer.write(0xFF06, 0x42);
    timer.write(0xFF05, 0xFF);
    timer.write(0xFF07, 0x05);
    for (int i = 0; i < 4; ++i) timer.tick();
    return timer;
}

}  // namespace

TEST_CASE("TIMA reads 0 for one cycle after overflow, then TMA") {
    Timer timer = timer_at_overflow();
    CHECK(timer.read(0xFF05) == 0x00);
    CHECK_FALSE(timer.take_interrupt());
    timer.tick();
    CHECK(timer.read(0xFF05) == 0x42);
    CHECK(timer.take_interrupt());
}

TEST_CASE("writing TIMA during the delay cancels the reload and interrupt") {
    Timer timer = timer_at_overflow();
    timer.write(0xFF05, 0x10);
    timer.tick();
    CHECK(timer.read(0xFF05) == 0x10);
    CHECK_FALSE(timer.take_interrupt());
}

TEST_CASE("writing TIMA on the reload cycle is ignored") {
    Timer timer = timer_at_overflow();
    timer.tick();  // reload cycle
    timer.write(0xFF05, 0x10);
    CHECK(timer.read(0xFF05) == 0x42);
}

TEST_CASE("writing TMA on the reload cycle also reaches TIMA") {
    Timer timer = timer_at_overflow();
    timer.tick();
    timer.write(0xFF06, 0x77);
    CHECK(timer.read(0xFF05) == 0x77);
}
