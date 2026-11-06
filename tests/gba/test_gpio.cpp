#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gba/bus.hpp"
#include "gba/gpio.hpp"

using namespace ezy::gba;

namespace {

std::tm fixed_time() {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;  // November
    t.tm_mday = 5;
    t.tm_wday = 4;
    t.tm_hour = 17;
    t.tm_min = 42;
    t.tm_sec = 9;
    return t;
}

// Drives the pins the way games' RTC drivers do.
struct Driver {
    Gpio& gpio;
    void pins(std::uint8_t v) { gpio.write(0xC4, v); }
    void begin() {
        gpio.write(0xC8, 1);
        gpio.write(0xC6, 0x07);
        pins(0x01);  // SCK high, CS low
        pins(0x05);  // CS high
    }
    void send(std::uint8_t byte) {
        for (int i = 0; i < 8; ++i) {
            const auto sio = static_cast<std::uint8_t>(((byte >> i) & 1) << 1);
            pins(static_cast<std::uint8_t>(0x04 | sio));
            pins(static_cast<std::uint8_t>(0x05 | sio));
        }
    }
    std::uint8_t receive() {
        gpio.write(0xC6, 0x05);  // SIO becomes an input
        std::uint8_t byte = 0;
        for (int i = 0; i < 8; ++i) {
            pins(0x04);
            pins(0x05);
            byte = static_cast<std::uint8_t>(byte | (((gpio.read(0xC4) >> 1) & 1) << i));
        }
        gpio.write(0xC6, 0x07);
        return byte;
    }
    void end() { pins(0x01); }
};

}  // namespace

TEST_CASE("RTC reads the date and time in BCD") {
    Gpio gpio(fixed_time);
    Driver d{gpio};
    d.begin();
    d.send(0xA6);  // read register 2: date + time
    for (const int b : {0x26, 0x11, 0x05, 0x04, 0x17, 0x42, 0x09}) CHECK(d.receive() == b);
    d.end();

    d.begin();
    d.send(0xE6);  // register 6: time only
    CHECK(d.receive() == 0x17);
    CHECK(d.receive() == 0x42);
    CHECK(d.receive() == 0x09);
    d.end();
}

TEST_CASE("RTC control register and 12-hour mode") {
    Gpio gpio(fixed_time);
    Driver d{gpio};
    d.begin();
    d.send(0x46);  // write register 4
    d.send(0x00);  // 12-hour mode
    d.end();
    d.begin();
    d.send(0xC6);
    CHECK(d.receive() == 0x00);
    d.end();
    d.begin();
    d.send(0xE6);
    CHECK(d.receive() == 0x85);  // 5 PM
    d.end();
}

TEST_CASE("GPIO registers read ROM unless control bit 0 is set") {
    std::vector<std::uint8_t> rom(0x200, 0xAB);
    constexpr std::string_view kId = "SIIRTC_V";
    std::copy(kId.begin(), kId.end(), rom.begin() + 0x100);
    CHECK(has_rtc(rom));
    Bus bus(rom);
    CHECK(bus.read16(0x080000C6) == 0xABAB);
    bus.write16(0x080000C8, 1);
    bus.write16(0x080000C6, 0x07);
    CHECK(bus.read16(0x080000C6) == 0x0007);
    bus.write16(0x080000C8, 0);
    CHECK(bus.read16(0x080000C6) == 0xABAB);

    CHECK_FALSE(has_rtc(std::vector<std::uint8_t>(0x200, 0)));
}
