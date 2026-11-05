#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "gba/gba.hpp"

using namespace ezy::gba;

namespace {

constexpr std::uint32_t kRom = 0x08000000;
constexpr std::uint32_t kData = 0x100;  // ROM offset of the test's input data
constexpr std::uint32_t kEwram = 0x02000000;

// A ROM that loads r0-r3 from a literal pool, calls SWI `number` and stops (B .).
// `data` is placed at ROM offset 0x100 (address 0x08000100).
struct SwiCall {
    std::uint8_t number = 0;
    std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    std::vector<std::uint8_t> data;

    [[nodiscard]] std::unique_ptr<Gba> run(std::uint64_t cycles = 200'000) const {
        std::vector<std::uint8_t> rom(0x400, 0);
        const auto put32 = [&](std::uint32_t at, std::uint32_t w) {
            for (std::uint32_t b = 0; b < 4; ++b) rom[at + b] = static_cast<std::uint8_t>(w >> (8 * b));
        };
        // LDR rN, [pc, #0x38]: each instruction reads the literal 0x40 bytes after itself.
        for (std::uint32_t n = 0; n < 4; ++n) put32(n * 4, 0xE59F0038 | (n << 12));
        put32(0x10, 0xEF000000 | (std::uint32_t{number} << 16));  // SWI number
        put32(0x14, 0xEAFFFFFE);                                   // B .
        put32(0x40, r0);
        put32(0x44, r1);
        put32(0x48, r2);
        put32(0x4C, r3);
        std::copy(data.begin(), data.end(), rom.begin() + kData);
        auto gba = Gba::create(rom);
        gba->run_until(cycles);
        return gba;
    }
};

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int v : values) out.push_back(static_cast<std::uint8_t>(v));
    return out;
}

