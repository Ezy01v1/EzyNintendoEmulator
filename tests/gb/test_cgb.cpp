#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "gb/gameboy.hpp"
#include "test_helpers.hpp"

using namespace ezy::gb;
using ezy::gb::test::make_rom;

namespace {

// A ROM whose header declares CGB support (0x80 = works on both, 0xC0 = CGB only).
std::vector<std::uint8_t> make_cgb_rom(std::uint8_t flag = 0x80) {
    auto rom = make_rom("CGBTEST");
    rom[kCgbFlagAddr] = flag;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return rom;
}

}  // namespace

TEST_CASE("the CGB flag is not part of the title") {
    const auto header = parse_header(make_cgb_rom());
    REQUIRE(header);
    CHECK(header->title == "CGBTEST");
    CHECK(header->cgb_flag == 0x80);
}

TEST_CASE("auto model runs every ROM as CGB") {
    const auto dmg_game = GameBoy::create(make_rom());
    REQUIRE(dmg_game);
    CHECK(dmg_game->model() == Model::Cgb);
    CHECK_FALSE(dmg_game->bus().cgb_mode());

    for (const std::uint8_t flag : {std::uint8_t{0x80}, std::uint8_t{0xC0}}) {
        const auto cgb_game = GameBoy::create(make_cgb_rom(flag));
        REQUIRE(cgb_game);
        CHECK(cgb_game->model() == Model::Cgb);
        CHECK(cgb_game->bus().cgb_mode());
    }
}

TEST_CASE("forcing the DMG model") {
    const auto gb = GameBoy::create(make_cgb_rom(), Options{.model = ModelChoice::Dmg});
    REQUIRE(gb);
    CHECK(gb->model() == Model::Dmg);
    CHECK_FALSE(gb->bus().cgb_mode());
    CHECK(gb->cpu().regs().af() == 0x01B0);  // unchanged DMG post-boot state
}

TEST_CASE("CGB-E post-boot registers for a CGB game") {
    const auto gb = GameBoy::create(make_cgb_rom());
    REQUIRE(gb);
    const auto& r = gb->cpu().regs();
    CHECK(r.af() == 0x1180);
    CHECK(r.bc() == 0x0000);
    CHECK(r.de() == 0xFF56);
    CHECK(r.hl() == 0x000D);
    CHECK(r.sp == 0xFFFE);
    CHECK(r.pc == 0x0100);
}

TEST_CASE("CGB-E post-boot registers for a DMG game in compatibility mode") {
    // Non-Nintendo licensee: B = 0 and HL = 0x007C.
    const auto gb = GameBoy::create(make_rom());
    REQUIRE(gb);
    const auto& r = gb->cpu().regs();
    CHECK(r.af() == 0x1180);
    CHECK(r.bc() == 0x0000);
    CHECK(r.de() == 0x0008);
    CHECK(r.hl() == 0x007C);

    // Nintendo licensee: B = sum of the title bytes.
    auto rom = make_rom("TEST");
    rom[kOldLicenseeAddr] = 0x01;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    const auto nintendo = GameBoy::create(rom);
    REQUIRE(nintendo);
    const std::uint8_t sum = 'T' + 'E' + 'S' + 'T';
    CHECK(nintendo->cpu().regs().b == sum);
}

namespace {

Bus make_cgb_bus() { return Bus(ezy::gb::test::make_cartridge(make_cgb_rom()), Model::Cgb); }

}  // namespace

TEST_CASE("VBK switches the 0x8000 window between two banks") {
    Bus bus = make_cgb_bus();
    CHECK(bus.peek(0xFF4F) == 0xFE);
    bus.poke(0x8000, 0x11);
    bus.poke(0xFF4F, 0x01);
    CHECK(bus.peek(0xFF4F) == 0xFF);
    CHECK(bus.peek(0x8000) == 0x00);
    bus.poke(0x8000, 0x22);
    bus.poke(0xFF4F, 0x00);
    CHECK(bus.peek(0x8000) == 0x11);
    CHECK(bus.ppu().read_vram_bank(1, 0x8000) == 0x22);
}

TEST_CASE("SVBK selects the WRAM bank at 0xD000, and 0 selects bank 1") {
    Bus bus = make_cgb_bus();
    CHECK(bus.peek(0xFF70) == 0xF9);  // bank 1 after boot
    bus.poke(0xD000, 0x01);
    bus.poke(0xFF70, 0x02);
    CHECK(bus.peek(0xD000) == 0x00);
    bus.poke(0xD000, 0x02);
    bus.poke(0xFF70, 0x00);
    CHECK(bus.wram_bank() == 1);
    CHECK(bus.peek(0xFF70) == 0xF8);
    CHECK(bus.peek(0xD000) == 0x01);
    bus.poke(0xFF70, 0x07);
    CHECK(bus.wram_bank() == 7);
}

