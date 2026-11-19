#pragma once

// Helpers shared by the N64 tests: a minimal bootable ROM and RDP command encoders.
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "mips_asm.hpp"

namespace n64_test {

// A ROM whose "IPL3" (at 0x40, run from SP DMEM at 0xA4000040) counts in t1 and stores
// the count to RDRAM forever.
inline std::vector<std::uint8_t> counting_rom(const std::string& title) {
    std::vector<std::uint8_t> rom(0x2000, 0);
    rom[0] = 0x80, rom[1] = 0x37, rom[2] = 0x12, rom[3] = 0x40;
    for (std::size_t i = 0; i < title.size() && i < 20; ++i) rom[0x20 + i] = static_cast<std::uint8_t>(title[i]);
    rom[0x3E] = 'E';
    using namespace mips;
    constexpr int t0 = 8, t1 = 9;
    const std::uint32_t program[] = {lui(t0, 0x8000), addiu(t1, t1, 1), sw(t1, t0, 0x100), j(0x02, 0xA4000044), nop()};
    std::size_t at = 0x40;
    for (const std::uint32_t w : program) {
        rom[at] = static_cast<std::uint8_t>(w >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(w >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(w >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(w);
        at += 4;
    }
    return rom;
}

// A ROM whose "IPL3" copies a loop to RDRAM at 0x80001000 and jumps there (kseg0 code in
// direct RAM, what the CPU JIT compiles). t1 counts iterations, t4 += the immediate of the
// loop's first instruction and is stored at 0x80000100; every 4096 iterations the loop
// rewrites that immediate with t1's low half.
inline std::vector<std::uint8_t> rdram_loop_rom(const std::string& title) {
    std::vector<std::uint8_t> rom = counting_rom(title);
    using namespace mips;
    constexpr int zero = 0, t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13;
    const std::uint32_t loop[] = {
        addiu(t4, t4, 0),                // 0x1000: the immediate the SH below rewrites
        addiu(t1, t1, 1),                // 0x1004
        mips::i(0x0C, t1, t5, 0xFFF),    // 0x1008: ANDI t5, t1, 0xFFF
        bne(t5, zero, 2),                // 0x100C: -> 0x1018
        sw(t4, t0, 0x100),               // 0x1010 (delay slot)
        mips::i(0x29, t0, t1, 0x1002),   // 0x1014: SH t1 -> the immediate at 0x1000
        j(0x02, 0x80001000),             // 0x1018
        nop(),
    };
    std::vector<std::uint32_t> program{lui(t0, 0x8000)};
    for (std::size_t k = 0; k < std::size(loop); ++k) {
        program.push_back(lui(t2, static_cast<std::int32_t>(loop[k] >> 16)));
        program.push_back(ori(t2, t2, static_cast<std::int32_t>(loop[k] & 0xFFFF)));
        program.push_back(sw(t2, t0, static_cast<std::int32_t>(0x1000 + 4 * k)));
    }
    program.insert(program.end(), {lui(t3, 0x8000), ori(t3, t3, 0x1000), jr(t3), nop()});
    std::size_t at = 0x40;
    for (const std::uint32_t w : program) {
        rom[at] = static_cast<std::uint8_t>(w >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(w >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(w >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(w);
        at += 4;
    }
    return rom;
}

constexpr std::uint32_t kDpcStart = 0x04100000, kDpcEnd = 0x04100004, kDpcCurrent = 0x04100008,
                        kDpcStatus = 0x0410000C;
constexpr std::uint64_t kFillMode = 0x2F30000000000000ULL, kSyncFull = 0x2900000000000000ULL;
constexpr std::uint64_t kPointMode = (0x2FULL << 56) | (6ULL << 41) | (3ULL << 38);  // G_TC_FILT, RGB dither off
constexpr std::uint64_t kFilterMode = kPointMode | (1ULL << 45);

constexpr std::uint64_t rdp_rectangle(std::uint64_t opcode, unsigned hi_x, unsigned hi_y, unsigned lo_x, unsigned lo_y) {
    // Each coordinate is encoded in quarter pixels.
    return (opcode << 56) | (std::uint64_t{hi_x * 4} << 44) | (std::uint64_t{hi_y * 4} << 32) |
           (std::uint64_t{lo_x * 4} << 12) | (lo_y * 4);
}

}  // namespace n64_test
