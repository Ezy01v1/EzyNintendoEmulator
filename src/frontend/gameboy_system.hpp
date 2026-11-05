#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "gb/gameboy.hpp"
#include "system.hpp"

namespace ezy::frontend {

class GameBoySystem final : public System {
public:
    // Returns nullptr if the ROM header can't be parsed. If the cartridge has a battery,
    // external RAM is loaded from save_path (when it exists) and written back to it.
    [[nodiscard]] static std::unique_ptr<GameBoySystem> create(std::span<const std::uint8_t> rom,
                                                               std::filesystem::path save_path = {});
    ~GameBoySystem() override { flush_save(); }
    GameBoySystem(const GameBoySystem&) = delete;
    GameBoySystem& operator=(const GameBoySystem&) = delete;

    [[nodiscard]] std::string_view name() const override { return "Game Boy"; }
    [[nodiscard]] int screen_width() const override { return gb::kLcdWidth; }
    [[nodiscard]] int screen_height() const override { return gb::kLcdHeight; }
    [[nodiscard]] double frame_seconds() const override { return gb::kDotsPerFrame / 4'194'304.0; }  // ~59.73 Hz

    void run_frame() override;
    void step_instruction() override { gameboy_->step(); }
    void reset() override;

    void handle_key(SDL_Scancode scancode, bool pressed) override;
    void render(std::span<std::uint32_t> argb_pixels) const override;
    [[nodiscard]] bool sound_active() const override { return false; }  // APU comes later

    void draw_debug_windows() override;

private:
    GameBoySystem(std::vector<std::uint8_t> rom, std::unique_ptr<gb::GameBoy> gameboy, std::filesystem::path save_path)
        : rom_(std::move(rom)), gameboy_(std::move(gameboy)), save_path_(std::move(save_path)) {}

    [[nodiscard]] bool persists_save() const;
    void load_save();
    void flush_save();  // writes the .sav if RAM changed since the last write

    std::vector<std::uint8_t> rom_;
    std::unique_ptr<gb::GameBoy> gameboy_;
    std::filesystem::path save_path_;
    int frames_since_flush_ = 0;
};

}  // namespace ezy::frontend
