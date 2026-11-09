#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "n64/n64.hpp"
#include "system.hpp"

namespace ezy::frontend {

class N64System final : public System {
public:
    static constexpr int kCanvasWidth = 640;
    static constexpr int kCanvasHeight = 480;
    static constexpr int kAudioRate = 44100;

    // nullptr if the file isn't an N64 ROM. Save states go next to the ROM (<rom>.ss1).
    [[nodiscard]] static std::unique_ptr<N64System> create(std::span<const std::uint8_t> rom,
                                                           std::filesystem::path rom_path = {});

    [[nodiscard]] std::string_view name() const override { return "Nintendo 64"; }
    [[nodiscard]] int screen_width() const override { return kCanvasWidth; }
    [[nodiscard]] int screen_height() const override { return kCanvasHeight; }
    [[nodiscard]] double frame_seconds() const override;

    void run_frame() override;
    void step_instruction() override { n64_->step(); }
    void reset() override;

    void handle_key(SDL_Scancode scancode, bool pressed) override;
    void handle_gamepad_button(SDL_GamepadButton button, bool pressed) override;
    void handle_gamepad_axis(SDL_GamepadAxis axis, std::int16_t value) override;
    void render(std::span<std::uint32_t> argb_pixels) const override;
    [[nodiscard]] bool sound_active() const override { return false; }
    [[nodiscard]] int audio_sample_rate() const override { return kAudioRate; }
    [[nodiscard]] std::span<const float> audio_samples() const override { return audio_; }
    void clear_audio_samples() override { audio_.clear(); }

    void draw_debug_windows() override;

private:
    N64System(std::vector<std::uint8_t> rom, std::unique_ptr<n64::N64> n64, std::filesystem::path rom_path)
        : rom_(std::move(rom)), n64_(std::move(n64)), rom_path_(std::move(rom_path)) {}

    void update_controller();
    // Moves the AI's samples to audio_, resampled from the AI rate to kAudioRate.
    void collect_audio();
    void save_state();
    void load_state();

    std::vector<std::uint8_t> rom_;
    std::unique_ptr<n64::N64> n64_;
    std::filesystem::path rom_path_;
    std::uint16_t buttons_ = 0;
    std::int8_t stick_x_ = 0, stick_y_ = 0;
    std::uint8_t stick_keys_ = 0;  // up, down, left, right held
    std::uint16_t pad_buttons_ = 0;     // from gamepad buttons
    std::uint16_t pad_axis_buttons_ = 0;  // from triggers and the right stick (Z, R, C buttons)
    std::int16_t pad_x_ = 0, pad_y_ = 0;  // left stick, SDL range
    std::vector<float> audio_;
    double resample_phase_ = 0;
};

}  // namespace ezy::frontend
