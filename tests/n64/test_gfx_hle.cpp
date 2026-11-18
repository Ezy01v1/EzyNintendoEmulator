#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "n64/n64.hpp"
#include "n64_test_helpers.hpp"

using namespace ezy::n64;
using namespace n64_test;

namespace {

// The fill list of "RDP runs a frozen RDRAM list...": a red/green 2x2 checker clipped by
// the scissor, drawn into a 5-pixel-wide RGBA5551 image at 0x4000.
const std::vector<std::uint64_t> kCheckerList = {
    0x3F10000400004000ULL,
    rdp_rectangle(0x2D, 1, 1, 4, 3),
    kFillMode,
    0x37000000F80107C1ULL,
    rdp_rectangle(0x36, 3, 2, 0, 0),
    kSyncFull,
};

std::uint16_t pixel(Bus& bus, unsigned x, unsigned y) {
    const std::uint32_t address = 0x4000 + (y * 5 + x) * 2;
    return static_cast<std::uint16_t>(bus.read32(address & ~3U) >> ((address & 2) ? 0 : 16));
}

}  // namespace

TEST_CASE("RDP submit_words draws like a fetched list") {
    const auto n64 = N64::create(counting_rom("HLE RDP"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    bus.write32(0x0430000C, 1U << 11);  // unmask DP
    bus.rdp().submit_words(kCheckerList, bus.mi(), bus.rdram());
    CHECK(pixel(bus, 1, 1) == 0xF801);  // red
    CHECK(pixel(bus, 2, 1) == 0x07C1);  // green
    CHECK(pixel(bus, 0, 0) == 0);       // outside the scissor
    CHECK(bus.interrupt_line());        // Sync Full
    CHECK((bus.read32(0x0410000C) & 0x20) == 0);  // pipe no longer busy after Sync Full
}

TEST_CASE("RDP trace records every consumed word") {
    const auto n64 = N64::create(counting_rom("HLE TRACE"));
    REQUIRE(n64);
    auto& bus = n64->bus();
    std::vector<std::uint64_t> trace;
    bus.rdp().set_trace(&trace);

    std::uint32_t end = 0x1000;
    for (const auto command : kCheckerList) {
        bus.write64(end, command);
        end += 8;
    }
    bus.write32(kDpcStart, 0x1000);
    bus.write32(kDpcEnd, end);
    for (unsigned i = 0; i < 40; ++i) n64->step();
    CHECK(trace == kCheckerList);

    trace.clear();
    bus.rdp().submit_words(kCheckerList, bus.mi(), bus.rdram());
    CHECK(trace == kCheckerList);
}