TEST_CASE("WRAM bank 0 at 0xC000 never switches, echo RAM mirrors the banked WRAM") {
    Bus bus = make_cgb_bus();
    bus.poke(0xC010, 0xAA);
    bus.poke(0xFF70, 0x03);
    bus.poke(0xD010, 0xBB);
    CHECK(bus.peek(0xC010) == 0xAA);
    CHECK(bus.peek(0xE010) == 0xAA);
    CHECK(bus.peek(0xF010) == 0xBB);
}

TEST_CASE("CGB registers are invisible in DMG compatibility mode") {
    Bus bus(ezy::gb::test::make_cartridge(make_rom()), Model::Cgb);
    REQUIRE_FALSE(bus.cgb_mode());
    bus.poke(0x8000, 0x11);
    bus.poke(0xFF4F, 0x01);
    CHECK(bus.peek(0xFF4F) == 0xFF);
    CHECK(bus.peek(0x8000) == 0x11);
    bus.poke(0xFF70, 0x03);
    CHECK(bus.peek(0xFF70) == 0xFF);
    CHECK(bus.wram_bank() == 1);
    CHECK(bus.opri() == 1);  // compatibility mode: sprites ordered by X like a DMG
    CHECK(bus.peek(0xFF74) == 0xFF);
}

TEST_CASE("CGB registers are invisible on a DMG") {
    Bus bus(ezy::gb::test::make_cartridge(make_cgb_rom()), Model::Dmg);
    bus.poke(0xFF4F, 0x01);
    CHECK(bus.peek(0xFF4F) == 0xFF);
    CHECK(bus.peek(0xFF56) == 0xFF);
    CHECK(bus.peek(0xFF6C) == 0xFF);
}

TEST_CASE("OPRI, the undocumented registers and RP in CGB mode") {
    Bus bus = make_cgb_bus();
    CHECK(bus.opri() == 0);
    CHECK(bus.peek(0xFF6C) == 0xFE);
    bus.poke(0xFF72, 0x5A);
    bus.poke(0xFF73, 0xA5);
    bus.poke(0xFF74, 0x3C);
    bus.poke(0xFF75, 0xFF);
    CHECK(bus.peek(0xFF72) == 0x5A);
    CHECK(bus.peek(0xFF73) == 0xA5);
    CHECK(bus.peek(0xFF74) == 0x3C);
    CHECK(bus.peek(0xFF75) == 0xFF);
    bus.poke(0xFF75, 0x00);
    CHECK(bus.peek(0xFF75) == 0x8F);
    // RP: bit 1 reads 1 = no infrared light received.
    CHECK(bus.peek(0xFF56) == 0x3E);
    bus.poke(0xFF56, 0xC1);
    CHECK(bus.peek(0xFF56) == 0xFF);
}

namespace {

// Runs a tiny program from WRAM: the instructions in `code`, then JR -2 forever.
void load_program(GameBoy& gb, std::initializer_list<std::uint8_t> code) {
    std::uint16_t addr = 0xC000;
    for (const auto byte : code) gb.bus().poke(addr++, byte);
    gb.bus().poke(addr++, 0x18);
    gb.bus().poke(addr, 0xFE);
    gb.cpu().regs().pc = 0xC000;
}

}  // namespace

TEST_CASE("STOP with KEY1 armed switches to double speed") {
    auto gb = GameBoy::create(make_cgb_rom());
    REQUIRE(gb);
    CHECK(gb->bus().peek(0xFF4D) == 0x7E);
    // LD A,1; LDH (0x4D),A; STOP 0
    load_program(*gb, {0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00});
    for (int i = 0; i < 3; ++i) gb->step();
    CHECK(gb->bus().double_speed());
    CHECK(gb->bus().peek(0xFF4D) == 0xFE);
    CHECK(gb->bus().peek(0xFF04) == 0x00 + (2050 * 4 >> 8));  // DIV reset, then the switch delay
}

TEST_CASE("STOP without arming does not switch and resets DIV") {
    auto gb = GameBoy::create(make_cgb_rom());
    REQUIRE(gb);
    load_program(*gb, {0x10, 0x00});
    gb->step();
    CHECK_FALSE(gb->bus().double_speed());
    CHECK(gb->bus().peek(0xFF04) == 0x00);
}

