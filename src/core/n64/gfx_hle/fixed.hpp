#pragma once

// The RSP's fixed-point arithmetic, as F3DEX uses it for matrices and vertices.
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "n64/rsp_vector.hpp"  // rsp_reciprocal

namespace ezy::n64 {

// A 4x4 matrix of s15.16 values, row by row. Vectors are rows: v' = v * M.
struct Mtx {
    std::array<std::int32_t, 16> m{};
};

// A GBI Mtx in RDRAM: the 16 integer halves, then the 16 fractional halves.
[[nodiscard]] inline Mtx load_mtx(std::span<const std::uint8_t> rdram, std::uint32_t address) {
    Mtx out;
    if (address + 64 > rdram.size()) return out;
    for (unsigned i = 0; i < 16; ++i) {
        const std::uint32_t whole = (std::uint32_t{rdram[address + i * 2]} << 8) | rdram[address + i * 2 + 1];
        const std::uint32_t frac = (std::uint32_t{rdram[address + 32 + i * 2]} << 8) | rdram[address + 32 + i * 2 + 1];
        out.m[i] = static_cast<std::int32_t>((whole << 16) | frac);
    }
    return out;
}

// One s15.16 x s15.16 product as the four-instruction sequence produces it, in units of
// 2^-16: VMUDL (fraction x fraction, low 16 bits of the result dropped), VMADM and VMADN
// (signed whole x unsigned fraction), VMADH (whole x whole).
[[nodiscard]] constexpr std::int64_t rsp_product(std::int32_t a, std::int32_t b) {
    const std::int64_t ai = a >> 16, bi = b >> 16;
    const std::int64_t af = a & 0xFFFF, bf = b & 0xFFFF;
    return ((af * bf) >> 16) + ai * bf + af * bi + ((ai * bi) * 65536);
}

[[nodiscard]] constexpr std::int32_t clamp_s15_16(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX));
}

[[nodiscard]] inline Mtx mul_mtx(const Mtx& a, const Mtx& b) {
    Mtx out;
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned col = 0; col < 4; ++col) {
            std::int64_t sum = 0;
            for (unsigned k = 0; k < 4; ++k) sum += rsp_product(a.m[row * 4 + k], b.m[k * 4 + col]);
            out.m[row * 4 + col] = clamp_s15_16(sum);
        }
    }
    return out;
}

// (x, y, z, 1) * M with integer coordinates (VMUDN/VMADH): exact, in s15.16.
[[nodiscard]] inline std::array<std::int32_t, 4> transform(const Mtx& m, std::int16_t x, std::int16_t y, std::int16_t z) {
    std::array<std::int32_t, 4> out{};
    for (unsigned col = 0; col < 4; ++col) {
        const std::int64_t sum = std::int64_t{x} * m.m[col] + std::int64_t{y} * m.m[4 + col] +
                                 std::int64_t{z} * m.m[8 + col] + m.m[12 + col];
        out[col] = clamp_s15_16(sum);
    }
    return out;
}

}  // namespace ezy::n64
