#include "gamecube_system.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>

#include "debug_ui.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/pi.hpp"

namespace ezy::frontend {

namespace {

// Controller buttons, in gc::PadState's layout (the SI poll's first word).
constexpr std::uint16_t kStart = 0x1000, kY = 0x0800, kX = 0x0400, kB = 0x0200, kA = 0x0100, kL = 0x0040,
                        kR = 0x0020, kZ = 0x0010, kUp = 0x0008, kDown = 0x0004, kRight = 0x0002, kLeft = 0x0001;

struct KeyBinding {
    SDL_Scancode key;
    std::uint16_t bit;
};

// The keyboard as in the N64 (Z/X the main buttons, Q the Z trigger, Return Start, the arrows
// the D-pad, A/S the shoulder triggers, I/J/K/L the stick, T/F/G/H the C stick), plus C/V for
// the GameCube's X and Y.
constexpr std::array<KeyBinding, 10> kKeymap = {{
    {SDL_SCANCODE_Z, kA},
    {SDL_SCANCODE_X, kB},
    {SDL_SCANCODE_C, kX},
    {SDL_SCANCODE_V, kY},
    {SDL_SCANCODE_Q, kZ},
    {SDL_SCANCODE_RETURN, kStart},
    {SDL_SCANCODE_UP, kUp},
    {SDL_SCANCODE_DOWN, kDown},
    {SDL_SCANCODE_LEFT, kLeft},
    {SDL_SCANCODE_RIGHT, kRight},
}};
// Up, down, left, right.
constexpr std::array<SDL_Scancode, 4> kStickKeys = {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L};
constexpr std::array<SDL_Scancode, 4> kCStickKeys = {SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H};

// The Xbox pad (SDL layout) as the design maps it: A, B, X, Y and Start as named, RB = Z, the
// D-pad; the triggers are the analog L/R (below), the sticks the stick and the C stick.
struct PadBinding {
    SDL_GamepadButton button;
    std::uint16_t bit;
};
constexpr std::array<PadBinding, 10> kPadMap = {{
    {SDL_GAMEPAD_BUTTON_SOUTH, kA},
    {SDL_GAMEPAD_BUTTON_EAST, kB},
    {SDL_GAMEPAD_BUTTON_WEST, kX},
    {SDL_GAMEPAD_BUTTON_NORTH, kY},
    {SDL_GAMEPAD_BUTTON_START, kStart},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kZ},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
}};

// A GameCube stick reaches about +-100 from its centre (128); the triggers run 0..255 and
// click their digital L/R at the end of their travel.
constexpr int kStickRange = 100;
constexpr int kPadDeadZone = 8000;
constexpr int kTriggerClick = 30000;  // SDL trigger value (0..32767) where L/R click

// One axis to the GameCube's raw stick byte. `keys_negative`/`keys_positive`: the keys held;
// the pad wins whenever it is outside its dead zone. `flip`: SDL's Y grows downward.
std::uint8_t stick_byte(std::int16_t pad, bool keys_negative, bool keys_positive, bool flip) {
    int offset = (keys_positive ? kStickRange : 0) - (keys_negative ? kStickRange : 0);
    if (std::abs(pad) >= kPadDeadZone) offset = (flip ? -pad : pad) * kStickRange / 32767;
    return static_cast<std::uint8_t>(128 + std::clamp(offset, -kStickRange, kStickRange));
}

}  // namespace

std::unique_ptr<GameCubeSystem> GameCubeSystem::create(const std::filesystem::path& disc) {
    std::unique_ptr<gc::GameCube> machine;
    try {
        machine = gc::GameCube::create(disc);
    } catch (const std::exception& e) {
        std::cerr << "The GameCube HLE boot failed: " << e.what() << '\n';
        return nullptr;
    }
    if (!machine) return nullptr;
    return std::unique_ptr<GameCubeSystem>(new GameCubeSystem(disc, std::move(machine)));
}

template <typename F>
void GameCubeSystem::guarded(F&& body) {
    if (!stop_reason_.empty()) return;
    try {
        body();
    } catch (const std::exception& e) {
        stop_reason_ = e.what();
        std::cerr << "GameCube stopped after " << gc_->stats().instructions << " instructions: " << stop_reason_ << '\n';
    }
}

void GameCubeSystem::run_frame() {
    guarded([this] { gc_->run_field(); });
}

void GameCubeSystem::step_instruction() {
    guarded([this] { gc_->step(); });
}

void GameCubeSystem::reset() {
    try {
        if (auto machine = gc::GameCube::create(disc_)) {
            gc_ = std::move(machine);
            stop_reason_.clear();
        }
    } catch (const std::exception& e) {
        std::cerr << "The GameCube HLE boot failed: " << e.what() << '\n';
    }
    update_pad();
}

