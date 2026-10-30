#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>

#include "beeper.hpp"
#include "chip8/chip8.hpp"
#include "debugger.hpp"

namespace {

using ezy::chip8::Chip8;
using ezy::frontend::RunState;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr double kMaxCatchUpSeconds = 0.25;  // avoid a "spiral of death" after a stall
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

std::optional<std::vector<std::uint8_t>> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

void handle_key(Chip8& chip8, SDL_Scancode scancode, bool pressed) {
    for (std::size_t key = 0; key < kKeymap.size(); ++key) {
        if (kKeymap[key] == scancode) {
            chip8.set_key(static_cast<std::uint8_t>(key), pressed);
        }
    }
}

// Debugger shortcuts. Returns false when the app should quit.
bool handle_shortcut(SDL_Scancode scancode, RunState& run) {
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE: return false;
        case SDL_SCANCODE_P: run.paused = !run.paused; break;
        case SDL_SCANCODE_N: run.step_instruction = run.paused; break;
        case SDL_SCANCODE_F5: run.reset = true; break;
        default: break;
    }
    return true;
}

// One emulated 60 Hz frame: a batch of instructions, then the timers.
void run_frame(Chip8& chip8, int instructions) {
    for (int i = 0; i < instructions; ++i) {
        chip8.step();
    }
    chip8.tick_timers();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezyemu <rom.ch8>\n";
        return 1;
    }

    const auto rom = read_file(argv[1]);
    Chip8 chip8;
    if (!rom || !chip8.load_rom(*rom)) {
        std::cerr << "Could not load ROM: " << argv[1] << '\n';
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("EzyEmulator - CHIP-8", kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        std::cerr << "SDL_CreateWindowAndRenderer failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             ezy::chip8::kScreenWidth, ezy::chip8::kScreenHeight);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        ezy::frontend::Beeper beeper;
        if (!beeper.open()) {
            std::cerr << "Audio unavailable, continuing without sound: " << SDL_GetError() << '\n';
        }

        RunState run;
        std::array<std::uint32_t, ezy::chip8::kScreenWidth * ezy::chip8::kScreenHeight> pixels{};

        // Fixed-timestep loop: emulation advances in exact 1/60 s steps, no matter how
        // often we render (60 Hz, 144 Hz or an uncapped loop).
        const double counter_freq = static_cast<double>(SDL_GetPerformanceFrequency());
        std::uint64_t previous = SDL_GetPerformanceCounter();
        double accumulator = 0.0;

        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                    if (ImGui::GetIO().WantTextInput) continue;  // typing into a UI field
                    const bool down = event.type == SDL_EVENT_KEY_DOWN;
                    if (down && !event.key.repeat && !handle_shortcut(event.key.scancode, run)) {
                        running = false;
                    }
                    handle_key(chip8, event.key.scancode, down);
                }
            }

            const std::uint64_t now = SDL_GetPerformanceCounter();
            accumulator += static_cast<double>(now - previous) / counter_freq;
            accumulator = std::min(accumulator, kMaxCatchUpSeconds);
            previous = now;

            if (run.reset) {
                chip8.load_rom(*rom);  // quirks are kept across resets
                run.reset = false;
            }

            if (run.paused) {
                if (run.step_instruction) chip8.step();
                if (run.step_frame) run_frame(chip8, run.instructions_per_frame);
                run.step_instruction = run.step_frame = false;
                accumulator = 0.0;
            } else {
                while (accumulator >= kFrameSeconds) {
                    run_frame(chip8, run.instructions_per_frame);
                    accumulator -= kFrameSeconds;
                }
            }
            beeper.update(!run.paused && chip8.sound_active());

            const auto fb = chip8.framebuffer();
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                pixels[i] = fb[i] ? kColorOn : kColorOff;
            }
            SDL_UpdateTexture(texture, nullptr, pixels.data(), ezy::chip8::kScreenWidth * sizeof(std::uint32_t));

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ezy::frontend::draw_debugger(chip8, run, texture);
            ImGui::Render();

            SDL_SetRenderDrawColor(renderer, 24, 24, 28, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
