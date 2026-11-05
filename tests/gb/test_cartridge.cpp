#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <variant>
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
    const std::vector<std::uint8_t> sav = cart.save_data();

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

// --- MBC3 ---

namespace {

// MBC3+RAM+BATTERY, 2 MiB ROM (128 banks, bank number in each bank's first byte), 32 KiB RAM.
Cartridge make_mbc3(std::uint8_t type = 0x13) {
    auto rom = make_rom("MBC3", type, 0x06, 0x03);
    rom.resize(2 * 1024 * 1024);
    for (std::size_t bank = 0; bank < 128; ++bank) rom[bank * 0x4000] = static_cast<std::uint8_t>(bank);
    rom[0] = 0;  // keep bank 0 header bytes untouched (0x0000 is outside the header)
    return ezy::gb::test::make_cartridge(rom);
}

}  // namespace

TEST_CASE("MBC3 maps all 128 ROM banks, bank 0 selects 1") {
    auto cart = make_mbc3();
    REQUIRE(std::holds_alternative<Mbc3>(cart.mbc()));
    CHECK(cart.read_rom(0x0000) == 0);
    for (int bank = 1; bank < 128; ++bank) {
        cart.write_rom(0x2000, static_cast<std::uint8_t>(bank));
        CHECK(cart.read_rom(0x4000) == bank);
        CHECK(cart.read_rom(0x0000) == 0);  // no MBC1-style upper bits in bank 0 area
    }
    cart.write_rom(0x2000, 0x00);
    CHECK(cart.read_rom(0x4000) == 1);
    cart.write_rom(0x3FFF, 0xA0);  // only 7 bits: 0x20
    CHECK(cart.read_rom(0x4000) == 0x20);
}

TEST_CASE("MBC3 RAM banks are separate and gated by enable") {
    auto cart = make_mbc3();
    cart.write_ram(0xA000, 0x11);
    CHECK(cart.read_ram(0xA000) == 0xFF);
    cart.write_rom(0x0000, 0x0A);
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cart.write_rom(0x4000, bank);
        cart.write_ram(0xA123, static_cast<std::uint8_t>(0xC0 + bank));
    }
    for (std::uint8_t bank = 0; bank < 4; ++bank) {
        cart.write_rom(0x4000, bank);
        CHECK(cart.read_ram(0xA123) == 0xC0 + bank);
    }
    CHECK(cart.save_data()[3 * 0x2000 + 0x123] == 0xC3);
    CHECK(cart.save_dirty());
}

// --- MBC3 RTC ---

namespace {

constexpr std::uint32_t kSecond = Rtc::kCyclesPerSecond;

// MBC3+TIMER+RAM+BATTERY with RAM and RTC enabled.
Cartridge make_rtc_cart() {
    auto cart = make_mbc3(0x10);
    cart.write_rom(0x0000, 0x0A);
    return cart;
}

void latch(Cartridge& cart) {
    cart.write_rom(0x6000, 0x00);
    cart.write_rom(0x6000, 0x01);
}

std::uint8_t read_rtc(Cartridge& cart, std::uint8_t reg) {
    cart.write_rom(0x4000, reg);
    return cart.read_ram(0xA000);
}

void write_rtc(Cartridge& cart, std::uint8_t reg, std::uint8_t value) {
    cart.write_rom(0x4000, reg);
    cart.write_ram(0xA000, value);
}

// Ticks in M-cycle steps like the bus does.
void run_cycles(Cartridge& cart, std::uint64_t t_cycles) {
    for (std::uint64_t i = 0; i < t_cycles / 4; ++i) cart.tick(4);
}

}  // namespace

TEST_CASE("RTC registers are only visible on MBC3+TIMER carts") {
    auto plain = make_mbc3(0x13);
    CHECK_FALSE(plain.has_rtc());
    plain.write_rom(0x0000, 0x0A);
    write_rtc(plain, 0x08, 30);
    CHECK(read_rtc(plain, 0x08) == 0xFF);

    auto cart = make_rtc_cart();
    CHECK(cart.has_rtc());
    write_rtc(cart, 0x08, 30);
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 30);
    CHECK(cart.save_dirty());
}

TEST_CASE("RTC advances one second per 4194304 cycles, reads need a latch") {
    auto cart = make_rtc_cart();
    run_cycles(cart, kSecond - 4);
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 0);
    run_cycles(cart, 4);
    CHECK(read_rtc(cart, 0x08) == 0);  // still the old latched copy
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 1);
}

