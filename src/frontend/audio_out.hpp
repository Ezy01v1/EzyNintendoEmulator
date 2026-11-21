#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "time_stretch.hpp"

namespace ezy::frontend {

// Stereo float output for systems that generate their own samples (Game Boy, GBA, N64).
//
// Sync model: emulation stays locked to video frames and audio follows. When the emulator
// runs slower than real time (an N64 race at 42 of 60 fields per second) it produces too few
// samples, so the sound is time-stretched without changing its pitch instead of being cut
// by silence. The stretch is the inverse of the measured emulation speed, nudged so the
// device's queue stays near kTargetQueuedSeconds: never dry, never far behind the picture.
// At full speed the ratio stays close to 1, where stretching gives the input back unchanged.
class AudioOut {
public:
    static constexpr int kChannels = 2;
    static constexpr double kTargetQueuedSeconds = 0.06;
    static constexpr double kMaxQueuedSeconds = 0.25;  // beyond this (a hitch caught up): drop
    static constexpr double kMaxRatio = 3.0;            // below a third of full speed it just gaps

    AudioOut() = default;
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    ~AudioOut() {
        if (stream_) SDL_DestroyAudioStream(stream_);
    }

    bool open(int sample_rate) {
        sample_rate_ = sample_rate;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, kChannels, sample_rate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_) return false;
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    // Called once per frame of the main loop with what the emulator produced since the last
    // call (possibly nothing).
    void push(std::span<const float> interleaved) {
        if (!stream_) return;
        measure_speed(interleaved.size() / kChannels, SDL_GetTicksNS());
        if (interleaved.empty()) return;
        const double queued = queued_seconds();
        // Proportional correction toward the target queue: up to +50 % when the queue runs
        // dry, down to -50 % when it is long (the speed estimate lags a sudden speed-up, and
        // the queue would otherwise fill to kMaxQueuedSeconds and drop sound).
        const double correction =
            std::clamp(1.0 + 0.5 * (kTargetQueuedSeconds - queued) / kTargetQueuedSeconds, 0.5, 1.5);
        const double ratio = std::clamp(correction / speed_, 0.5, kMaxRatio);
        out_.clear();
        stretcher_.process(interleaved, ratio, out_);
        if (out_.empty() || queued > kMaxQueuedSeconds) return;
        SDL_PutAudioStreamData(stream_, out_.data(), static_cast<int>(out_.size() * sizeof(float)));
    }

    void clear() {
        if (stream_) SDL_ClearAudioStream(stream_);
        stretcher_.reset();
        window_start_ = 0;
        window_frames_ = 0;
        speed_ = 1.0;
    }

    // The emulator's speed as last measured (1 = real time), for display or debugging.
    [[nodiscard]] double speed() const { return speed_; }

private:
    static constexpr std::uint64_t kWindowNs = 250'000'000;  // speed is measured over ~0.25 s
    static constexpr std::uint64_t kHitchNs = 500'000'000;   // a longer gap (pause, load) restarts it

    [[nodiscard]] double queued_seconds() const {
        const int bytes = SDL_GetAudioStreamQueued(stream_);
        return bytes <= 0 ? 0.0 : static_cast<double>(bytes) / (sample_rate_ * kChannels * sizeof(float));
    }

    // Samples produced per second of real time over the window, against the device rate.
    void measure_speed(std::size_t frames, std::uint64_t now) {
        if (window_start_ == 0 || now - last_push_ > kHitchNs) {
            window_start_ = now;
            window_frames_ = 0;
        } else {
            window_frames_ += frames;
            if (const std::uint64_t elapsed = now - window_start_; elapsed >= kWindowNs) {
                const double measured =
                    static_cast<double>(window_frames_) / (static_cast<double>(elapsed) * 1e-9 * sample_rate_);
                speed_ = std::clamp(0.25 * speed_ + 0.75 * measured, 1.0 / kMaxRatio, 1.2);
                window_start_ = now;
                window_frames_ = 0;
            }
        }
        last_push_ = now;
    }

    SDL_AudioStream* stream_ = nullptr;
    int sample_rate_ = 48000;
    TimeStretcher stretcher_;
    std::vector<float> out_;
    std::uint64_t window_start_ = 0, last_push_ = 0;
    std::size_t window_frames_ = 0;
    double speed_ = 1.0;
};

}  // namespace ezy::frontend
