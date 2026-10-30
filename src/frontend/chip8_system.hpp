#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "chip8/chip8.hpp"
#include "system.hpp"

namespace ezy::frontend {

class Chip8System final : public System {
public:
    // Returns nullptr if the ROM doesn't fit in memory.
    [[nodiscard]] static std::unique_ptr<Chip8System> create(std::span<const std::uint8_t> rom);

    [[nodiscard]] std::string_view name() const override { return "CHIP-8"; }
    [[nodiscard]] int screen_width() const override { return chip8::kScreenWidth; }
    [[nodiscard]] int screen_height() const override { return chip8::kScreenHeight; }
    [[nodiscard]] double frame_seconds() const override { return 1.0 / 60.0; }

    void run_frame() override;
    void step_instruction() override { chip8_.step(); }
    void reset() override { chip8_.load_rom(rom_); }  // quirks are kept across resets

    void handle_key(SDL_Scancode scancode, bool pressed) override;
    void render(std::span<std::uint32_t> argb_pixels) const override;
    [[nodiscard]] bool sound_active() const override { return chip8_.sound_active(); }

    void draw_debug_windows() override;

private:
    explicit Chip8System(std::vector<std::uint8_t> rom) : rom_(std::move(rom)) {}

    std::vector<std::uint8_t> rom_;
    chip8::Chip8 chip8_;
    int instructions_per_frame_ = 11;  // ~660 Hz at 60 frames per second
};

}  // namespace ezy::frontend
