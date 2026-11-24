#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "gc/gamecube.hpp"
#include "system.hpp"

namespace ezy::frontend {

// The GameCube in ezyemu (design, "Runner y ezyemu"): a disc image opened by path (it is
// read on demand, never loaded whole), the XFB on a 640x480 screen, controller 1 from the
// keyboard and the gamepad. No sound yet (sub-project 3).
class GameCubeSystem final : public System {
public:
    static constexpr int kScreenWidth = 640;
    static constexpr int kScreenHeight = 480;

    // nullptr (reason on stderr) when the disc cannot be opened or booted.
    [[nodiscard]] static std::unique_ptr<GameCubeSystem> create(const std::filesystem::path& disc);

    [[nodiscard]] std::string_view name() const override { return "GameCube"; }
    [[nodiscard]] int screen_width() const override { return kScreenWidth; }
    [[nodiscard]] int screen_height() const override { return kScreenHeight; }
    [[nodiscard]] double frame_seconds() const override { return 1.001 / 60.0; }  // one NTSC field

    // One field. When emulation stops (an unimplemented instruction, register or disc
    // command), the reason goes to stderr and the debugger window, and the machine stays
    // stopped until a reset.
    void run_frame() override;
    void step_instruction() override;
    void reset() override;

    void handle_key(SDL_Scancode scancode, bool pressed) override;
    void handle_gamepad_button(SDL_GamepadButton button, bool pressed) override;
    void handle_gamepad_axis(SDL_GamepadAxis axis, std::int16_t value) override;
    void render(std::span<std::uint32_t> argb_pixels) const override;
    [[nodiscard]] bool sound_active() const override { return false; }

    void draw_debug_windows() override;

private:
    GameCubeSystem(std::filesystem::path disc, std::unique_ptr<gc::GameCube> gc)
        : disc_(std::move(disc)), gc_(std::move(gc)) {}

    // Runs `body`, catching the machine's errors into stop_reason_.
    template <typename F>
    void guarded(F&& body);
    void update_pad();

    std::filesystem::path disc_;
    std::unique_ptr<gc::GameCube> gc_;
    std::string stop_reason_;  // empty while running
    std::uint16_t key_buttons_ = 0, pad_buttons_ = 0;  // PadState::buttons layout
    std::uint8_t stick_keys_ = 0, c_keys_ = 0;         // up, down, left, right held
    bool key_l_ = false, key_r_ = false;               // A/S: the triggers pressed fully
    std::int16_t pad_x_ = 0, pad_y_ = 0, pad_cx_ = 0, pad_cy_ = 0;  // SDL range
    std::int16_t pad_l_ = 0, pad_r_ = 0;                            // SDL trigger range 0..32767
};

}  // namespace ezy::frontend