void GameCubeSystem::update_pad() {
    gc::PadState pad;
    pad.buttons = static_cast<std::uint16_t>(key_buttons_ | pad_buttons_);
    pad.stick_x = stick_byte(pad_x_, (stick_keys_ & 4) != 0, (stick_keys_ & 8) != 0, false);
    pad.stick_y = stick_byte(pad_y_, (stick_keys_ & 2) != 0, (stick_keys_ & 1) != 0, true);
    pad.c_x = stick_byte(pad_cx_, (c_keys_ & 4) != 0, (c_keys_ & 8) != 0, false);
    pad.c_y = stick_byte(pad_cy_, (c_keys_ & 2) != 0, (c_keys_ & 1) != 0, true);
    const auto trigger = [](std::int16_t value, bool key) {
        return key ? std::uint8_t{255} : static_cast<std::uint8_t>(std::clamp(int{value}, 0, 32767) * 255 / 32767);
    };
    pad.trigger_l = trigger(pad_l_, key_l_);
    pad.trigger_r = trigger(pad_r_, key_r_);
    if (key_l_ || pad_l_ >= kTriggerClick) pad.buttons |= kL;
    if (key_r_ || pad_r_ >= kTriggerClick) pad.buttons |= kR;
    gc_->set_pad(pad);
}

void GameCubeSystem::handle_key(SDL_Scancode scancode, bool pressed) {
    for (const auto& [key, bit] : kKeymap) {
        if (key == scancode) key_buttons_ = static_cast<std::uint16_t>(pressed ? key_buttons_ | bit : key_buttons_ & ~bit);
    }
    const auto hold = [&](const std::array<SDL_Scancode, 4>& keys, std::uint8_t& held) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] != scancode) continue;
            const auto bit = static_cast<std::uint8_t>(1U << i);
            held = static_cast<std::uint8_t>(pressed ? held | bit : held & ~bit);
        }
    };
    hold(kStickKeys, stick_keys_);
    hold(kCStickKeys, c_keys_);
    if (scancode == SDL_SCANCODE_A) key_l_ = pressed;
    if (scancode == SDL_SCANCODE_S) key_r_ = pressed;
    update_pad();
}

void GameCubeSystem::handle_gamepad_button(SDL_GamepadButton button, bool pressed) {
    for (const auto& [pad_button, bit] : kPadMap) {
        if (pad_button == button) pad_buttons_ = static_cast<std::uint16_t>(pressed ? pad_buttons_ | bit : pad_buttons_ & ~bit);
    }
    update_pad();
}

void GameCubeSystem::handle_gamepad_axis(SDL_GamepadAxis axis, std::int16_t value) {
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX: pad_x_ = value; break;
        case SDL_GAMEPAD_AXIS_LEFTY: pad_y_ = value; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: pad_cx_ = value; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: pad_cy_ = value; break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: pad_l_ = value; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: pad_r_ = value; break;
        default: break;
    }
    update_pad();
}

// The XFB (gc::GameCube::xfb, 640 wide, the frame's height), nearest-scaled to 640x480.
void GameCubeSystem::render(std::span<std::uint32_t> argb_pixels) const {
    const gc::Image img = gc_->xfb();
    if (img.width <= 0 || img.height <= 0) {
        std::fill(argb_pixels.begin(), argb_pixels.end(), 0xFF000000);
        return;
    }
    for (int y = 0; y < kScreenHeight; ++y) {
        const int sy = y * img.height / kScreenHeight;
        for (int x = 0; x < kScreenWidth; ++x) {
            const int sx = x * img.width / kScreenWidth;
            argb_pixels[static_cast<std::size_t>(y * kScreenWidth + x)] =
                img.argb[static_cast<std::size_t>(sy * img.width + sx)];
        }
    }
}

void GameCubeSystem::draw_debug_windows() {
    const gc::Gekko& cpu = gc_->cpu();
    debug_ui::place_window(800, 130, 470, 330);
    ImGui::Begin("Gekko");
    for (int i = 0; i < 32; i += 4) {
        ImGui::Text("r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X", i, cpu.gpr(i), i + 1, cpu.gpr(i + 1), i + 2,
                    cpu.gpr(i + 2), i + 3, cpu.gpr(i + 3));
    }
    ImGui::TextColored(debug_ui::kHighlightPc, "pc %08X", cpu.pc());
    ImGui::Text("lr %08X  ctr %08X  cr %08X", cpu.lr(), cpu.ctr(), cpu.cr());
    ImGui::Text("msr %08X  xer %08X", cpu.msr(), cpu.xer());
    ImGui::End();

    debug_ui::place_window(800, 470, 470, 240);
    ImGui::Begin("GameCube");
    const gc::Stats s = gc_->stats();
    ImGui::Text("fields %llu  instructions %llu", static_cast<unsigned long long>(s.fields),
                static_cast<unsigned long long>(s.instructions));
    ImGui::Text("VI interrupts %llu  DI reads %llu  DSP mails %llu",
                static_cast<unsigned long long>(s.interrupts[gc::Pi::kVi]), static_cast<unsigned long long>(s.di_reads),
                static_cast<unsigned long long>(s.dsp_mails));
    ImGui::Text("FIFO bytes (last field) %llu  primitives %llu", static_cast<unsigned long long>(s.fifo_bytes_last_field),
                static_cast<unsigned long long>(s.gx_primitives_last_field));
    if (!stop_reason_.empty()) ImGui::TextWrapped("Stopped: %s", stop_reason_.c_str());
    ImGui::End();
}

}  // namespace ezy::frontend
