#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gb/gameboy.hpp"
#include "test_helpers.hpp"

using namespace ezy::gb;

namespace {

// A CGB ROM whose program keeps changing VRAM, WRAM and palettes, so frames differ.
std::vector<std::uint8_t> make_busy_rom(std::string_view title = "STATE") {
    auto rom = test::make_rom(title, 0x1B, 0x00, 0x02);  // MBC5+RAM+BATTERY, 8 KiB RAM
    rom[kCgbFlagAddr] = 0x80;
    // 0x0100: JP 0x0150
    rom[0x100] = 0xC3;
    rom[0x101] = 0x50;
    rom[0x102] = 0x01;
    // 0x0150: INC A; LD (0x9800),A ... crude but deterministic:
    //   loop: INC A; LD (HL+),A; LDH (0x69),A; JR loop
    const std::vector<std::uint8_t> code = {0x21, 0x00, 0x80,  // LD HL,0x8000
                                            0x3C, 0x22, 0xE0, 0x69, 0x18, 0xFA};
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return rom;
}

void run_frames(GameBoy& gb, int n) {
    for (int i = 0; i < n; ++i) gb.run_frame();
}

}  // namespace

TEST_CASE("save then load restores identical frames") {
    auto gb = GameBoy::create(make_busy_rom());
    REQUIRE(gb);
    run_frames(*gb, 5);
    const auto state = gb->save_state();
    run_frames(*gb, 3);
    const std::vector<std::uint16_t> frame_a(gb->framebuffer().begin(), gb->framebuffer().end());
    const auto cycles_a = gb->bus().t_cycles();
    const auto pc_a = gb->cpu().regs().pc;

    REQUIRE(gb->load_state(state));
    run_frames(*gb, 3);
    CHECK(gb->bus().t_cycles() == cycles_a);
    CHECK(gb->cpu().regs().pc == pc_a);
    CHECK(std::vector<std::uint16_t>(gb->framebuffer().begin(), gb->framebuffer().end()) == frame_a);
}

TEST_CASE("a state from another ROM is rejected and the machine is untouched") {
    auto a = GameBoy::create(make_busy_rom("GAME A"));
    auto b = GameBoy::create(make_busy_rom("GAME B"));
    REQUIRE(a);
    REQUIRE(b);
    run_frames(*a, 2);
    const auto pc = b->cpu().regs().pc;
    const auto cycles = b->bus().t_cycles();
    CHECK_FALSE(b->load_state(a->save_state()));
    CHECK(b->cpu().regs().pc == pc);
    CHECK(b->bus().t_cycles() == cycles);
}

TEST_CASE("a state from the other model is rejected") {
    auto cgb = GameBoy::create(make_busy_rom());
    auto dmg = GameBoy::create(make_busy_rom(), Options{.model = ModelChoice::Dmg});
    REQUIRE(cgb);
    REQUIRE(dmg);
    CHECK_FALSE(dmg->load_state(cgb->save_state()));
}

TEST_CASE("a truncated state is rejected and the machine is untouched") {
    auto gb = GameBoy::create(make_busy_rom());
    REQUIRE(gb);
    run_frames(*gb, 2);
    auto state = gb->save_state();
    run_frames(*gb, 1);
    const auto cycles = gb->bus().t_cycles();
    const auto vram = gb->bus().peek(0x8000);
    state.resize(state.size() - 100);
    CHECK_FALSE(gb->load_state(state));
    CHECK(gb->bus().t_cycles() == cycles);
    CHECK(gb->bus().peek(0x8000) == vram);
}

TEST_CASE("a state with another format version is rejected") {
    auto gb = GameBoy::create(make_busy_rom());
    REQUIRE(gb);
    auto state = gb->save_state();
    state[4] ^= 0xFF;  // the version follows the 4-byte magic
    CHECK_FALSE(gb->load_state(state));
}
