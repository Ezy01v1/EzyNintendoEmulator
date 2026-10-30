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
    for (int i = 0; i < 4; ++i) timer.tick();
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
