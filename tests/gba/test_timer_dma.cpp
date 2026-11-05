#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "gba/gba.hpp"

using namespace ezy::gba;

namespace {

// A console sitting in `B .` at the ROM entry, so tests can drive I/O directly and let
// time pass with run_until(). The CPU's own accesses don't touch I/O.
std::unique_ptr<Gba> idle_gba() {
    std::vector<std::uint8_t> rom(0x200, 0);
    rom[0] = 0xFE;
    rom[1] = 0xFF;
    rom[2] = 0xFF;
    rom[3] = 0xEA;  // B .
    return Gba::create(rom);
}

constexpr std::uint32_t kTm0Cnt = 0x04000100;
constexpr std::uint32_t kTm1Cnt = 0x04000104;
constexpr std::uint16_t kEnable = 0x80;
constexpr std::uint16_t kIrq = 0x40;
constexpr std::uint16_t kCascade = 0x04;

}  // namespace

// --- Timers ---

TEST_CASE("timer counts from the reload value and reads live") {
    auto gba = idle_gba();
    const std::uint64_t t0 = gba->scheduler().now();
    gba->bus().write16(kTm0Cnt, 0xFF00);                // reload
    gba->bus().write16(kTm0Cnt + 2, kEnable);           // prescaler 1
    CHECK(gba->bus().read16(kTm0Cnt) == 0xFF00);
    gba->run_until(t0 + 100);
    const auto elapsed = gba->scheduler().now() - t0;
    CHECK(gba->bus().read16(kTm0Cnt) == 0xFF00 + elapsed);
}

TEST_CASE("timer overflow raises its IRQ at the exact cycle for every prescaler") {
    for (const int prescaler : {0, 1, 2, 3}) {
        CAPTURE(prescaler);
        auto gba = idle_gba();
        const int shift = std::array{0, 6, 8, 10}[static_cast<std::size_t>(prescaler)];
        gba->bus().write16(kTm0Cnt, 0xFFF0);  // 16 ticks to overflow
        const std::uint64_t start = gba->scheduler().now();
        gba->bus().write16(kTm0Cnt + 2, static_cast<std::uint16_t>(kEnable | kIrq | prescaler));
        const std::uint64_t due = start + (16ULL << shift);
        CHECK(gba->scheduler().pending(EventType::Timer0));
        gba->run_until(due - 1);
        // run_until stops at an instruction boundary; make sure we're still before it.
        if (gba->scheduler().now() < due) CHECK((gba->bus().interrupt_flags() & 0x08) == 0);
        gba->run_until(due + 16);
        CHECK((gba->bus().interrupt_flags() & 0x08) != 0);  // timer 0 = IF bit 3
        CHECK(gba->bus().read16(kTm0Cnt) >= 0xFFF0);        // reloaded
    }
}

TEST_CASE("count-up timer advances once per overflow of the previous one") {
    auto gba = idle_gba();
    gba->bus().write16(kTm1Cnt, 0xFFFE);                    // overflows after 2 cascades
    gba->bus().write16(kTm1Cnt + 2, kEnable | kCascade | kIrq);
    gba->bus().write16(kTm0Cnt, 0xFF00);                    // overflows every 256 cycles
    const std::uint64_t start = gba->scheduler().now();
    gba->bus().write16(kTm0Cnt + 2, kEnable);
    CHECK_FALSE(gba->scheduler().pending(EventType::Timer1));  // no event of its own
    gba->run_until(start + 256 + 8);
    CHECK(gba->bus().read16(kTm1Cnt) == 0xFFFF);
    CHECK((gba->bus().interrupt_flags() & 0x10) == 0);
    gba->run_until(start + 512 + 8);
    CHECK(gba->bus().read16(kTm1Cnt) == 0xFFFE);            // overflowed and reloaded
    CHECK((gba->bus().interrupt_flags() & 0x10) != 0);      // timer 1 = IF bit 4
}

TEST_CASE("disabling a timer freezes it; re-enabling reloads") {
    auto gba = idle_gba();
    gba->bus().write16(kTm0Cnt, 0x1000);
    gba->bus().write16(kTm0Cnt + 2, kEnable | 1);  // /64
    const std::uint64_t start = gba->scheduler().now();
    gba->run_until(start + 64 * 10 + 4);
    gba->bus().write16(kTm0Cnt + 2, 1);            // stop
    const auto frozen = gba->bus().read16(kTm0Cnt);
    CHECK(frozen >= 0x100A);
    CHECK_FALSE(gba->scheduler().pending(EventType::Timer0));
    gba->run_until(gba->scheduler().now() + 5000);
    CHECK(gba->bus().read16(kTm0Cnt) == frozen);
    gba->bus().write16(kTm0Cnt + 2, kEnable | 1);
    CHECK(gba->bus().read16(kTm0Cnt) == 0x1000);
}

