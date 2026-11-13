#include <doctest/doctest.h>
#include "n64/rdp_rasterizer.hpp"

namespace {
class Combiner : public ezy::n64::RdpRasterizer {
public:
    using RdpRasterizer::Rgba;
    using Selectors = std::array<unsigned, 8>;
    static constexpr Rgba texel0{11, 29, 47, 63}, texel1{73, 89, 101, 127};
    static constexpr Rgba next{19, 31, 43, 59}, shade{137, 149, 163, 179};
    static constexpr Rgba primitive{193, 199, 211, 223}, environment{227, 233, 239, 251};
    Rgba run(const Selectors& selectors, bool two_cycles = false) {
        // Set up through real RDP commands. Cycle 0 emits ENV for COMBINED tests.
        constexpr std::array<unsigned, 8> shifts0{52, 28, 47, 15, 44, 12, 41, 9};
        constexpr std::array<unsigned, 8> shifts1{37, 24, 32, 6, 21, 3, 18, 0};
        const Selectors first{15, 15, 31, 5, 7, 7, 7, 5};
        std::uint64_t word = 0x3C00000000000000ULL;
        for (unsigned i = 0; i < selectors.size(); ++i)
            word |= (std::uint64_t{first[i]} << shifts0[i]) | (std::uint64_t{selectors[i]} << shifts1[i]);
        execute(word, {});
        execute(0x2F00000000000000ULL | (std::uint64_t{two_cycles} << 52), {});
        execute(0x3A00001BC1C7D3DFULL, {}); // PRIM_LOD_FRAC = 27
        execute(0x3B000000E3E9EFFBULL, {});
        execute(0x2B00000000000711ULL, {}); // R key center = 7, scale = 17
        execute(0x2A0000000D251339ULL, {}); // G/B centers = 13/19, scales = 37/57
        execute(0x2C00000000006671ULL, {}); // K4 = 51, K5 = 113
        noise_state_ = 1;
        return combine(texel0, texel1, shade, 85, &next);
    }
    [[nodiscard]] std::uint32_t noise_state() const { return noise_state_; }
    void command(std::uint64_t word) { execute(word, {}); }
    [[nodiscard]] Rgba pixel() const { return combine(texel0, texel1, shade, 85, &next); }
};
constexpr Combiner::Selectors zero{15, 15, 31, 7, 7, 7, 7, 7};
constexpr unsigned scaled(unsigned value) { return (255 * value + 128) >> 8; }
}

TEST_CASE("RDP combiner selects all RGB and alpha addends with two-cycle texel routing") {
    Combiner combiner;
    using Rgba = Combiner::Rgba;
    const std::array<Rgba, 8> single{Rgba{}, Combiner::texel0, Combiner::texel1,
        Combiner::primitive, Combiner::shade, Combiner::environment, Rgba{255,255,255,255}, Rgba{}};
    const std::array<Rgba, 8> dual{Combiner::environment, Combiner::texel1, Combiner::next,
        Combiner::primitive, Combiner::shade, Combiner::environment, Rgba{255,255,255,255}, Rgba{}};
    for (bool two : {false, true}) for (unsigned source = 0; source < 8; ++source) {
        CAPTURE(two); CAPTURE(source);
        auto selectors = zero;
        selectors[3] = selectors[7] = source;
        CHECK(combiner.run(selectors, two) == (two ? dual : single)[source]);
        CHECK(combiner.noise_state() == 0x41C67EA6U); // Even unused NOISE advances once per pixel.
    }
}

