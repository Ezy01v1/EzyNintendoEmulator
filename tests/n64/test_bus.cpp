#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/scheduler.hpp"

using namespace ezy::n64;

namespace {

// A 1 MB cartridge whose byte at offset i is (i * 3) & 0xFF (after the header).
Cartridge test_cart() {
    std::vector<std::uint8_t> rom(0x100000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    for (std::size_t i = 0x1000; i < rom.size(); ++i) rom[i] = static_cast<std::uint8_t>(i * 3);
    return *Cartridge::load(rom);
}

struct BusFixture {
    Scheduler scheduler;
    Cartridge cart = test_cart();
    Bus bus{scheduler, cart};

    // Runs every event due up to `cycles` from now.
    void run(std::uint64_t cycles) {
        const std::uint64_t end = scheduler.now() + cycles;
        while (true) {
            const auto next = scheduler.next_event_time();
            if (!next || *next > end) break;
            scheduler.advance_to(*next);
            while (const auto due = scheduler.pop_due()) bus.handle_event(due->type, due->when);
        }
        scheduler.advance_to(end);
    }
};

constexpr std::uint32_t kMiMode = 0x04300000, kMiVersion = 0x04300004, kMiIntr = 0x04300008, kMiMask = 0x0430000C;
constexpr std::uint32_t kPiDram = 0x04600000, kPiCart = 0x04600004, kPiWrLen = 0x0460000C, kPiStatus = 0x04600010;

}  // namespace

TEST_CASE("RDRAM reads and writes are big-endian") {
    BusFixture f;
    f.bus.write32(0x100, 0x11223344);
    CHECK(f.bus.read8(0x100) == 0x11);
    CHECK(f.bus.read8(0x103) == 0x44);
    CHECK(f.bus.read16(0x102) == 0x3344);
    f.bus.write8(0x101, 0xAA);
    CHECK(f.bus.read32(0x100) == 0x11AA3344);
}

TEST_CASE("64-bit access splits into two words") {
    BusFixture f;
    f.bus.write64(0x200, 0x0102030405060708ULL);
    CHECK(f.bus.read32(0x200) == 0x01020304);
    CHECK(f.bus.read32(0x204) == 0x05060708);
    CHECK(f.bus.read64(0x200) == 0x0102030405060708ULL);
}

TEST_CASE("MI_VERSION reads 0x02020102") {
    BusFixture f;
    CHECK(f.bus.read32(kMiVersion) == 0x02020102);
}

TEST_CASE("MI line follows interrupt and mask") {
    BusFixture f;
    f.bus.mi().raise(MiInterrupt::Pi);
    CHECK(f.bus.read32(kMiIntr) == 0x10);
    CHECK_FALSE(f.bus.interrupt_line());
    f.bus.write32(kMiMask, 1U << 9);  // set PI mask
    CHECK(f.bus.read32(kMiMask) == 0x10);
    CHECK(f.bus.interrupt_line());
    f.bus.write32(kMiMask, 1U << 8);  // clear PI mask
    CHECK_FALSE(f.bus.interrupt_line());
}

TEST_CASE("MI_MODE bit 11 clears the DP interrupt") {
    BusFixture f;
    f.bus.mi().raise(MiInterrupt::Dp);
    f.bus.write32(kMiMode, 1U << 11);
    CHECK(f.bus.read32(kMiIntr) == 0);
}

TEST_CASE("unmapped read returns 0") {
    BusFixture f;
    CHECK(f.bus.read32(0x00900000) == 0);  // beyond 8 MB of RDRAM
    CHECK(f.bus.read32(0x04900000) == 0);
}

TEST_CASE("SP DMEM is reachable and the RSP stays halted") {
    BusFixture f;
    f.bus.write32(0x04000010, 0xDEADBEEF);
    CHECK(f.bus.read32(0x04000010) == 0xDEADBEEF);
    f.bus.write32(0x04040010, 1);  // SP_STATUS: clear halt
    CHECK((f.bus.read32(0x04040010) & 1) == 1);
}

TEST_CASE("cartridge ROM reads through domain 1") {
    BusFixture f;
    CHECK(f.bus.read32(0x10000000) == 0x80371240);
    CHECK(f.bus.read8(0x10001001) == static_cast<std::uint8_t>(0x1001 * 3));
}

TEST_CASE("PI DMA copies cart to RDRAM and interrupts at the end") {
    BusFixture f;
    f.bus.write32(kPiDram, 0x1000);
    f.bus.write32(kPiCart, 0x10002000);
    f.bus.write32(kPiWrLen, 0x7F);  // 128 bytes
    for (std::uint32_t i = 0; i < 128; ++i) CHECK(f.bus.read8(0x1000 + i) == static_cast<std::uint8_t>((0x2000 + i) * 3));
    CHECK((f.bus.read32(kMiIntr) & 0x10) == 0);
    f.run(1'000'000);
    CHECK((f.bus.read32(kMiIntr) & 0x10) != 0);
    CHECK((f.bus.read32(kPiStatus) & 1) == 0);
    CHECK((f.bus.read32(kPiStatus) & 8) != 0);
    f.bus.write32(kPiStatus, 2);  // clear the interrupt
    CHECK((f.bus.read32(kMiIntr) & 0x10) == 0);
    CHECK(f.bus.read32(kPiDram) == 0x1080);
    CHECK(f.bus.read32(kPiCart) == 0x10002080);
}

TEST_CASE("PI stays busy until the scheduled end") {
    BusFixture f;
    f.bus.write32(0x04600014, 0x40);  // DOM1 LAT
    f.bus.write32(0x04600018, 0x12);  // DOM1 PWD
    f.bus.write32(0x0460001C, 0x07);  // DOM1 PGS
    f.bus.write32(0x04600020, 0x03);  // DOM1 RLS
    f.bus.write32(kPiDram, 0);
    f.bus.write32(kPiCart, 0x10001000);
    f.bus.write32(kPiWrLen, 0xFFF);  // 4 KB
    CHECK((f.bus.read32(kPiStatus) & 1) == 1);
    // 4 KB in 512-byte pages: 8 x (0x40 + 1) + 2048 halfwords x (0x12 + 1 + 0x03 + 1) RCP cycles
    const std::uint64_t rcp = 8 * 0x41 + 2048 * 0x17;
    f.run(rcp * 3 / 2 - 1);
    CHECK((f.bus.read32(kPiStatus) & 1) == 1);
    f.run(2);
    CHECK((f.bus.read32(kPiStatus) & 1) == 0);
}

TEST_CASE("PI DMA aligns the RDRAM address down to 8 bytes") {
    BusFixture f;
    f.bus.write32(kPiDram, 0x1005);
    f.bus.write32(kPiCart, 0x10002000);
    f.bus.write32(kPiWrLen, 0x7);
    CHECK(f.bus.read8(0x1000) == static_cast<std::uint8_t>(0x2000 * 3));
}

TEST_CASE("ISViewer write of the length emits the buffer") {
    BusFixture f;
    CHECK(f.bus.read32(0x13FF0000) == 0x49533634);  // "IS64"
    f.bus.write32(0x13FF0020, 0x48690A00);           // "Hi\n"
    f.bus.write32(0x13FF0014, 3);
    CHECK(f.bus.take_isviewer_output() == "Hi\n");
    CHECK(f.bus.take_isviewer_output().empty());
}

TEST_CASE("uncached RDRAM access has a penalty, RCP registers too") {
    BusFixture f;
    CHECK(f.bus.uncached_cycles(0x1000) > 0);
    CHECK(f.bus.uncached_cycles(0x04300004) > 0);
    CHECK(f.bus.uncached_cycles(0x10000000) > f.bus.uncached_cycles(0x1000));
}
