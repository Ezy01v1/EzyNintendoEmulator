#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include "gb/cartridge.hpp"

using namespace ezy::gb;

namespace {

// Builds a synthetic 32 KiB ROM with a valid header.
std::vector<std::uint8_t> make_rom(std::string_view title, std::uint8_t type = 0x00, std::uint8_t rom_code = 0x00,
                                   std::uint8_t ram_code = 0x00) {
    std::vector<std::uint8_t> rom(32 * 1024, 0);
    for (std::size_t i = 0; i < title.size(); ++i) {
        rom[kTitleStart + i] = static_cast<std::uint8_t>(title[i]);
    }
    rom[kCartTypeAddr] = type;
    rom[kRomSizeAddr] = rom_code;
    rom[kRamSizeAddr] = ram_code;
    rom[kHeaderChecksumAddr] = compute_header_checksum(rom);
    return rom;
}

}  // namespace

TEST_CASE("parse_header reads title, type and sizes") {
    const auto rom = make_rom("TESTGAME", 0x03, 0x02, 0x03);
    const auto header = parse_header(rom);
    REQUIRE(header);
    CHECK(header->title == "TESTGAME");
    CHECK(header->cart_type == 0x03);
    CHECK(cart_type_name(header->cart_type) == "MBC1+RAM+BATTERY");
    CHECK(header->rom_size == 128 * 1024);
    CHECK(header->ram_size == 32 * 1024);
    CHECK(header->header_checksum_ok);
}

TEST_CASE("title uses all 16 bytes when there is no zero padding") {
    const auto header = parse_header(make_rom("ABCDEFGHIJKLMNOP"));
    REQUIRE(header);
    CHECK(header->title == "ABCDEFGHIJKLMNOP");
}

TEST_CASE("header checksum mismatch is detected") {
    auto rom = make_rom("BROKEN");
    rom[kHeaderChecksumAddr] ^= 0xFF;
    const auto header = parse_header(rom);
    REQUIRE(header);
    CHECK_FALSE(header->header_checksum_ok);
}

TEST_CASE("compute_header_checksum matches a known value") {
    // All-zero header bytes: 25 iterations of x = x - 0 - 1  ->  -25 = 0xE7.
    const std::vector<std::uint8_t> rom(kHeaderEnd, 0);
    CHECK(compute_header_checksum(rom) == 0xE7);
}

TEST_CASE("parse_header rejects ROMs that are too small or have unknown sizes") {
    CHECK_FALSE(parse_header(std::vector<std::uint8_t>(kHeaderEnd - 1, 0)));
    CHECK_FALSE(parse_header(make_rom("X", 0x00, 0x09)));        // ROM size code out of range
    CHECK_FALSE(parse_header(make_rom("X", 0x00, 0x00, 0x06)));  // RAM size code out of range
}

TEST_CASE("Cartridge reads ROM bytes and returns 0xFF past the end") {
    auto rom = make_rom("READ");
    rom[0x0100] = 0x00;  // NOP
    rom[0x0101] = 0xC3;  // JP
    rom.resize(0x4000);  // shorter than the 32 KiB window
    const auto cart = Cartridge::load(rom);
    REQUIRE(cart);
    CHECK(cart->read_rom(0x0101) == 0xC3);
    CHECK(cart->read_rom(0x7FFF) == 0xFF);
}
