#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "n64/cartridge.hpp"

using namespace ezy::n64;

namespace {

// A big-endian (.z64) ROM: the magic word, a title and a region code, the rest zero.
std::vector<std::uint8_t> make_z64(std::size_t size = 0x2000, char region = 'E') {
    std::vector<std::uint8_t> rom(size, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    const std::string title = "EZY TEST ROM";
    for (std::size_t i = 0; i < title.size(); ++i) rom[0x20 + i] = static_cast<std::uint8_t>(title[i]);
    for (std::size_t i = title.size(); i < 20; ++i) rom[0x20 + i] = ' ';
    rom[0x3E] = static_cast<std::uint8_t>(region);
    for (std::size_t i = 0x1000; i < size; ++i) rom[i] = static_cast<std::uint8_t>(i * 7);
    return rom;
}

std::vector<std::uint8_t> to_v64(std::vector<std::uint8_t> rom) {  // byte-swapped halfwords
    for (std::size_t i = 0; i + 1 < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
    return rom;
}

std::vector<std::uint8_t> to_n64(std::vector<std::uint8_t> rom) {  // little-endian words
    for (std::size_t i = 0; i + 3 < rom.size(); i += 4) {
        std::swap(rom[i], rom[i + 3]);
        std::swap(rom[i + 1], rom[i + 2]);
    }
    return rom;
}

}  // namespace

TEST_CASE("z64, v64 and n64 files load to the same big-endian rom") {
    const auto z64 = make_z64();
    const auto a = Cartridge::load(z64);
    const auto b = Cartridge::load(to_v64(z64));
    const auto c = Cartridge::load(to_n64(z64));
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(std::vector<std::uint8_t>(a->rom().begin(), a->rom().end()) == z64);
    CHECK(std::vector<std::uint8_t>(b->rom().begin(), b->rom().end()) == z64);
    CHECK(std::vector<std::uint8_t>(c->rom().begin(), c->rom().end()) == z64);
    CHECK(a->read32(0) == 0x80371240);
    CHECK(a->read32(0x2000) == 0);  // past the end
}

TEST_CASE("title and region come from the header") {
    CHECK(Cartridge::load(make_z64(0x2000, 'E'))->title() == "EZY TEST ROM");
    CHECK(Cartridge::load(make_z64(0x2000, 'E'))->region() == Region::Ntsc);
    CHECK(Cartridge::load(make_z64(0x2000, 'J'))->region() == Region::Ntsc);
    CHECK(Cartridge::load(make_z64(0x2000, 'P'))->region() == Region::Pal);
    CHECK(Cartridge::load(make_z64(0x2000, 'D'))->region() == Region::Pal);
    CHECK(Cartridge::load(make_z64(0x2000, 'B'))->region() == Region::Mpal);
}

TEST_CASE("unknown IPL3 falls back to CIC 6102") {
    CHECK(Cartridge::load(make_z64())->cic() == Cic::Cic6102);
    CHECK(Cartridge::load(make_z64(0x2000, 'P'))->cic() == Cic::Cic7101);  // PAL 6102 is a 7101
}

TEST_CASE("CIC seeds") {
    CHECK(cic_seed(Cic::Cic6101) == 0x3F);
    CHECK(cic_seed(Cic::Cic6102) == 0x3F);
    CHECK(cic_seed(Cic::Cic6103) == 0x78);
    CHECK(cic_seed(Cic::Cic6105) == 0x91);
    CHECK(cic_seed(Cic::Cic6106) == 0x85);
    CHECK(cic_seed(Cic::Cic7101) == 0x3F);
}

TEST_CASE("rom smaller than the header and IPL3 is refused") {
    CHECK_FALSE(Cartridge::load(make_z64(0x0FFC)));
    CHECK_FALSE(Cartridge::load(std::vector<std::uint8_t>{}));
    CHECK(Cartridge::load(make_z64(0x1000)));
}

TEST_CASE("unknown first word is refused") {
    auto rom = make_z64();
    rom[0] = 0x12;
    CHECK_FALSE(Cartridge::load(rom));
}

TEST_CASE("odd-sized v64 file is refused, odd-sized z64 is padded") {
    auto v64 = to_v64(make_z64(0x2000));
    v64.push_back(0xAA);  // half a halfword
    CHECK_FALSE(Cartridge::load(v64));
    auto z64 = make_z64(0x2000);
    z64.push_back(0xAA);
    const auto cart = Cartridge::load(z64);
    REQUIRE(cart);
    CHECK(cart->read32(0x2000) == 0xAA000000);
}
