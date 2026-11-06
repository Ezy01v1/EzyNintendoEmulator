#include <doctest/doctest.h>

#include <cstdint>
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
