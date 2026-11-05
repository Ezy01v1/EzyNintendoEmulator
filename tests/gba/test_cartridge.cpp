#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "gba/cartridge.hpp"

using namespace ezy::gba;

namespace {

std::vector<std::uint8_t> make_rom(std::string_view title, std::string_view code) {
    std::vector<std::uint8_t> rom(0x200, 0);
    for (std::size_t i = 0; i < title.size(); ++i) rom[kTitleAddr + i] = static_cast<std::uint8_t>(title[i]);
    for (std::size_t i = 0; i < code.size(); ++i) rom[kGameCodeAddr + i] = static_cast<std::uint8_t>(code[i]);
    rom[kMakerCodeAddr] = '0';
    rom[kMakerCodeAddr + 1] = '1';
    rom[kFixedValueAddr] = 0x96;
    rom[kVersionAddr] = 2;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return rom;
}

}  // namespace

TEST_CASE("GBA header fields are parsed") {
    const auto header = parse_header(make_rom("EZYTEST", "AEZE"));
    REQUIRE(header);
    CHECK(header->title == "EZYTEST");
    CHECK(header->game_code == "AEZE");
    CHECK(header->maker_code == "01");
    CHECK(header->version == 2);
    CHECK(header->fixed_value_ok);
    CHECK(header->header_checksum_ok);
}

TEST_CASE("GBA header checksum of an all-zero header is -0x19") {
    const std::vector<std::uint8_t> rom(kHeaderEnd, 0);
    CHECK(compute_header_checksum(rom) == 0xE7);
}

TEST_CASE("GBA header checksum mismatch is reported, not rejected") {
    auto rom = make_rom("X", "XXXX");
    rom[kHeaderChecksumAddr] ^= 0xFF;
    const auto header = parse_header(rom);
    REQUIRE(header);
    CHECK_FALSE(header->header_checksum_ok);
}

TEST_CASE("GBA ROMs smaller than the header or over 32 MiB are rejected") {
    CHECK_FALSE(parse_header(std::vector<std::uint8_t>(kHeaderEnd - 1, 0)));
    CHECK_FALSE(parse_header(std::vector<std::uint8_t>(kMaxRomSize + 1, 0)));
    CHECK(parse_header(std::vector<std::uint8_t>(kHeaderEnd, 0)));
}
