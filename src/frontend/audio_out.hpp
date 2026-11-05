#pragma once

#include <SDL3/SDL.h>

#include <span>

namespace ezy::frontend {

// Stereo float output for systems that generate their own samples (the Game Boy APU).
//
// Sync model: emulation stays locked to video frames and audio follows. Because the two
// clocks never match exactly, the queue is kept bounded: if it grows past kMaxQueued
// (the emulator ran ahead), new samples are dropped; if it runs dry the device just
// plays silence until the next frame refills it. Cheap, and good enough until we need
// audio-driven sync or dynamic rate control.
class AudioOut {
public:
    static constexpr int kChannels = 2;
    static constexpr float kMaxQueuedSeconds = 0.1F;

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

    void push(std::span<const float> interleaved) {
        if (!stream_ || interleaved.empty()) return;
        const int max_bytes =
            static_cast<int>(kMaxQueuedSeconds * static_cast<float>(sample_rate_)) * kChannels * static_cast<int>(sizeof(float));
        if (SDL_GetAudioStreamQueued(stream_) > max_bytes) return;  // running ahead: drop
        SDL_PutAudioStreamData(stream_, interleaved.data(), static_cast<int>(interleaved.size_bytes()));
    }

    void clear() {
        if (stream_) SDL_ClearAudioStream(stream_);
    }

private:
    SDL_AudioStream* stream_ = nullptr;
    int sample_rate_ = 48000;
};

}  // namespace ezy::frontend
