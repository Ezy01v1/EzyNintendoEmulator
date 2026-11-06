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
    bus.poke(0xFF40, 0x00);  // LCD off, so VRAM and OAM are not locked
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

TEST_CASE("CPU reads of VRAM return 0xFF during mode 3, peek still sees it") {
    Bus bus(make_cartridge());
    bus.poke(0x8000, 0x42);
    while (bus.ppu().mode() != 3) bus.tick();
    CHECK(bus.read(0x8000) == 0xFF);
    bus.write(0x8000, 0x99);  // dropped
    CHECK(bus.peek(0x8000) == 0x42);
}

TEST_CASE("CPU reads of OAM return 0xFF during mode 2") {
    Bus bus(make_cartridge());
    bus.poke(0xFE00, 0x42);
    while (bus.ppu().mode() != 0) bus.tick();
    while (bus.ppu().mode() != 2) bus.tick();
    CHECK(bus.read(0xFE00) == 0xFF);
    CHECK(bus.peek(0xFE00) == 0x42);
}

namespace {

// LCD off (so OAM is never locked by the PPU) and 160 bytes at 0xC000 = 0x10, 0x11, ...
Bus make_dma_bus() {
    Bus bus(make_cartridge());
    bus.poke(0xFF40, 0x00);
    for (std::uint16_t i = 0; i < 0xA0; ++i) bus.poke(static_cast<std::uint16_t>(0xC000 + i), static_cast<std::uint8_t>(0x10 + i));
    return bus;
}

}  // namespace

// Mooneye oam_dma_start: write at M0, OAM still free at M1, the DMA owns it from M2.
TEST_CASE("OAM DMA starts 2 M-cycles after the write and copies one byte per M-cycle") {
    Bus bus = make_dma_bus();
    bus.write(0xFF46, 0xC0);
    bus.tick();
    CHECK_FALSE(bus.oam_dma_active());
    bus.tick();
    CHECK(bus.oam_dma_active());
    for (int i = 0; i < 80; ++i) bus.tick();
    CHECK(bus.peek(0xFE00 + 79) == 0x10 + 79);
    CHECK(bus.peek(0xFE00 + 80) == 0x00);
    for (int i = 0; i < 80; ++i) bus.tick();
    CHECK(bus.peek(0xFE9F) == 0x10 + 159);
    CHECK_FALSE(bus.oam_dma_active());
}

TEST_CASE("CPU reads outside HRAM see the DMA byte during DMA, HRAM stays readable") {
    Bus bus = make_dma_bus();
    bus.poke(0xFF80, 0x77);
    bus.poke(0xD000, 0x55);
    bus.write(0xFF46, 0xC0);
    bus.tick();
    bus.tick();  // started; nothing copied yet
    const std::uint8_t seen = bus.read(0xD000);  // same bus as the source; this cycle copies byte 0
    CHECK(seen == 0x10);
    CHECK(bus.read(0xFF80) == 0x77);
    bus.write(0xD000, 0x99);  // dropped
    CHECK(bus.peek(0xD000) == 0x55);
    CHECK(bus.read(0xFE00) == 0xFF);  // OAM belongs to the DMA
}

TEST_CASE("restarting OAM DMA mid-transfer") {
    Bus bus = make_dma_bus();
    for (std::uint16_t i = 0; i < 0xA0; ++i) bus.poke(static_cast<std::uint16_t>(0xC100 + i), 0xAA);
    bus.write(0xFF46, 0xC0);
    for (int i = 0; i < 12; ++i) bus.tick();  // start-up + 10 bytes
    bus.write(0xFF46, 0xC1);                // the old transfer runs during the new start-up
    for (int i = 0; i < 162; ++i) bus.tick();
    CHECK(bus.peek(0xFE00) == 0xAA);
    CHECK(bus.peek(0xFE9F) == 0xAA);
}

TEST_CASE("OAM DMA from 0xFE00 reads the WRAM echo at 0xDE00") {
    Bus bus = make_dma_bus();
    bus.poke(0xDE05, 0x5E);
    bus.write(0xFF46, 0xFE);
    for (int i = 0; i < 162; ++i) bus.tick();
    CHECK(bus.peek(0xFE05) == 0x5E);
}

TEST_CASE("OAM DMA from VRAM leaves the external bus to the CPU") {
    Bus bus = make_dma_bus();
    bus.poke(0x8000, 0xD7);
    bus.poke(0x8001, 0xD8);
    bus.poke(0xC800, 0x42);
    bus.write(0xFF46, 0x80);
    bus.tick();
    bus.tick();
    CHECK(bus.read(0xC800) == 0x42);  // WRAM is on the other bus
    CHECK(bus.read(0x8100) == 0xD8);  // VRAM is the DMA's bus: the CPU sees the byte being moved
}
