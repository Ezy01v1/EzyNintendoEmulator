#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "gba/gba.hpp"
#include "system.hpp"

namespace ezy::frontend {

class GbaSystem final : public System {
public:
    // Returns nullptr if the ROM header can't be parsed. The BIOS is optional.
    [[nodiscard]] static std::unique_ptr<GbaSystem> create(std::span<const std::uint8_t> rom,
                                                           std::span<const std::uint8_t> bios = {});

    [[nodiscard]] std::string_view name() const override { return "Game Boy Advance"; }
    [[nodiscard]] int screen_width() const override { return gba::kScreenWidth; }
    [[nodiscard]] int screen_height() const override { return gba::kScreenHeight; }
    [[nodiscard]] double frame_seconds() const override {
        return static_cast<double>(gba::kCyclesPerFrame) / gba::kCpuHz;  // ~59.73 Hz
    }

    void run_frame() override { gba_->run_frame(); }
    void step_instruction() override { gba_->step(); }
    void reset() override;

    void handle_key(SDL_Scancode scancode, bool pressed) override;
    void render(std::span<std::uint32_t> argb_pixels) const override;
    [[nodiscard]] bool sound_active() const override { return false; }  // GBA sound comes later

    void draw_debug_windows() override;

private:
    GbaSystem(std::vector<std::uint8_t> rom, std::vector<std::uint8_t> bios, std::unique_ptr<gba::Gba> gba)
        : rom_(std::move(rom)), bios_(std::move(bios)), gba_(std::move(gba)) {}

    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> bios_;
    std::unique_ptr<gba::Gba> gba_;
    std::uint16_t keys_ = 0;  // KEYINPUT layout, 1 = pressed
};

}  // namespace ezy::frontend
