#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "gba/bus.hpp"

using namespace ezy::gba;

namespace {

// 4 KiB ROM where each halfword holds its own index: 0x0000, 0x0001, ...
std::vector<std::uint8_t> make_rom() {
    std::vector<std::uint8_t> rom(4096);
    for (std::size_t i = 0; i < rom.size(); i += 2) {
        rom[i] = static_cast<std::uint8_t>((i / 2) & 0xFF);
        rom[i + 1] = static_cast<std::uint8_t>((i / 2) >> 8);
    }
    return rom;
}

}  // namespace

TEST_CASE("GBA bus reads and writes every RAM region, little-endian") {
    Bus bus(make_rom());
    for (const std::uint32_t base : {0x02000000U, 0x03000000U, 0x04000000U, 0x05000000U, 0x06000000U, 0x07000000U}) {
        CAPTURE(base);
        bus.write32(base + 0x10, 0x11223344);
        CHECK(bus.read32(base + 0x10) == 0x11223344);
        CHECK(bus.read16(base + 0x10) == 0x3344);
        CHECK(bus.read16(base + 0x12) == 0x1122);
        CHECK(bus.read8(base + 0x13) == 0x11);
    }
}

TEST_CASE("GBA bus mirrors RAM regions") {
    Bus bus(make_rom());
    bus.write32(0x02000000, 0xCAFEBABE);
    CHECK(bus.read32(0x02040000) == 0xCAFEBABE);  // EWRAM every 256 KiB
    CHECK(bus.read32(0x02FC0000) == 0xCAFEBABE);
    bus.write32(0x03007FFC, 0x12345678);
    CHECK(bus.read32(0x03FFFFFC) == 0x12345678);  // IWRAM every 32 KiB (the BIOS uses this mirror)
    bus.write16(0x05000000, 0x7FFF);
    CHECK(bus.read16(0x05000400) == 0x7FFF);  // palette every 1 KiB
    bus.write16(0x07000000, 0x1234);
    CHECK(bus.read16(0x07000400) == 0x1234);  // OAM every 1 KiB
}

TEST_CASE("GBA VRAM is 96 KiB: the last 32 KiB of the 128 KiB window mirror the OBJ area") {
    Bus bus(make_rom());
    bus.write16(0x06010000, 0xBEEF);
    CHECK(bus.read16(0x06018000) == 0xBEEF);
    bus.write16(0x06000000, 0xF00D);
    CHECK(bus.read16(0x06020000) == 0xF00D);  // the whole window repeats every 128 KiB
}

TEST_CASE("GBA ROM is visible in all three waitstate mirrors and is read-only") {
    Bus bus(make_rom());
    for (const std::uint32_t base : {0x08000000U, 0x0A000000U, 0x0C000000U}) {
        CAPTURE(base);
        CHECK(bus.read16(base + 0x20) == 0x0010);
        CHECK(bus.read32(base + 0x20) == 0x00110010);
    }
    bus.write16(0x08000020, 0xFFFF);
    CHECK(bus.read16(0x08000020) == 0x0010);
}

TEST_CASE("GBA ROM reads past the end return the halfword address") {
    Bus bus(make_rom());
    CHECK(bus.read16(0x08001000) == 0x0800);  // 0x1000 / 2
    CHECK(bus.read32(0x08001000) == 0x08010800);
    CHECK(bus.read8(0x08001001) == 0x08);
}

TEST_CASE("GBA bus aligns 16- and 32-bit accesses") {
    Bus bus(make_rom());
    bus.write32(0x03000000, 0xAABBCCDD);
    CHECK(bus.read32(0x03000003) == 0xAABBCCDD);
    CHECK(bus.read16(0x03000001) == 0xCCDD);
    bus.write16(0x03000007, 0x1234);  // lands on 0x03000006
    CHECK(bus.read16(0x03000006) == 0x1234);
}

TEST_CASE("misaligned LDR rotates the aligned word") {
    CHECK(rotate_misaligned_word(0xAABBCCDD, 0x1000) == 0xAABBCCDD);
    CHECK(rotate_misaligned_word(0xAABBCCDD, 0x1001) == 0xDDAABBCC);
    CHECK(rotate_misaligned_word(0xAABBCCDD, 0x1002) == 0xCCDDAABB);
    CHECK(rotate_misaligned_word(0xAABBCCDD, 0x1003) == 0xBBCCDDAA);
}

TEST_CASE("GBA byte writes: palette/VRAM duplicate, OAM and VRAM OBJ ignore") {
    Bus bus(make_rom());
    bus.write8(0x05000001, 0x42);
    CHECK(bus.read16(0x05000000) == 0x4242);
    bus.write8(0x06000003, 0x17);
    CHECK(bus.read16(0x06000002) == 0x1717);
    bus.write8(0x06010000, 0x99);  // OBJ tiles (text modes)
    CHECK(bus.read16(0x06010000) == 0x0000);
    bus.write8(0x07000000, 0x55);
    CHECK(bus.read16(0x07000000) == 0x0000);
}

TEST_CASE("GBA SRAM is an 8-bit bus, erased to 0xFF") {
    Bus bus(make_rom());
    CHECK(bus.read8(0x0E000000) == 0xFF);
    bus.write8(0x0E000010, 0x5A);
    CHECK(bus.read8(0x0E000010) == 0x5A);
    CHECK(bus.read16(0x0E000010) == 0x5A5A);
    CHECK(bus.read32(0x0E000010) == 0x5A5A5A5A);
    CHECK(bus.read8(0x0E010010) == 0x5A);  // 64 KiB mirror
    CHECK(bus.read8(0x0F000010) == 0x5A);
}

TEST_CASE("GBA BIOS: loaded bytes are readable, writes ignored, missing BIOS reads 0") {
    const std::array<std::uint8_t, 4> bios = {0x18, 0x00, 0x00, 0xEA};  // B #0x68
    Bus with(make_rom(), bios);
    CHECK(with.read32(0x00000000) == 0xEA000018);
    with.write32(0x00000000, 0);
    CHECK(with.read32(0x00000000) == 0xEA000018);
    const Bus without(make_rom());
    CHECK(without.read32(0x00000000) == 0);
}

TEST_CASE("GBA unmapped regions read 0 and ignore writes") {
    Bus bus(make_rom());
    bus.write32(0x01000000, 0xFFFFFFFF);
    CHECK(bus.read32(0x01000000) == 0);
    CHECK(bus.read32(0x04000400) == 0);  // past the I/O registers
    CHECK(bus.read32(0x10000000) == 0);
}
