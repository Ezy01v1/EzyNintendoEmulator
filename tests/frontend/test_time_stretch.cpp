#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "time_stretch.hpp"

using ezy::frontend::TimeStretcher;

namespace {

constexpr double kRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

// Interleaved stereo: a few tones plus deterministic noise, different on each side.
std::vector<float> test_signal(std::size_t frames) {
    std::vector<float> out;
    std::uint32_t noise = 0x12345678;
    for (std::size_t i = 0; i < frames; ++i) {
        noise = noise * 1664525U + 1013904223U;
        const double t = static_cast<double>(i) / kRate;
        const double n = (static_cast<double>(noise >> 8) / 16777216.0 - 0.5) * 0.1;
        out.push_back(static_cast<float>(0.4 * std::sin(2 * kPi * 440 * t) + 0.2 * std::sin(2 * kPi * 1230 * t) + n));
        out.push_back(static_cast<float>(0.3 * std::sin(2 * kPi * 330 * t + 1.0) - n));
    }
    return out;
}

std::vector<float> sine(std::size_t frames, double hz) {
    std::vector<float> out;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(0.5 * std::sin(2 * kPi * hz * static_cast<double>(i) / kRate));
        out.push_back(v), out.push_back(v);
    }
    return out;
}

// Feeds the input in emulator-sized pieces (one 60 Hz frame's worth).
std::vector<float> stretch(const std::vector<float>& in, double ratio, TimeStretcher& stretcher) {
    std::vector<float> out;
    constexpr std::size_t kPiece = 735 * 2;
    for (std::size_t at = 0; at < in.size(); at += kPiece) {
        const std::size_t n = std::min(kPiece, in.size() - at);
        stretcher.process(std::span(in).subspan(at, n), ratio, out);
    }
    return out;
}

// Frequency of the left channel from its upward zero crossings, skipping the first `skip` frames.
double frequency(const std::vector<float>& interleaved, std::size_t skip) {
    std::vector<double> crossings;
    for (std::size_t i = skip + 1; i < interleaved.size() / 2; ++i) {
        const float a = interleaved[(i - 1) * 2], b = interleaved[i * 2];
        if (a < 0 && b >= 0) crossings.push_back(static_cast<double>(i - 1) + a / (a - b));
    }
    if (crossings.size() < 2) return 0;
    return kRate * static_cast<double>(crossings.size() - 1) / (crossings.back() - crossings.front());
}

}  // namespace

TEST_CASE("Ratio 1 gives back the input sample for sample after the first hop") {
    TimeStretcher stretcher;
    const auto in = test_signal(44100);
    const auto out = stretch(in, 1.0, stretcher);
    REQUIRE(out.size() >= in.size() - 2 * TimeStretcher::kFrame * 2);
    std::size_t mismatches = 0;
    for (std::size_t i = TimeStretcher::kHop * 2; i < out.size(); ++i) {
        if (std::abs(out[i] - in[i]) > 1e-6F) ++mismatches;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("Stretching lengthens the sound by the ratio and keeps its pitch") {
    for (const double ratio : {1.25, 1.5, 2.0}) {
        CAPTURE(ratio);
        TimeStretcher stretcher;
        const auto in = sine(88200, 440.0);
        const auto out = stretch(in, ratio, stretcher);
        const double expected = static_cast<double>(in.size()) * ratio;
        CHECK(static_cast<double>(out.size()) == doctest::Approx(expected).epsilon(0.03));
        CHECK(frequency(out, 4096) == doctest::Approx(440.0).epsilon(0.01));
        // Overlap-add of windows that sum to 1 never exceeds the input's peak.
        float peak = 0;
        for (const float v : out) peak = std::max(peak, std::abs(v));
        CHECK(peak <= 0.5F + 1e-5F);
    }
}

TEST_CASE("Changing the ratio mid-stream and silence stay well-behaved") {
    TimeStretcher stretcher;
    std::vector<float> out;
    const auto tone = sine(44100, 523.25);
    for (std::size_t at = 0; at < tone.size(); at += 1470) {
        const double ratio = 1.0 + 0.6 * static_cast<double>(at) / static_cast<double>(tone.size());
        stretcher.process(std::span(tone).subspan(at, std::min<std::size_t>(1470, tone.size() - at)), ratio, out);
    }
    CHECK(frequency(out, 4096) == doctest::Approx(523.25).epsilon(0.01));
    for (const float v : out) REQUIRE(std::isfinite(v));

    TimeStretcher quiet;
    const std::vector<float> silence(2 * 20000, 0.0F);
    const auto still = stretch(silence, 1.7, quiet);
    CHECK(!still.empty());
    for (const float v : still) REQUIRE(v == 0.0F);
}

TEST_CASE("Reset forgets the queued input") {
    TimeStretcher stretcher;
    std::vector<float> out;
    stretcher.process(test_signal(3000), 1.0, out);
    CHECK(stretcher.pending_frames() > 0);
    stretcher.reset();
    CHECK(stretcher.pending_frames() == 0);  // the leading silence lies behind the read position
}
