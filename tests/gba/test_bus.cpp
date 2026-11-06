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

TEST_CASE("KEYCNT raises the keypad interrupt in OR and AND mode") {
    constexpr std::uint16_t kKeypadIrq = 1U << 12;
    Bus bus(make_rom());
    bus.write16(0x04000202, 0xFFFF);  // clear IF

    // OR mode: any selected key (A or B) fires.
    bus.write16(0x04000132, 0x4003);
    CHECK((bus.interrupt_flags() & kKeypadIrq) == 0);
    bus.set_keys(0x0008);  // Start: not selected
    CHECK((bus.interrupt_flags() & kKeypadIrq) == 0);
    bus.set_keys(0x0002);  // B
    CHECK((bus.interrupt_flags() & kKeypadIrq) != 0);

    // AND mode: all selected keys must be held.
    bus.write16(0x04000202, kKeypadIrq);
    bus.set_keys(0);
    bus.write16(0x04000132, 0xC003);
    bus.set_keys(0x0001);
    CHECK((bus.interrupt_flags() & kKeypadIrq) == 0);
    bus.set_keys(0x0003);
    CHECK((bus.interrupt_flags() & kKeypadIrq) != 0);

    // Writing KEYCNT while the condition already holds fires too; disabled never fires.
    bus.write16(0x04000202, kKeypadIrq);
    bus.write16(0x04000132, 0x0003);
    bus.set_keys(0x0003);
    CHECK((bus.interrupt_flags() & kKeypadIrq) == 0);
    bus.write16(0x04000132, 0x4001);
    CHECK((bus.interrupt_flags() & kKeypadIrq) != 0);
}

TEST_CASE("unmapped reads return the last opcode the CPU fetched (open bus)") {
    Bus bus(make_rom());
    CHECK(bus.read32(0x10000000) == 0);  // nothing fetched yet

    // ARM fetch: the whole word.
    bus.write32(0x03000000, 0xE3A01234);
    (void)bus.read32(0x03000000, static_cast<Access>(kCode | kNonseq));
    CHECK(bus.read32(0x10000000) == 0xE3A01234);
    CHECK(bus.read16(0x10000002) == 0xE3A0);
    CHECK(bus.read8(0x10000001) == 0x12);
    CHECK(bus.read32(0x00004000) == 0xE3A01234);  // past the BIOS
    CHECK(bus.read32(0x04000400) == 0xE3A01234);  // past I/O

    // Data loads don't change it.
    (void)bus.read32(0x08000000, kNonseq);
    CHECK(bus.read32(0x10000000) == 0xE3A01234);

    // Thumb fetch: the halfword in both halves.
    (void)bus.read16(0x08000006, static_cast<Access>(kCode | kSeq));
    CHECK(bus.read32(0x10000000) == 0x00030003);
}

TEST_CASE("write-only and unused I/O registers read as open bus or 0, like mGBA") {
    // Through the CPU path: the untimed reads show the stored values.
    Bus bus(make_rom());
    bus.write32(0x03000000, 0xE1A00000);  // NOP, fetched so the bus holds it
    (void)bus.read32(0x03000000, static_cast<Access>(kCode | kNonseq));

    bus.write16(0x04000010, 0x0123);              // BG0HOFS: write-only
    CHECK(bus.read16(0x04000010, kNonseq) == 0x0000);      // low half of the latched opcode
    CHECK(bus.read16(0x04000012, kNonseq) == 0xE1A0);      // BG0VOFS, high half
    CHECK(bus.read16(0x040000B0, kNonseq) == 0x0000);      // DMA0SAD
    CHECK(bus.read16(0x040000B8, kNonseq) == 0);           // DMA0CNT_L: 0, not open bus
    CHECK(bus.read16(0x04000066, kNonseq) == 0);           // listed unused: 0
    CHECK(bus.read16(0x0400020A, kNonseq) == 0);           // after IME: 0
    CHECK(bus.read16(0x040000E2, kNonseq) == 0xE1A0);      // other unused: open bus
    CHECK(bus.read32(0x04000208, kNonseq) == 0);           // IME = 0 and 0x20A = 0

    // Readable registers are unaffected, including the readable half of a mixed word.
    bus.write16(0x04000008, 0x1F83);  // BG0CNT
    CHECK(bus.read16(0x04000008, kNonseq) == 0x1F83);
    CHECK(bus.read32(0x0400004C, kNonseq) == 0xE1A00000);  // MOSAIC + unused 0x04E
    bus.write16(0x04000050, 0x3F41);  // BLDCNT
    CHECK(bus.read32(0x04000050, kNonseq) == 0x00003F41);  // BLDALPHA (readable) is 0
    CHECK(bus.read8(0x04000011, kNonseq) == 0x00);
}

TEST_CASE("open bus during DMA holds the last value the DMA moved") {
    Bus bus(make_rom());
    bus.write32(0x02000000, 0xCAFEF00D);
    bus.write32(0x040000D4, 0x02000000);  // DMA3 src
    bus.write32(0x040000D8, 0x03000100);  // dst
    bus.write16(0x040000DC, 1);
    bus.write16(0x040000DE, 0x8400);      // enable, 32-bit, immediate
    CHECK(bus.read32(0x03000100) == 0xCAFEF00D);
    CHECK(bus.read32(0x10000000) == 0xCAFEF00D);
}
