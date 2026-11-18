#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace ezy::frontend {

// Pitch-preserving time stretching of interleaved stereo float audio (WSOLA: waveform
// similarity overlap-add). Output comes in hops of kHop frames, each the overlap-add of two
// Hann-windowed kFrame-long input segments; the next segment is taken around where the
// stretch ratio says the input should be, shifted by up to kSearch frames to where it best
// continues the previous one, so periodic sounds keep their phase instead of clicking.
//
// With ratio 1 the chosen segments are contiguous and, after a first hop that fades in, the
// output is the input sample for sample (Hann windows at half overlap add up to exactly 1).
class TimeStretcher {
public:
    static constexpr std::size_t kChannels = 2;
    static constexpr std::size_t kFrame = 1024;      // frames per segment (~23 ms at 44.1 kHz)
    static constexpr std::size_t kHop = kFrame / 2;  // output frames per segment
    static constexpr std::size_t kSearch = 256;      // largest shift, either way

    TimeStretcher() {
        for (std::size_t i = 0; i < kFrame; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / kFrame));
        }
        reset();
    }

    void reset() {
        input_.clear();
        tail_.assign(kHop * kChannels, 0.0F);
        base_ = 0;
        position_ = static_cast<double>(kSearch);  // room to search backwards from the start
        previous_ = -1;
        input_.assign(kSearch * kChannels, 0.0F);   // ... over silence
    }

    // Appends input and writes every whole output hop it allows to `out`. `ratio` is output
    // length over input length (1.5 plays 2 s of input in 3 s); used from now on.
    void process(std::span<const float> interleaved, double ratio, std::vector<float>& out) {
        input_.insert(input_.end(), interleaved.begin(), interleaved.end());
        const double advance = static_cast<double>(kHop) / std::max(ratio, 0.01);
        for (;;) {
            const auto center = static_cast<std::ptrdiff_t>(std::llround(position_)) - base_;
            // The natural continuation of the previous segment, which the shifts are scored against.
            const std::ptrdiff_t natural = previous_ < 0 ? center : previous_ + static_cast<std::ptrdiff_t>(kHop) - base_;
            const std::ptrdiff_t needed = std::max(center + static_cast<std::ptrdiff_t>(kSearch), natural) +
                                          static_cast<std::ptrdiff_t>(kFrame);
            if (needed > static_cast<std::ptrdiff_t>(frames())) break;
            const std::ptrdiff_t start = best_start(center, natural);
            emit(start, out);
            previous_ = start + base_;
            position_ += advance;
            discard_consumed();
        }
    }

    // Input frames waiting to be used (for latency bookkeeping).
    [[nodiscard]] std::size_t pending_frames() const {
        const double used = position_ - static_cast<double>(base_);
        return frames() > used ? frames() - static_cast<std::size_t>(used) : 0;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr std::size_t kStride = 2;  // frames skipped while scoring (speed)

    [[nodiscard]] std::size_t frames() const { return input_.size() / kChannels; }
    [[nodiscard]] float mono(std::ptrdiff_t frame) const {
        const auto at = static_cast<std::size_t>(frame) * kChannels;
        return input_[at] + input_[at + 1];
    }

    // Normalized cross-correlation of the segment at `candidate` with the one at `natural`.
    [[nodiscard]] double score(std::ptrdiff_t candidate, std::ptrdiff_t natural) const {
        double dot = 0.0, energy = 0.0;
        for (std::size_t i = 0; i < kFrame; i += kStride) {
            const double c = mono(candidate + static_cast<std::ptrdiff_t>(i));
            dot += c * mono(natural + static_cast<std::ptrdiff_t>(i));
            energy += c * c;
        }
        return energy > 1e-12 ? dot / std::sqrt(energy) : 0.0;
    }

    // The segment start within kSearch of center that best continues the previous segment.
    // The natural continuation wins ties, which keeps ratio 1 (and silence) exact.
    [[nodiscard]] std::ptrdiff_t best_start(std::ptrdiff_t center, std::ptrdiff_t natural) const {
        if (previous_ < 0) return center;
        const auto search = static_cast<std::ptrdiff_t>(kSearch);
        const std::ptrdiff_t low = std::max<std::ptrdiff_t>(center - search, 0), high = center + search;
        std::ptrdiff_t best = -1;
        double best_score = 0.0;
        if (natural >= low && natural <= high) best = natural, best_score = score(natural, natural);
        // Coarse pass every other shift, then the neighbours of the best one.
        std::ptrdiff_t coarse = best;
        double coarse_score = best_score;
        for (std::ptrdiff_t s = low; s <= high; s += 2) {
            const double value = score(s, natural);
            if (coarse < 0 || better(value, coarse_score)) coarse = s, coarse_score = value;
        }
        for (std::ptrdiff_t s = std::max(low, coarse - 1); s <= std::min(high, coarse + 1); ++s) {
            const double value = score(s, natural);
            if (better(value, coarse_score)) coarse = s, coarse_score = value;
        }
        if (best < 0 || better(coarse_score, best_score)) best = coarse;
        return best;
    }
    // Strictly better beyond rounding, so equal scores (a shifted period of the same wave)
    // never displace the natural continuation.
    [[nodiscard]] static bool better(double value, double reference) {
        return value > reference + 1e-9 * (std::abs(reference) + 1.0);
    }

    // Overlap-adds the windowed segment at `start` onto the tail and outputs one hop.
    void emit(std::ptrdiff_t start, std::vector<float>& out) {
        const auto first = static_cast<std::size_t>(start) * kChannels;
        for (std::size_t i = 0; i < kHop; ++i) {
            for (std::size_t c = 0; c < kChannels; ++c) {
                out.push_back(tail_[i * kChannels + c] + window_[i] * input_[first + i * kChannels + c]);
                tail_[i * kChannels + c] = window_[i + kHop] * input_[first + (i + kHop) * kChannels + c];
            }
        }
    }

    // Drops input no later segment or search can reach.
    void discard_consumed() {
        const auto reach = static_cast<std::ptrdiff_t>(std::floor(position_)) - static_cast<std::ptrdiff_t>(kSearch) - base_;
        const std::ptrdiff_t keep_from = std::min(reach, previous_ + static_cast<std::ptrdiff_t>(kHop) - base_);
        if (keep_from < static_cast<std::ptrdiff_t>(4 * kFrame)) return;  // erase in batches
        input_.erase(input_.begin(), input_.begin() + keep_from * static_cast<std::ptrdiff_t>(kChannels));
        base_ += keep_from;
    }

    std::vector<float> input_;  // interleaved; frame 0 is absolute frame base_
    std::vector<float> tail_;   // second half of the last windowed segment
    std::vector<float> window_ = std::vector<float>(kFrame);
    std::ptrdiff_t base_ = 0;
    double position_ = 0;          // absolute input frame where the next segment should start
    std::ptrdiff_t previous_ = -1;  // absolute start of the last segment
};

}  // namespace ezy::frontend
