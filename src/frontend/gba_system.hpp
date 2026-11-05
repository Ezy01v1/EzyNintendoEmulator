#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "gba/gba.hpp"
#include "system.hpp"

namespace ezy::frontend {

class GbaSystem final : public System {
public:
    // Returns nullptr if the ROM header can't be parsed. The BIOS is optional. The save
    // chip's contents are loaded from save_path (when it exists) and written back to it.
    [[nodiscard]] static std::unique_ptr<GbaSystem> create(std::span<const std::uint8_t> rom,
                                                           std::span<const std::uint8_t> bios = {},
                                                           std::filesystem::path save_path = {});
    ~GbaSystem() override { flush_save(); }
    GbaSystem(const GbaSystem&) = delete;
    GbaSystem& operator=(const GbaSystem&) = delete;

    [[nodiscard]] std::string_view name() const override { return "Game Boy Advance"; }
    [[nodiscard]] int screen_width() const override { return gba::kScreenWidth; }
    [[nodiscard]] int screen_height() const override { return gba::kScreenHeight; }
    [[nodiscard]] double frame_seconds() const override {
        return static_cast<double>(gba::kCyclesPerFrame) / gba::kCpuHz;  // ~59.73 Hz
    }

    void run_frame() override;
    void step_instruction() override { gba_->step(); }
    void reset() override;

    void handle_key(SDL_Scancode scancode, bool pressed) override;
    void render(std::span<std::uint32_t> argb_pixels) const override;
    [[nodiscard]] bool sound_active() const override { return false; }  // GBA sound comes later

    void draw_debug_windows() override;

private:
    GbaSystem(std::vector<std::uint8_t> rom, std::vector<std::uint8_t> bios, std::unique_ptr<gba::Gba> gba,
              std::filesystem::path save_path)
        : rom_(std::move(rom)), bios_(std::move(bios)), gba_(std::move(gba)), save_path_(std::move(save_path)) {}

    void load_save();
    void flush_save();  // writes the .sav if the chip changed since the last write

    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> bios_;
    std::unique_ptr<gba::Gba> gba_;
    std::filesystem::path save_path_;
    int frames_since_flush_ = 0;
    std::uint16_t keys_ = 0;  // KEYINPUT layout, 1 = pressed
};

}  // namespace ezy::frontend
