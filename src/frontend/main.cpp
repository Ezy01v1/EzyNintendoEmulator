#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio_out.hpp"
#include "beeper.hpp"
#include "chip8_system.hpp"
#include "debugger.hpp"
#include "gameboy_system.hpp"
#include "gba_system.hpp"
#include "system.hpp"

namespace {

using ezy::frontend::RunState;
using ezy::frontend::System;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr double kMaxCatchUpSeconds = 0.25;  // avoid a "spiral of death" after a stall

std::optional<std::vector<std::uint8_t>> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

// Game Boy options after the ROM path: --model auto|dmg|cgb, --boot-rom <file>,
// --no-color-correction. Returns nullopt (after printing why) on a bad option.
std::optional<ezy::frontend::GameBoySettings> parse_gb_options(int argc, char* argv[]) {
    ezy::frontend::GameBoySettings settings;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) {
            const std::string model = argv[++i];
            if (model == "auto") {
                settings.model = ezy::gb::ModelChoice::Auto;
            } else if (model == "dmg") {
                settings.model = ezy::gb::ModelChoice::Dmg;
            } else if (model == "cgb") {
                settings.model = ezy::gb::ModelChoice::Cgb;
            } else {
                std::cerr << "Unknown model: " << model << " (use auto, dmg or cgb)\n";
                return std::nullopt;
            }
        } else if (arg == "--boot-rom" && i + 1 < argc) {
            settings.boot_rom = read_file(argv[++i]);
            if (!settings.boot_rom) {
                std::cerr << "Could not read boot ROM: " << argv[i] << '\n';
                return std::nullopt;
            }
        } else if (arg == "--no-color-correction") {
            settings.color_correction = false;
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return std::nullopt;
        }
    }
    return settings;
}

// Picks the console from the file extension.
std::unique_ptr<System> create_system(const char* path, std::span<const std::uint8_t> rom,
                                      ezy::frontend::GameBoySettings gb_settings) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".gba") {
        return ezy::frontend::GbaSystem::create(rom, {}, std::filesystem::path(path).replace_extension(".sav"));
    }
    if (ext == ".gb" || ext == ".gbc") {
        return ezy::frontend::GameBoySystem::create(rom, std::filesystem::path(path).replace_extension(".sav"),
                                                    std::move(gb_settings));
    }
    return ezy::frontend::Chip8System::create(rom);
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

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ezyemu <rom.ch8 | rom.gb | rom.gbc | rom.gba> [Game Boy options]\n"
                     "  --model auto|dmg|cgb    hardware to emulate (auto = Game Boy Color)\n"
                     "  --boot-rom <file>       run a real CGB boot ROM dump (2304 bytes)\n"
                     "  --no-color-correction   show raw CGB colours\n";
        return 1;
    }

    auto gb_settings = parse_gb_options(argc, argv);
    if (!gb_settings) return 1;
    const auto rom = read_file(argv[1]);
    std::unique_ptr<System> system = rom ? create_system(argv[1], *rom, std::move(*gb_settings)) : nullptr;
    if (!system) {
        std::cerr << "Could not load ROM: " << argv[1] << '\n';
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    const std::string title = "EzyEmulator - " + std::string(system->name());
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer(title.c_str(), kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE, &window,
                                     &renderer)) {
        std::cerr << "SDL_CreateWindowAndRenderer failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             system->screen_width(), system->screen_height());
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        ezy::frontend::Beeper beeper;
        ezy::frontend::AudioOut audio;
        const bool own_audio = system->audio_sample_rate() > 0;
        if (!(own_audio ? audio.open(system->audio_sample_rate()) : beeper.open())) {
            std::cerr << "Audio unavailable, continuing without sound: " << SDL_GetError() << '\n';
        }

        RunState run;
        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(system->screen_width() * system->screen_height()));

        // Fixed-timestep loop: emulation advances in exact frame-sized steps, no matter how
        // often we render (60 Hz, 144 Hz or an uncapped loop).
        const double frame_seconds = system->frame_seconds();
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
                    system->handle_key(event.key.scancode, down);
                }
            }

            const std::uint64_t now = SDL_GetPerformanceCounter();
            accumulator += static_cast<double>(now - previous) / counter_freq;
            accumulator = std::min(accumulator, kMaxCatchUpSeconds);
            previous = now;

            if (run.reset) {
                system->reset();
                run.reset = false;
            }

            if (run.paused) {
                if (run.step_instruction) system->step_instruction();
                if (run.step_frame) system->run_frame();
                run.step_instruction = run.step_frame = false;
                accumulator = 0.0;
            } else {
                while (accumulator >= frame_seconds) {
                    system->run_frame();
                    accumulator -= frame_seconds;
                }
            }
            if (own_audio) {
                if (run.paused) {
                    audio.clear();
                } else {
                    audio.push(system->audio_samples());
                }
                system->clear_audio_samples();
            } else {
                beeper.update(!run.paused && system->sound_active());
            }

            system->render(pixels);
            SDL_UpdateTexture(texture, nullptr, pixels.data(),
                              system->screen_width() * static_cast<int>(sizeof(std::uint32_t)));

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ezy::frontend::draw_debugger(*system, run, texture);
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