TEST_CASE("RDP combiner bounds special RGB selectors and preserves alpha sources") {
    Combiner combiner;
    using Rgba = Combiner::Rgba;
    const std::array<Rgba, 8> inputs{Rgba{}, Combiner::texel0, Combiner::texel1,
        Combiner::primitive, Combiner::shade, Combiner::environment, Rgba{255,255,255,255}, Rgba{}};
    for (unsigned slot = 0; slot < 3; ++slot) {
        const unsigned limit = slot == 2 ? 32 : 16;
        for (unsigned source = 0; source < limit; ++source) {
            CAPTURE(slot); CAPTURE(source);
            auto selectors = zero;
            selectors[0] = 6; selectors[2] = 13; // A=ONE, C=LOD(85)
            selectors[slot] = source;
            const auto out = combiner.run(selectors);
            for (unsigned channel = 0; channel < 3; ++channel) {
                unsigned expected = 0;
                if (slot == 0) {
                    // NOISE's first signed sample is negative, hence clamped to zero here.
                    const unsigned a = source < 7 ? inputs[source][channel] : 0;
                    expected = (a * 85 + 128) >> 8;
                } else if (slot == 1) {
                    const unsigned b = source < 6 ? inputs[source][channel]
                        : source == 6 ? std::array{7U,13U,19U}[channel] : source == 7 ? 51 : 0;
                    expected = ((255 - b) * 85 + 128) >> 8;
                } else {
                    const unsigned c = source < 6 ? inputs[source][channel]
                        : source == 6 ? std::array{17U,37U,57U}[channel]
                        : source <= 12 ? inputs[source - 7][3]
                        : source == 13 ? 85 : source == 14 ? 27 : source == 15 ? 113 : 0;
                    expected = scaled(c);
                }
                CHECK(out[channel] == expected);
            }
        }
    }
    for (unsigned slot : {4U,5U,6U}) for (unsigned source = 0; source < 8; ++source) {
        auto selectors = zero;
        selectors[4] = 6; selectors[6] = 0; // alpha A=ONE, C=LOD(85)
        selectors[slot] = source;
        const unsigned value = slot == 6 ? (source == 0 ? 85 : source == 6 ? 27 : inputs[source][3]) : inputs[source][3];
        const auto expected = slot == 4 ? (value * 85 + 128) >> 8
            : slot == 5 ? ((255 - value) * 85 + 128) >> 8 : scaled(value);
        CAPTURE(slot); CAPTURE(source);
        CHECK(combiner.run(selectors)[3] == expected);
    }
}

TEST_CASE("RDP zero combiner products preserve D and advance noise in both cycles") {
    Combiner combiner;
    for (bool two : {false, true}) {
        for (unsigned source = 0; source < 6; ++source) {
            auto selectors = zero;
            selectors[0] = selectors[1] = source;
            selectors[2] = 13; // Nonzero LOD multiplier, so A-B must cancel exactly.
            selectors[3] = selectors[7] = 5;
            selectors[4] = selectors[5] = source;
            selectors[6] = 6; // Nonzero primitive LOD multiplier.
            CAPTURE(two); CAPTURE(source);
            CHECK(combiner.run(selectors, two) == Combiner::environment);
            CHECK(combiner.noise_state() == 0x41C67EA6U);
        }
        auto selectors = zero;
        selectors[0] = 8; selectors[1] = 9; // Different reserved selectors both read zero.
        selectors[2] = 4; selectors[3] = selectors[7] = 3;
        CHECK(combiner.run(selectors, two) == Combiner::primitive);
    }
}

TEST_CASE("RDP combiner uses successive color and combine commands immediately") {
    Combiner combiner;
    auto selectors = zero;
    selectors[3] = selectors[7] = 5;  // ENV
    CHECK(combiner.run(selectors) == Combiner::environment);
    combiner.command(0x3B00000010203040ULL);
    const Combiner::Rgba new_environment{16, 32, 48, 64};
    CHECK(combiner.pixel() == new_environment);

    selectors[3] = selectors[7] = 3;  // PRIMITIVE
    CHECK(combiner.run(selectors) == Combiner::primitive);
    combiner.command(0x3A00000050607080ULL);
    const Combiner::Rgba new_primitive{80, 96, 112, 128};
    CHECK(combiner.pixel() == new_primitive);

    // Switching the selector while keeping the colors must immediately change the result.
    combiner.command(0x3C00000000000000ULL);
    const auto changed = combiner.pixel();
    CHECK(changed[0] == 0);
    CHECK(changed[3] == 0);  // The previous PRIMITIVE D selector no longer applies.
}

TEST_CASE("RDP combiner uses updated key, conversion and primitive LOD uniforms") {
    Combiner combiner;
    auto selectors = zero;
    selectors[0] = 6; selectors[1] = 6; selectors[2] = 13;  // (ONE - KEY_CENTER) * LOD
    CHECK(combiner.run(selectors)[0] == ((255 - 7) * 85 + 128) >> 8);
    combiner.command(0x2B00000000006411ULL);  // R center 100
    CHECK(combiner.pixel()[0] == ((255 - 100) * 85 + 128) >> 8);

    selectors[1] = 7; selectors[2] = 15;  // (ONE - K4) * K5
    CHECK(combiner.run(selectors)[0] == ((255 - 51) * 113 + 128) >> 8);
    combiner.command(0x2C00000000000080ULL);  // K4=0, K5=128
    CHECK(combiner.pixel()[0] == 128);

    selectors[1] = 8; selectors[2] = 14;  // ONE * primitive LOD fraction
    CHECK(combiner.run(selectors)[0] == ((255 * 27 + 128) >> 8));
    combiner.command(0x3A000080C1C7D3DFULL);  // primitive LOD fraction 128
    CHECK(combiner.pixel()[0] == 128);
}