TEST_CASE("KEY1 does not exist outside CGB mode") {
    auto gb = GameBoy::create(make_rom());
    REQUIRE(gb);
    gb->bus().poke(0xFF4D, 0x01);
    CHECK(gb->bus().peek(0xFF4D) == 0xFF);
    load_program(*gb, {0x10, 0x00});
    gb->step();
    CHECK_FALSE(gb->bus().double_speed());
}

TEST_CASE("a frame takes twice the CPU M-cycles in double speed") {
    auto gb = GameBoy::create(make_cgb_rom());
    REQUIRE(gb);
    load_program(*gb, {0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00});
    for (int i = 0; i < 3; ++i) gb->step();
    REQUIRE(gb->bus().double_speed());
    gb->run_frame();  // sync to a frame boundary
    const auto t0 = gb->bus().t_cycles();
    const auto d0 = gb->bus().dots();
    gb->run_frame();
    CHECK(gb->bus().dots() - d0 == kDotsPerFrame);
    CHECK(gb->bus().t_cycles() - t0 == 2 * kDotsPerFrame);
}

TEST_CASE("timer runs at CPU speed in double speed") {
    auto gb = GameBoy::create(make_cgb_rom());
    REQUIRE(gb);
    load_program(*gb, {0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00});
    for (int i = 0; i < 3; ++i) gb->step();
    gb->bus().poke(0xFF04, 0);
    for (int i = 0; i < 64; ++i) gb->bus().tick();  // 64 CPU M-cycles = 256 CPU T-cycles
    CHECK(gb->bus().peek(0xFF04) == 1);
    CHECK(gb->bus().dots() % 2 == 0);
}

TEST_CASE("a DMG game on a CGB gets its compatibility palette at power-on") {
    auto gb = GameBoy::create(make_rom());  // non-Nintendo: the default palette
    REQUIRE(gb);
    const auto ram = gb->bus().ppu().bg_palette_ram();
    CHECK((ram[2] | (ram[3] << 8)) == 0x1BEF);
}

namespace {

// A fake 2304-byte CGB boot ROM: LD A,0x11; LDH (0x50),A; then 0xAA filler.
std::vector<std::uint8_t> make_boot_rom() {
    std::vector<std::uint8_t> boot(0x900, 0xAA);
    const std::array<std::uint8_t, 4> code = {0x3E, 0x11, 0xE0, 0x50};
    std::copy(code.begin(), code.end(), boot.begin());
    return boot;
}

}  // namespace

TEST_CASE("boot ROM overlays 0x0000-0x00FF and 0x0200-0x08FF but not the header") {
    auto rom = make_cgb_rom();
    rom[0x0005] = 0x55;
    rom[0x0300] = 0x66;
    auto gb = GameBoy::create(rom, Options{.boot_rom = make_boot_rom()});
    REQUIRE(gb);
    CHECK(gb->cpu().regs().pc == 0x0000);
    CHECK(gb->cpu().regs().af() == 0x0000);
    CHECK(gb->bus().peek(0x0000) == 0x3E);
    CHECK(gb->bus().peek(0x0005) == 0xAA);
    CHECK(gb->bus().peek(0x0134) == 'C');  // header from the cartridge
    CHECK(gb->bus().peek(0x0300) == 0xAA);
    CHECK(gb->bus().peek(0x0900) == rom[0x0900]);
}

TEST_CASE("writing 0xFF50 unmaps the boot ROM") {
    auto rom = make_cgb_rom();
    rom[0x0005] = 0x55;
    auto gb = GameBoy::create(rom, Options{.boot_rom = make_boot_rom()});
    REQUIRE(gb);
    gb->step();
    gb->step();
    CHECK(gb->bus().peek(0x0005) == 0x55);
    CHECK(gb->bus().cgb_mode());
}

TEST_CASE("KEY0 written by the boot ROM selects DMG compatibility mode") {
    auto gb = GameBoy::create(make_rom(), Options{.boot_rom = make_boot_rom()});
    REQUIRE(gb);
    CHECK(gb->bus().cgb_mode());  // the boot ROM itself runs in CGB mode
    gb->bus().poke(0xFF4C, 0x04);
    gb->bus().poke(0xFF50, 0x11);
    CHECK_FALSE(gb->bus().cgb_mode());
    gb->bus().poke(0xFF4C, 0x00);  // locked now
    gb->bus().poke(0xFF4F, 0x01);
    CHECK(gb->bus().peek(0xFF4F) == 0xFF);
}

TEST_CASE("wrong-size boot ROM is rejected") {
    CHECK(GameBoy::create(make_cgb_rom(), Options{.boot_rom = std::vector<std::uint8_t>(0x100)}) == nullptr);
}