// --- DMA ---

namespace {

constexpr std::uint32_t kDma3 = 0x040000D4;  // SAD, DAD, CNT_L, CNT_H
constexpr std::uint32_t kDma0 = 0x040000B0;

void dma(Gba& gba, std::uint32_t base, std::uint32_t src, std::uint32_t dst, std::uint16_t count, std::uint16_t control) {
    gba.bus().write32(base, src);
    gba.bus().write32(base + 4, dst);
    gba.bus().write16(base + 8, count);
    gba.bus().write16(base + 10, control);
}

}  // namespace

TEST_CASE("immediate DMA copies words, clears enable and costs cycles") {
    auto gba = idle_gba();
    for (std::uint32_t i = 0; i < 4; ++i) gba->bus().write32(0x03000000 + i * 4, 0x11110000 + i);
    const std::uint64_t before = gba->scheduler().now();
    dma(*gba, kDma3, 0x03000000, 0x02000000, 4, 0x8400 | 0x4000);  // enable, 32-bit, IRQ
    CHECK(gba->scheduler().now() - before == 2 + 4 * (1 + 6));     // start-up + IWRAM read + EWRAM write
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(gba->bus().read32(0x02000000 + i * 4) == 0x11110000 + i);
    CHECK((gba->bus().read16(kDma3 + 10) & 0x8000) == 0);
    CHECK((gba->bus().interrupt_flags() & (1U << 11)) != 0);  // DMA3 = IF bit 11
}

TEST_CASE("DMA with a fixed source fills, decrementing destination goes down") {
    auto gba = idle_gba();
    gba->bus().write16(0x03000000, 0xABCD);
    dma(*gba, kDma3, 0x03000000, 0x02000010, 3, 0x8000 | (2U << 7) | (1U << 5));  // src fixed, dst decrement
    CHECK(gba->bus().read16(0x02000010) == 0xABCD);
    CHECK(gba->bus().read16(0x0200000E) == 0xABCD);
    CHECK(gba->bus().read16(0x0200000C) == 0xABCD);
    CHECK(gba->bus().read16(0x0200000A) == 0);
}

TEST_CASE("HBlank DMA with repeat and dest reload runs once per visible line") {
    auto gba = idle_gba();
    gba->bus().write16(0x03000000, 0x0001);
    gba->bus().write16(0x03000002, 0x0002);
    // Each HBlank copies 1 halfword from an incrementing source to a reloaded destination.
    dma(*gba, kDma0, 0x03000000, 0x05000000, 1, 0x8000 | (2U << 12) | 0x0200 | (3U << 5));
    CHECK((gba->bus().read16(kDma0 + 10) & 0x8000) != 0);  // armed, not run yet
    gba->run_until(1232 + 16);                              // first HBlank (line 0)
    CHECK(gba->bus().read16(0x05000000) == 0x0001);
    gba->run_until(2 * 1232 + 16);                          // line 1
    CHECK(gba->bus().read16(0x05000000) == 0x0002);         // same destination: reloaded
    CHECK((gba->bus().read16(kDma0 + 10) & 0x8000) != 0);   // still armed (repeat)
}

TEST_CASE("VBlank DMA waits for line 160") {
    auto gba = idle_gba();
    gba->bus().write32(0x03000000, 0xCAFEBABE);
    dma(*gba, kDma3, 0x03000000, 0x02000000, 1, 0x8400 | (1U << 12));
    gba->run_until(159 * 1232);
    CHECK(gba->bus().read32(0x02000000) == 0);
    gba->run_frame();
    CHECK(gba->bus().read32(0x02000000) == 0xCAFEBABE);
    CHECK((gba->bus().read16(kDma3 + 10) & 0x8000) == 0);  // no repeat: done
}

TEST_CASE("DMA0 cannot read the cartridge; DMA count 0 means the maximum") {
    auto gba = idle_gba();
    dma(*gba, kDma0, 0x08000000, 0x02000000, 1, 0x8400);  // ROM masked to 0x00000000 (BIOS)
    CHECK(gba->bus().read32(0x02000000) == gba->bus().read32(0x00000000));
    gba->bus().write16(0x03000000, 0x7777);
    dma(*gba, kDma3, 0x03000000, 0x02000000, 0, 0x8000 | (2U << 7));  // 0 -> 0x10000 halfwords
    CHECK(gba->bus().read16(0x0201FFFE) == 0x7777);  // the 65536th halfword
}
