#include <doctest/doctest.h>
#include "n64/rdp_rasterizer.hpp"

namespace {
class ConstantRectangle : public ezy::n64::RdpRasterizer {
public:
    using RdpRasterizer::Rgba;
    void configure(std::uint64_t modes, unsigned cycle, unsigned size, unsigned field,
                   std::uint32_t blend, std::uint32_t fog) {
        solid_.modes = modes;
        solid_.blend = blend;
        solid_.fog = fog;
        cycle_type_ = cycle;
        pixel_size_ = size;
        color_width_ = 7;
        color_address_ = 3;
        scissor_ = field == 0 ? 0 : (1ULL << 25) | (std::uint64_t{field - 1} << 24);
        noise_state_ = 1;
    }
    bool fast(const Rgba& color, std::span<std::uint8_t> memory, unsigned left = 4) const {
        return try_constant_rectangle(left, 0, 28, 12, color, memory);
    }
    void scalar(const Rgba& color, std::span<std::uint8_t> memory) const {
        for (unsigned y = 0; y < 3; ++y) {
            if (!field_matches(y)) continue;
            for (unsigned x = 1; x < 7; ++x) write_opaque_pixel(x, y, color, Rgba{}, 8, memory);
        }
    }
    [[nodiscard]] auto noise() const { return noise_state_; }
};
}

TEST_CASE("RDP constant rectangles match scalar pixels across blend, alpha, coverage and memory bounds") {
    ConstantRectangle fast, scalar;
    std::uint32_t seed = 0xC057A47;
    const auto next = [&] { seed = seed * 1664525U + 1013904223U; return seed; };
    constexpr std::array<std::size_t, 7> lengths{0, 1, 7, 31, 57, 80, 128};
    for (unsigned trial = 0; trial < 512; ++trial) {
        CAPTURE(trial);
        std::uint64_t modes = next();
        modes &= ~((1ULL << 4) | (1ULL << 5) | (1ULL << 6));
        modes |= 3ULL << 38;
        if ((modes & 3) == 3) modes &= ~2ULL; // fixed alpha threshold is deterministic
        const auto blend = next(), fog = next();
        const unsigned size = trial & 1 ? 2 : 3;
        fast.configure(modes, (trial >> 1) & 1, size, trial % 3, blend, fog);
        scalar.configure(modes, (trial >> 1) & 1, size, trial % 3, blend, fog);
        const ConstantRectangle::Rgba color{next() >> 24, next() >> 24, next() >> 24, next() >> 24};
        std::array<std::uint8_t, 128> actual{};
        for (auto& byte : actual) byte = static_cast<std::uint8_t>(next() >> 24);
        auto expected = actual;
        const auto length = lengths[trial % lengths.size()];
        REQUIRE(fast.fast(color, std::span(actual).first(length)));
        scalar.scalar(color, std::span(expected).first(length));
        CHECK(actual == expected);
        CHECK(fast.noise() == scalar.noise());
    }
}

TEST_CASE("RDP constant rectangles defer memory, depth, dither, random alpha and fractional edges") {
    ConstantRectangle rectangle;
    constexpr std::uint64_t no_dither = 3ULL << 38;
    const std::array<std::uint64_t, 6> modes{no_dither | 64, no_dither | 16, no_dither | 32,
                                           0, 1ULL << 38, no_dither | 3};
    std::array<std::uint8_t, 128> memory{};
    memory.fill(0xA5);
    const auto unchanged = memory;
    for (const auto mode : modes) {
        rectangle.configure(mode, 0, 2, 0, 0, 0);
        CHECK_FALSE(rectangle.fast({17, 33, 65, 129}, memory));
        CHECK(memory == unchanged);
        CHECK(rectangle.noise() == 1);
    }
    rectangle.configure(no_dither, 0, 2, 0, 0, 0);
    CHECK_FALSE(rectangle.fast({17, 33, 65, 129}, memory, 1));
    CHECK(memory == unchanged);
}
