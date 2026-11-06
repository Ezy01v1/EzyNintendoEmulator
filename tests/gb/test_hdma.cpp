#include <doctest/doctest.h>

#include <cstdint>

#include "gb/bus.hpp"
#include "test_helpers.hpp"

using namespace ezy::gb;

namespace {

Bus make_cgb_bus() {
    auto rom = test::make_rom("HDMA");
    rom[kCgbFlagAddr] = 0x80;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return Bus(test::make_cartridge(rom), Model::Cgb);
}

// Source 0xC000 (filled with 0, 1, 2, ...), destination 0x8000.
void setup_transfer(Bus& bus) {
    for (std::uint16_t i = 0; i < 0x100; ++i) bus.poke(static_cast<std::uint16_t>(0xC000 + i), static_cast<std::uint8_t>(i));
    bus.poke(0xFF51, 0xC0);
    bus.poke(0xFF52, 0x0F);  // low 4 bits ignored
    bus.poke(0xFF53, 0x80);  // only bits 0-4 used: 0x8000 + 0x0000
    bus.poke(0xFF54, 0x00);
}

}  // namespace

TEST_CASE("GDMA copies length bytes and stalls the CPU") {
    Bus bus = make_cgb_bus();
    bus.poke(0xFF40, 0x00);
    setup_transfer(bus);
    const auto before = bus.t_cycles();
    bus.write(0xFF55, 0x01);  // 2 blocks = 32 bytes, general purpose
    CHECK(bus.t_cycles() - before == 4 + 16 * 4);  // the write itself + 16 M-cycles of copying
    CHECK(bus.peek(0x8000) == 0x00);
    CHECK(bus.peek(0x801F) == 0x1F);
    CHECK(bus.peek(0x8020) == 0x00);
    CHECK(bus.peek(0xFF55) == 0xFF);
}

TEST_CASE("HDMA copies 16 bytes per HBlank and reports remaining blocks") {
    Bus bus = make_cgb_bus();
    setup_transfer(bus);
    while (bus.ppu().mode() != 2) bus.tick();
    bus.poke(0xFF55, 0x82);  // 3 blocks, HBlank mode
    CHECK(bus.peek(0xFF55) == 0x02);
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x800F) == 0x0F);
    CHECK(bus.peek(0x8010) == 0x00);
    CHECK(bus.peek(0xFF55) == 0x01);
    while (bus.ppu().mode() != 2) bus.tick();
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x801F) == 0x1F);
    while (bus.ppu().mode() != 2) bus.tick();
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x802F) == 0x2F);
    CHECK(bus.peek(0xFF55) == 0xFF);
}

TEST_CASE("writing bit 7 = 0 cancels HDMA") {
    Bus bus = make_cgb_bus();
    setup_transfer(bus);
    while (bus.ppu().mode() != 2) bus.tick();
    bus.poke(0xFF55, 0x83);
    while (bus.ppu().mode() != 0) bus.tick();
    bus.poke(0xFF55, 0x00);
    CHECK(bus.peek(0xFF55) == 0x82);  // stopped, 3 blocks were left
    while (bus.ppu().mode() != 2) bus.tick();
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x8010) == 0x00);  // nothing more copied
}

TEST_CASE("HDMA with the LCD off waits for the LCD") {
    Bus bus = make_cgb_bus();
    bus.poke(0xFF40, 0x00);
    setup_transfer(bus);
    bus.poke(0xFF55, 0x81);
    for (int i = 0; i < 1000; ++i) bus.tick();  // must not hang, and copies one block
    CHECK(bus.peek(0x800F) == 0x0F);
    CHECK(bus.peek(0x8010) == 0x00);
    CHECK(bus.peek(0xFF55) == 0x00);
    bus.poke(0xFF40, 0x91);
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x801F) == 0x1F);
    CHECK(bus.peek(0xFF55) == 0xFF);
}

TEST_CASE("VRAM DMA registers do not exist outside CGB mode") {
    Bus bus(test::make_cartridge(), Model::Cgb);
    bus.poke(0xFF55, 0x00);
    CHECK(bus.peek(0xFF55) == 0xFF);
}

TEST_CASE("HDMA started during HBlank copies its first block at once") {
    Bus bus = make_cgb_bus();
    setup_transfer(bus);
    while (bus.ppu().mode() != 0) bus.tick();
    bus.poke(0xFF55, 0x81);
    CHECK(bus.peek(0x800F) == 0x0F);
    CHECK(bus.peek(0xFF55) == 0x00);
}

TEST_CASE("HBlank HDMA pauses while the CPU is halted") {
    Bus bus = make_cgb_bus();
    setup_transfer(bus);
    while (bus.ppu().mode() != 2) bus.tick();
    bus.poke(0xFF55, 0x80);
    bus.set_cpu_halted(true);
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x800F) == 0x00);  // nothing while halted
    CHECK(bus.peek(0xFF55) == 0x00);  // the block is still pending
    bus.set_cpu_halted(false);
    while (bus.ppu().mode() != 2) bus.tick();
    while (bus.ppu().mode() != 0) bus.tick();
    CHECK(bus.peek(0x800F) == 0x0F);
}