TEST_CASE("RTC latch needs 0x00 then 0x01") {
    auto cart = make_rtc_cart();
    run_cycles(cart, kSecond);
    cart.write_rom(0x6000, 0x01);
    CHECK(read_rtc(cart, 0x08) == 0);
    cart.write_rom(0x6000, 0x00);
    cart.write_rom(0x6000, 0x01);
    CHECK(read_rtc(cart, 0x08) == 1);
}

TEST_CASE("RTC carries seconds, minutes, hours and days, and sets day carry") {
    auto cart = make_rtc_cart();
    write_rtc(cart, 0x08, 59);
    write_rtc(cart, 0x09, 59);
    write_rtc(cart, 0x0A, 23);
    write_rtc(cart, 0x0B, 0xFF);
    write_rtc(cart, 0x0C, 0x01);  // day 511
    run_cycles(cart, kSecond);
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 0);
    CHECK(read_rtc(cart, 0x09) == 0);
    CHECK(read_rtc(cart, 0x0A) == 0);
    CHECK(read_rtc(cart, 0x0B) == 0);
    CHECK(read_rtc(cart, 0x0C) == Rtc::kDayCarry);  // day 0, carry set
}

TEST_CASE("RTC day counter crosses into bit 8") {
    auto cart = make_rtc_cart();
    write_rtc(cart, 0x08, 59);
    write_rtc(cart, 0x09, 59);
    write_rtc(cart, 0x0A, 23);
    write_rtc(cart, 0x0B, 0xFF);
    run_cycles(cart, kSecond);
    latch(cart);
    CHECK(read_rtc(cart, 0x0B) == 0);
    CHECK(read_rtc(cart, 0x0C) == 0x01);
}

TEST_CASE("RTC out-of-range seconds wrap at 64 without carrying") {
    auto cart = make_rtc_cart();
    write_rtc(cart, 0x08, 63);
    run_cycles(cart, kSecond);
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 0);
    CHECK(read_rtc(cart, 0x09) == 0);
}

TEST_CASE("RTC halt stops the clock") {
    auto cart = make_rtc_cart();
    write_rtc(cart, 0x0C, Rtc::kHalt);
    run_cycles(cart, 3ULL * kSecond);
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 0);
    write_rtc(cart, 0x0C, 0x00);
    run_cycles(cart, kSecond);
    latch(cart);
    CHECK(read_rtc(cart, 0x08) == 1);
}

TEST_CASE("RTC .sav footer round-trips and adds elapsed wall time") {
    auto cart = make_rtc_cart();
    write_rtc(cart, 0x08, 10);
    write_rtc(cart, 0x09, 20);
    write_rtc(cart, 0x0A, 3);
    latch(cart);
    cart.write_rom(0x4000, 0x00);
    cart.write_ram(0xA000, 0x5A);  // a RAM byte

    constexpr std::int64_t kSavedAt = 1'700'000'000;
    const auto sav = cart.save_data(kSavedAt);
    REQUIRE(sav.size() == 32 * 1024 + 48);
    CHECK(sav[32 * 1024 + 0] == 10);   // seconds, u32 LE
    CHECK(sav[32 * 1024 + 4] == 20);   // minutes, u32 LE
    CHECK(sav[32 * 1024 + 20] == 10);  // latched seconds

    // Reload 1 day, 2 hours and 5 seconds later.
    auto fresh = make_mbc3(0x10);
    CHECK(fresh.load_save_data(sav, kSavedAt + 86400 + 2 * 3600 + 5));
    fresh.write_rom(0x0000, 0x0A);
    CHECK(fresh.read_ram(0xA000) == 0x5A);
    latch(fresh);
    CHECK(read_rtc(fresh, 0x08) == 15);
    CHECK(read_rtc(fresh, 0x09) == 20);
    CHECK(read_rtc(fresh, 0x0A) == 5);
    CHECK(read_rtc(fresh, 0x0B) == 1);
}

TEST_CASE("RTC .sav with a wrong footer size is rejected") {
    auto cart = make_mbc3(0x10);
    const std::vector<std::uint8_t> sav(32 * 1024 + 10, 0);
    CHECK_FALSE(cart.load_save_data(sav));
}
