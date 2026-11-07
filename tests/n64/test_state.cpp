#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mips_asm.hpp"
#include "n64/n64.hpp"

using namespace ezy::n64;

namespace {

// A ROM whose "IPL3" (at 0x40, run from SP DMEM at 0xA4000040) counts in t1 and stores
// the count to RDRAM forever.
std::vector<std::uint8_t> counting_rom(const std::string& title) {
    std::vector<std::uint8_t> rom(0x2000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    for (std::size_t i = 0; i < title.size() && i < 20; ++i) rom[0x20 + i] = static_cast<std::uint8_t>(title[i]);
    rom[0x3E] = 'E';
    using namespace mips;
    constexpr int t0 = 8, t1 = 9;
    const std::uint32_t program[] = {lui(t0, 0x8000), addiu(t1, t1, 1), sw(t1, t0, 0x100), j(0x02, 0xA4000044), nop()};
    std::size_t at = 0x40;
    for (const std::uint32_t w : program) {
        rom[at] = static_cast<std::uint8_t>(w >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(w >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(w >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(w);
        at += 4;
    }
    return rom;
}

}  // namespace

TEST_CASE("the console boots the IPL3 code and reaches VBlank") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    REQUIRE(n64);
    n64->run_frame();
    CHECK(n64->cpu().gpr(9) > 1000);
    // The last count stored to RDRAM is the register or, mid-loop, one behind it.
    const std::uint32_t stored = n64->bus().read32(0x100);
    const auto count = static_cast<std::uint32_t>(n64->cpu().gpr(9));
    CHECK(count - stored <= 1);
}

TEST_CASE("save state round trip resumes identically") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    REQUIRE(n64);
    n64->run_frame();
    const std::vector<std::uint8_t> state = n64->save_state();
    n64->run_frame();
    n64->run_frame();
    const std::uint64_t t1_a = n64->cpu().gpr(9);
    const std::uint64_t cycles_a = n64->cycles();
    REQUIRE(n64->load_state(state));
    n64->run_frame();
    n64->run_frame();
    CHECK(n64->cpu().gpr(9) == t1_a);
    CHECK(n64->cycles() == cycles_a);
}

TEST_CASE("save state mid-DMA resumes at the same cycle") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    REQUIRE(n64);
    n64->bus().write32(0x04600000, 0x10000);      // PI_DRAM_ADDR
    n64->bus().write32(0x04600004, 0x10000000);   // PI_CART_ADDR
    n64->bus().write32(0x0460000C, 0x1FFF);       // 8 KB
    REQUIRE(n64->bus().pi().busy());
    const std::vector<std::uint8_t> state = n64->save_state();
    while (n64->bus().pi().busy()) n64->step();
    const std::uint64_t end_a = n64->cycles();
    REQUIRE(n64->load_state(state));
    CHECK(n64->bus().pi().busy());
    while (n64->bus().pi().busy()) n64->step();
    CHECK(n64->cycles() == end_a);
}

TEST_CASE("state of another ROM is refused") {
    const auto a = N64::create(counting_rom("GAME A"));
    const auto b = N64::create(counting_rom("GAME B"));
    REQUIRE(a);
    REQUIRE(b);
    a->run_frame();
    const std::uint64_t t1 = b->cpu().gpr(9);
    CHECK_FALSE(b->load_state(a->save_state()));
    CHECK(b->cpu().gpr(9) == t1);
}

TEST_CASE("a truncated state is refused and leaves the machine as it was") {
    const auto n64 = N64::create(counting_rom("STATE TEST"));
    n64->run_frame();
    std::vector<std::uint8_t> state = n64->save_state();
    state.resize(state.size() / 2);
    const std::uint64_t t1 = n64->cpu().gpr(9);
    CHECK_FALSE(n64->load_state(state));
    CHECK(n64->cpu().gpr(9) == t1);
}

TEST_CASE("create refuses a file that isn't an N64 ROM") {
    const std::vector<std::uint8_t> junk(0x2000, 0x55);
    CHECK_FALSE(N64::create(junk));
}