// Header word: type in bits 4-7 (plus a sub-type in bits 0-3), decompressed size above.
std::vector<std::uint8_t> with_header(std::uint32_t type_bits, std::uint32_t size, std::vector<std::uint8_t> body) {
    const std::uint32_t header = type_bits | (size << 8);
    std::vector<std::uint8_t> out = {static_cast<std::uint8_t>(header), static_cast<std::uint8_t>(header >> 8),
                                     static_cast<std::uint8_t>(header >> 16), static_cast<std::uint8_t>(header >> 24)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::uint32_t angle_distance(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t d = (a - b) & 0xFFFF;
    return std::min(d, 0x10000 - d);
}

}  // namespace

TEST_CASE("SWI LZ77UnComp: literals and a back-reference, to WRAM and to VRAM") {
    // "ABC" + a 9-byte copy from 3 bytes back = "ABCABCABCABC".
    const auto data = with_header(0x10, 12, bytes({0x10, 'A', 'B', 'C', 0x60, 0x02}));
    const auto wram = SwiCall{0x11, kRom + kData, kEwram, 0, 0, data}.run();
    for (std::uint32_t i = 0; i < 12; ++i) CHECK(wram->bus().read8(kEwram + i) == "ABC"[i % 3]);
    CHECK(wram->bus().read8(kEwram + 12) == 0);

    const auto vram = SwiCall{0x12, kRom + kData, 0x06000000, 0, 0, data}.run();
    CHECK(vram->bus().read16(0x06000000) == ('B' << 8 | 'A'));  // halfword writes: no doubled bytes
    CHECK(vram->bus().read16(0x06000002) == ('A' << 8 | 'C'));
    CHECK(vram->bus().read16(0x0600000A) == ('C' << 8 | 'B'));
}

TEST_CASE("SWI RLUnComp: runs and literals") {
    // 5 x 'A', then the literals "XYZ".
    const auto data = with_header(0x30, 8, bytes({0x82, 'A', 0x02, 'X', 'Y', 'Z'}));
    const auto gba = SwiCall{0x14, kRom + kData, kEwram, 0, 0, data}.run();
    const char* expected = "AAAAAXYZ";
    for (std::uint32_t i = 0; i < 8; ++i) CHECK(gba->bus().read8(kEwram + i) == expected[i]);
}

TEST_CASE("SWI HuffUnComp: 8-bit symbols through a one-node tree") {
    // Tree: size byte 1 (4 bytes), root 0xC0 (both children are leaves) -> 'A', 'B'.
    // Bits 0 1 1 0 = "ABBA".
    const auto data = with_header(0x28, 4, bytes({0x01, 0xC0, 'A', 'B', 0x00, 0x00, 0x00, 0x60}));
    const auto gba = SwiCall{0x13, kRom + kData, kEwram, 0, 0, data}.run();
    CHECK(gba->bus().read32(kEwram) == 0x41424241);  // 'A' 'B' 'B' 'A'
}

TEST_CASE("SWI BitUnPack: 2-bit units to bytes, offset added to non-zero units") {
    // Source byte 0b10'11'01'00 -> units 0, 1, 3, 2 (LSB first) -> 0, 0x11, 0x13, 0x12.
    std::vector<std::uint8_t> data(0x20, 0);
    data[0] = 0xB4;
    const std::vector<std::uint8_t> info = bytes({1, 0, 2, 8, 0x10, 0, 0, 0});  // len, widths, offset
    std::copy(info.begin(), info.end(), data.begin() + 0x10);
    const auto gba = SwiCall{0x10, kRom + kData, kEwram, kRom + kData + 0x10, 0, data}.run();
    CHECK(gba->bus().read32(kEwram) == 0x12131100);
}

TEST_CASE("SWI Diff8bitUnFilter and Diff16bitUnFilter") {
    const auto d8 = SwiCall{0x16, kRom + kData, kEwram, 0, 0, with_header(0x81, 4, bytes({0x10, 0x01, 0x01, 0xFE}))}.run();
    CHECK(d8->bus().read32(kEwram) == 0x10121110);
    const auto d16 = SwiCall{0x18, kRom + kData, kEwram, 0, 0, with_header(0x82, 4, bytes({0x00, 0x10, 0x05, 0x00}))}.run();
    CHECK(d16->bus().read16(kEwram) == 0x1000);
    CHECK(d16->bus().read16(kEwram + 2) == 0x1005);
}

TEST_CASE("SWI ObjAffineSet: identity and a 90 degree turn, with a stride") {
    // Two sources: scale 1.0, angle 0; scale 1.0, angle 0x4000 (90 degrees).
    const auto data = bytes({0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0, 0,  //
                             0x00, 0x01, 0x00, 0x01, 0x00, 0x40, 0, 0});
    const auto gba = SwiCall{0x0F, kRom + kData, kEwram, 2, 8, data}.run();  // OAM-style stride 8
    const auto at = [&](std::uint32_t i) { return gba->bus().read16(kEwram + i * 8); };
    CHECK(at(0) == 0x0100);  // PA
    CHECK(at(1) == 0x0000);  // PB
    CHECK(at(2) == 0x0000);  // PC
    CHECK(at(3) == 0x0100);  // PD
    CHECK(at(4) == 0x0000);
    CHECK(at(5) == 0xFF00);  // -1.0: PB = -sin * scale
    CHECK(at(6) == 0x0100);
    CHECK(at(7) == 0x0000);
}

TEST_CASE("SWI BgAffineSet: the reference point puts the texture origin at the screen centre") {
    // Origin (64, 32) in 8.8, screen centre (120, 80), scale 1.0, angle 0.
    const auto data = bytes({0x00, 0x40, 0, 0, 0x00, 0x20, 0, 0, 120, 0, 80, 0, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 0});
    const auto gba = SwiCall{0x0E, kRom + kData, kEwram, 1, 0, data}.run();
    CHECK(gba->bus().read16(kEwram) == 0x0100);
    CHECK(gba->bus().read16(kEwram + 6) == 0x0100);
    CHECK(gba->bus().read32(kEwram + 8) == static_cast<std::uint32_t>((64 - 120) * 256));
    CHECK(gba->bus().read32(kEwram + 12) == static_cast<std::uint32_t>((32 - 80) * 256));
}

TEST_CASE("SWI ArcTan2: the four axes and the diagonals") {
    const auto angle = [](std::int16_t x, std::int16_t y) {
        return SwiCall{0x0A, static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)}.run(2000)->cpu().reg(0);
    };
    CHECK(angle(0x100, 0) == 0x0000);
    CHECK(angle(0, 0x100) == 0x4000);
    CHECK(angle(-0x100, 0) == 0x8000);
    CHECK(angle(0, -0x100) == 0xC000);
    CHECK(angle_distance(angle(0x100, 0x100), 0x2000) <= 0x10);
    CHECK(angle_distance(angle(-0x100, 0x100), 0x6000) <= 0x10);
    CHECK(angle_distance(angle(-0x100, -0x100), 0xA000) <= 0x10);
    CHECK(angle_distance(angle(0x100, -0x100), 0xE000) <= 0x10);
    // Somewhere in between: atan2(1, 2) = 26.565 degrees = 0x12E4.
    CHECK(angle_distance(angle(0x200, 0x100), 0x12E4) <= 0x10);
    // All the way around, against std::atan2 (0x10000 = a full turn).
    for (int step = 0; step < 48; ++step) {
        const double theta = (step + 0.3) * 2.0 * 3.14159265358979 / 48.0;
        const auto x = static_cast<std::int16_t>(std::lround(std::cos(theta) * 3000));
        const auto y = static_cast<std::int16_t>(std::lround(std::sin(theta) * 3000));
        const auto expected = static_cast<std::uint32_t>(std::lround(theta / (2.0 * 3.14159265358979) * 65536.0)) & 0xFFFF;
        CAPTURE(step);
        CHECK(angle_distance(angle(x, y), expected) <= 0x20);
    }
}

