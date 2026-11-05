#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gb/cartridge.hpp"
#include "test_helpers.hpp"

using namespace ezy::gb;
using ezy::gb::test::make_rom;

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

// --- MBC1 external RAM ---

namespace {

// MBC1+RAM+BATTERY, 32 KiB ROM, RAM size from ram_code (0x02 = 8 KiB, 0x03 = 32 KiB).
Cartridge make_mbc1_ram(std::uint8_t ram_code) {
    return ezy::gb::test::make_cartridge(make_rom("SAVE", 0x03, 0x00, ram_code));
}

void enable_ram(Cartridge& cart) { cart.write_rom(0x0000, 0x0A); }

}  // namespace

TEST_CASE("external RAM is disabled after power on") {
    auto cart = make_mbc1_ram(0x02);
    cart.write_ram(0xA000, 0x42);
    CHECK(cart.read_ram(0xA000) == 0xFF);
    CHECK_FALSE(cart.save_dirty());
    enable_ram(cart);
    CHECK(cart.read_ram(0xA000) == 0x00);  // the write above was ignored
}

TEST_CASE("external RAM round-trips once enabled and locks again") {
    auto cart = make_mbc1_ram(0x02);
    enable_ram(cart);
    cart.write_ram(0xA000, 0x12);
    cart.write_ram(0xBFFF, 0x34);
    CHECK(cart.read_ram(0xA000) == 0x12);
    CHECK(cart.read_ram(0xBFFF) == 0x34);
    CHECK(cart.save_dirty());

    cart.write_rom(0x1FFF, 0x00);
    CHECK(cart.read_ram(0xA000) == 0xFF);
    cart.write_rom(0x0000, 0xFA);  // only the low nibble matters
    CHECK(cart.read_ram(0xA000) == 0x12);
}

TEST_CASE("32 KiB RAM banks are separate in advanced mode") {
    auto cart = make_mbc1_ram(0x03);
    enable_ram(cart);
    cart.write_rom(0x6000, 0x01);  // advanced mode
    cart.write_rom(0x4000, 0x02);
    cart.write_ram(0xA010, 0xB2);
    cart.write_rom(0x4000, 0x00);
    CHECK(cart.read_ram(0xA010) == 0x00);
    cart.write_ram(0xA010, 0xB0);
    cart.write_rom(0x4000, 0x02);
    CHECK(cart.read_ram(0xA010) == 0xB2);
    CHECK(cart.save_data()[2 * 0x2000 + 0x10] == 0xB2);

    cart.write_rom(0x6000, 0x00);  // simple mode: always bank 0
    CHECK(cart.read_ram(0xA010) == 0xB0);
}

TEST_CASE("8 KiB RAM ignores the bank register") {
    auto cart = make_mbc1_ram(0x02);
    enable_ram(cart);
    cart.write_rom(0x6000, 0x01);
    cart.write_rom(0x4000, 0x03);
    cart.write_ram(0xA000, 0x77);
    cart.write_rom(0x4000, 0x00);
    CHECK(cart.read_ram(0xA000) == 0x77);
}

TEST_CASE("cartridge without RAM reads 0xFF") {
    auto cart = ezy::gb::test::make_cartridge(make_rom("NORAM", 0x01));
    enable_ram(cart);
    cart.write_ram(0xA000, 0x55);
    CHECK(cart.read_ram(0xA000) == 0xFF);
    CHECK(cart.save_data().empty());
    CHECK_FALSE(cart.has_battery());
}

TEST_CASE("save data round-trips into a fresh cartridge") {
    auto cart = make_mbc1_ram(0x03);
    CHECK(cart.has_battery());
    enable_ram(cart);
    cart.write_rom(0x6000, 0x01);
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cart.write_rom(0x4000, bank);
        cart.write_ram(0xA000, static_cast<std::uint8_t>(0x10 + bank));
    }
    const std::vector<std::uint8_t> sav(cart.save_data().begin(), cart.save_data().end());

    auto fresh = make_mbc1_ram(0x03);
    CHECK(fresh.load_save_data(sav));
    CHECK_FALSE(fresh.save_dirty());  // loading is not a game write
    CHECK(std::ranges::equal(fresh.save_data(), sav));
}

TEST_CASE("load_save_data reports a size mismatch but keeps what fits") {
    auto cart = make_mbc1_ram(0x02);
    const std::vector<std::uint8_t> short_sav(16, 0xAB);
    CHECK_FALSE(cart.load_save_data(short_sav));
    CHECK(cart.save_data()[15] == 0xAB);
    CHECK(cart.save_data()[16] == 0x00);
}
