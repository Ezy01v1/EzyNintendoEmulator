#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "gba/gba.hpp"

using namespace ezy::gba;

namespace {

// Counts in r1 forever, storing it to IWRAM; game code "TEST" or `code`.
std::vector<std::uint8_t> counter_rom(const char* code = "TEST") {
    std::vector<std::uint8_t> rom(0x200, 0);
    const std::uint32_t program[] = {
        0xE3A00403,  // mov r0, #0x03000000
        0xE2811001,  // add r1, r1, #1
        0xE5801000,  // str r1, [r0]
        0xEAFFFFFC,  // b 4
    };
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t b = 0; b < 4; ++b) rom[i * 4 + b] = static_cast<std::uint8_t>(program[i] >> (8 * b));
    }
    for (std::size_t i = 0; i < 4; ++i) rom[0xAC + i] = static_cast<std::uint8_t>(code[i]);
    rom[0xB2] = 0x96;
    return rom;
}

}  // namespace

TEST_CASE("a loaded save state replays exactly the same frames") {
    const auto rom = counter_rom();
    auto gba = Gba::create(rom);
    REQUIRE(gba);
    gba->bus().write16(0x04000000, 0x0403);   // mode 3, BG2: the PPU draws something
    gba->bus().write16(0x04000102, 0x0080);   // timer 0 running
    for (int i = 0; i < 3; ++i) gba->run_frame();

    const std::vector<std::uint8_t> state = gba->save_state();
    for (int i = 0; i < 5; ++i) gba->run_frame();
    const std::vector<std::uint8_t> after = gba->save_state();
    const std::uint32_t counter = gba->bus().read32(0x03000000);

    REQUIRE(gba->load_state(state));
    CHECK(gba->frame_count() == 3);
    for (int i = 0; i < 5; ++i) gba->run_frame();
    CHECK(gba->bus().read32(0x03000000) == counter);
    CHECK(gba->save_state() == after);

    // Also into a fresh machine.
    auto other = Gba::create(rom);
    REQUIRE(other->load_state(state));
    for (int i = 0; i < 5; ++i) other->run_frame();
    CHECK(other->save_state() == after);  // padding never leaks into the bytes
}

TEST_CASE("load_state rejects other games and truncated states, leaving the machine alone") {
    auto gba = Gba::create(counter_rom());
    gba->run_frame();
    std::vector<std::uint8_t> state = gba->save_state();
    gba->run_frame();
    const std::vector<std::uint8_t> now = gba->save_state();

    auto other_game = Gba::create(counter_rom("ABCD"));
    CHECK_FALSE(other_game->load_state(state));

    std::vector<std::uint8_t> truncated(state.begin(), state.end() - 100);
    CHECK_FALSE(gba->load_state(truncated));
    CHECK(gba->save_state() == now);

    state.push_back(0);  // trailing garbage
    CHECK_FALSE(gba->load_state(state));
    CHECK(gba->save_state() == now);
    CHECK_FALSE(gba->load_state({}));
}