TEST_CASE("SWI GetBiosChecksum and MidiKey2Freq") {
    CHECK(SwiCall{0x0D}.run(2000)->cpu().reg(0) == 0xBAAE187F);
    // WaveData+4 holds the sample's frequency (here 0x100000 << 10 style values are fine).
    const auto wave = bytes({0, 0, 0, 0, 0x00, 0x00, 0x10, 0x00});  // freq = 0x100000
    CHECK(SwiCall{0x1F, kRom + kData, 180, 0, 0, wave}.run(2000)->cpu().reg(0) == 0x100000);
    CHECK(SwiCall{0x1F, kRom + kData, 168, 0, 0, wave}.run(2000)->cpu().reg(0) == 0x080000);  // one octave down
}

TEST_CASE("SWI RegisterRamReset clears what the flags pick, and blanks the display") {
    auto gba = SwiCall{0x01, 0x05}.run(0);  // EWRAM + palette
    gba->bus().write32(kEwram + 0x100, 0x12345678);
    gba->bus().write32(0x05000010, 0x7FFF7FFF);
    gba->bus().write32(0x06000010, 0x11112222);  // VRAM: not picked
    gba->run_until(400'000);
    CHECK(gba->bus().read32(kEwram + 0x100) == 0);
    CHECK(gba->bus().read32(0x05000010) == 0);
    CHECK(gba->bus().read32(0x06000010) == 0x11112222);
    CHECK(gba->bus().read16(0x04000000) == 0x0080);
}

TEST_CASE("SWI SoftReset restarts the cartridge with fresh registers") {
    // Counts passes in EWRAM; the first pass sets r6 and calls SoftReset.
    std::vector<std::uint8_t> rom(0x400, 0);
    const std::uint32_t code[] = {
        0xE3A01402,  // MOV   r1, #0x02000000
        0xE5D10000,  // LDRB  r0, [r1]
        0xE2800001,  // ADD   r0, r0, #1
        0xE5C10000,  // STRB  r0, [r1]
        0xE3500001,  // CMP   r0, #1
        0x03A06066,  // MOVEQ r6, #0x66
        0x0F000000,  // SWIEQ 0x00 (SoftReset)
        0xEAFFFFFE,  // B .
    };
    for (std::size_t i = 0; i < std::size(code); ++i) {
        for (std::size_t b = 0; b < 4; ++b) rom[i * 4 + b] = static_cast<std::uint8_t>(code[i] >> (8 * b));
    }
    auto gba = Gba::create(rom);
    gba->bus().write32(0x03007E10, 0xDEADBEEF);  // in the area SoftReset clears
    gba->run_until(5000);
    CHECK(gba->bus().read8(kEwram) == 2);  // ran twice
    CHECK(gba->cpu().reg(6) == 0);         // cleared by the reset
    CHECK(gba->cpu().reg(13) == 0x03007F00);
    CHECK(gba->cpu().mode() == Mode::System);
    CHECK(gba->bus().read32(0x03007E10) == 0);
}
