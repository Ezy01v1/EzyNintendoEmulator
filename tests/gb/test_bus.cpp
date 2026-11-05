#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "gb/bus.hpp"
#include "test_helpers.hpp"

using namespace ezy::gb;
using ezy::gb::test::make_cartridge;
using ezy::gb::test::make_rom;

TEST_CASE("ROM area reads from the cartridge and ignores writes") {
    auto rom = make_rom();
    rom[0x0150] = 0xAB;
    Bus bus(make_cartridge(rom));
    CHECK(bus.read(0x0150) == 0xAB);
    bus.write(0x0150, 0x00);
    CHECK(bus.read(0x0150) == 0xAB);
}

TEST_CASE("RAM regions store values") {
    Bus bus(make_cartridge());
    constexpr std::array<std::uint16_t, 8> kAddrs = {0x8000, 0x9FFF, 0xC000, 0xDFFF, 0xFE00, 0xFE9F, 0xFF80, 0xFFFE};
    for (const std::uint16_t addr : kAddrs) {
        bus.write(addr, 0x5A);
        CHECK(bus.read(addr) == 0x5A);
    }
}

TEST_CASE("echo RAM mirrors C000-DDFF") {
    Bus bus(make_cartridge());
    bus.write(0xC123, 0x42);
    CHECK(bus.read(0xE123) == 0x42);
    bus.write(0xFDFF, 0x99);
    CHECK(bus.read(0xDDFF) == 0x99);
}

TEST_CASE("unusable area and missing external RAM read 0xFF") {
    Bus bus(make_cartridge());
    bus.write(0xFEA0, 0x00);
    CHECK(bus.read(0xFEA0) == 0xFF);
    CHECK(bus.read(0xA000) == 0xFF);
}

TEST_CASE("IE and IF registers") {
    Bus bus(make_cartridge());
    bus.write(0xFFFF, 0x1F);
    CHECK(bus.interrupt_enable() == 0x1F);

    bus.write(0xFF0F, 0xFF);
    CHECK(bus.interrupt_flags() == 0x1F);  // only 5 bits are stored
    CHECK(bus.read(0xFF0F) == 0xFF);       // upper 3 bits read as 1

    bus.write(0xFF0F, 0x00);
    bus.request_interrupt(interrupt::kTimer);
    CHECK(bus.read(0xFF0F) == (0xE0 | interrupt::kTimer));
}

TEST_CASE("serial transfer logs the byte and raises the serial interrupt") {
    Bus bus(make_cartridge());
    for (const char c : std::string_view("OK")) {
        bus.write(0xFF01, static_cast<std::uint8_t>(c));
        bus.write(0xFF02, 0x81);  // start, internal clock
    }
    CHECK(bus.serial_output() == "OK");
    CHECK((bus.read(0xFF02) & 0x80) == 0);  // transfer complete
    CHECK(bus.read(0xFF01) == 0xFF);        // nothing connected
    CHECK((bus.interrupt_flags() & interrupt::kSerial) != 0);
}

TEST_CASE("read/write/tick cost one M-cycle; peek/poke are free") {
    Bus bus(make_cartridge());
    bus.read(0xC000);
    bus.write(0xC000, 1);
    bus.tick();
    CHECK(bus.t_cycles() == 12);

    bus.poke(0xC000, 2);
    CHECK(bus.peek(0xC000) == 2);
    CHECK(bus.t_cycles() == 12);
}

TEST_CASE("bus routes 0xA000-0xBFFF to cartridge RAM") {
    Bus bus(ezy::gb::test::make_cartridge(ezy::gb::test::make_rom("BUSRAM", 0x03, 0x00, 0x02)));
    bus.poke(0xA000, 0x99);
    CHECK(bus.peek(0xA000) == 0xFF);  // disabled
    bus.poke(0x0000, 0x0A);
    bus.poke(0xA000, 0x99);
    CHECK(bus.peek(0xA000) == 0x99);
}
