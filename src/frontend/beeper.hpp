#pragma once

#include <SDL3/SDL.h>

#include <array>

namespace ezy::frontend {

// Square-wave beeper. Each frame we top up the SDL audio stream with just enough
// samples to stay ~2 frames ahead, so the beep starts and stops with low latency.
class Beeper {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr float kFrequency = 440.0F;
    static constexpr float kVolume = 0.15F;

    Beeper() = default;
    Beeper(const Beeper&) = delete;
    Beeper& operator=(const Beeper&) = delete;

    ~Beeper() {
        if (stream_) SDL_DestroyAudioStream(stream_);
    }

    bool open() {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kSampleRate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_) {
            return false;
        }
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    void update(bool active) {
        if (!stream_) return;
        if (!active) {
            SDL_ClearAudioStream(stream_);  // stop immediately
            return;
        }
        constexpr int kSamplesPerFrame = kSampleRate / 60;
        constexpr int kTargetQueued = kSamplesPerFrame * 2 * static_cast<int>(sizeof(float));
        while (SDL_GetAudioStreamQueued(stream_) < kTargetQueued) {
            std::array<float, kSamplesPerFrame> samples{};
            for (auto& s : samples) {
                s = phase_ < 0.5F ? kVolume : -kVolume;
                phase_ += kFrequency / kSampleRate;
                if (phase_ >= 1.0F) phase_ -= 1.0F;
            }
            SDL_PutAudioStreamData(stream_, samples.data(), static_cast<int>(sizeof(samples)));
        }
    }

private:
    SDL_AudioStream* stream_ = nullptr;
    float phase_ = 0.0F;
};

}  // namespace ezy::frontend
