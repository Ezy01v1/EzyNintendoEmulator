#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>

#include "chip8/chip8.hpp"

namespace {

constexpr int kScale = 12;
constexpr int kInstructionsPerFrame = 11;  // ~700 Hz at 60 frames per second
constexpr std::uint32_t kColorOn = 0xFFE0E0E0;
constexpr std::uint32_t kColorOff = 0xFF101018;

// CHIP-8 keypad      QWERTY keys (by physical position)
// 1 2 3 C            1 2 3 4
// 4 5 6 D            Q W E R
// 7 8 9 E            A S D F
// A 0 B F            Z X C V
constexpr std::array<SDL_Scancode, 16> kKeymap = {
    SDL_SCANCODE_X, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,  // 0 1 2 3
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_A,  // 4 5 6 7
    SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_C,  // 8 9 A B
    SDL_SCANCODE_4, SDL_SCANCODE_R, SDL_SCANCODE_F, SDL_SCANCODE_V,  // C D E F
};

// Square-wave beeper. Each frame we top up the SDL audio stream with just enough
// samples to stay ~2 frames ahead, so the beep starts and stops with low latency.
class Beeper {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr float kFrequency = 440.0F;
    static constexpr float kVolume = 0.15F;

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

    ~Beeper() {
        if (stream_) SDL_DestroyAudioStream(stream_);
    }

private:
    SDL_AudioStream* stream_ = nullptr;
    float phase_ = 0.0F;
};

std::optional<std::vector<std::uint8_t>> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

void handle_key(ezy::chip8::Chip8& chip8, SDL_Scancode scancode, bool pressed) {
    for (std::size_t key = 0; key < kKeymap.size(); ++key) {
        if (kKeymap[key] == scancode) {
            chip8.set_key(static_cast<std::uint8_t>(key), pressed);
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezyemu <rom.ch8>\n";
        return 1;
    }

    const auto rom = read_file(argv[1]);
    ezy::chip8::Chip8 chip8;
    if (!rom || !chip8.load_rom(*rom)) {
        std::cerr << "Could not load ROM: " << argv[1] << '\n';
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    Beeper beeper;
    if (!beeper.open()) {
        std::cerr << "Audio unavailable, continuing without sound: " << SDL_GetError() << '\n';
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("EzyEmulator - CHIP-8", ezy::chip8::kScreenWidth * kScale,
                                     ezy::chip8::kScreenHeight * kScale, 0, &window, &renderer)) {
        std::cerr << "SDL_CreateWindowAndRenderer failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             ezy::chip8::kScreenWidth, ezy::chip8::kScreenHeight);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    std::array<std::uint32_t, ezy::chip8::kScreenWidth * ezy::chip8::kScreenHeight> pixels{};

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                }
                handle_key(chip8, event.key.scancode, event.type == SDL_EVENT_KEY_DOWN);
            }
        }

        // Simple pacing: one frame per vsync. A proper timing model comes later.
        for (int i = 0; i < kInstructionsPerFrame; ++i) {
            chip8.step();
        }
        chip8.tick_timers();
        beeper.update(chip8.sound_active());

        const auto fb = chip8.framebuffer();
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = fb[i] ? kColorOn : kColorOff;
        }
        SDL_UpdateTexture(texture, nullptr, pixels.data(), ezy::chip8::kScreenWidth * sizeof(std::uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
