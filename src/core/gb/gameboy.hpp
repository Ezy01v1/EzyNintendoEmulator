#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "gb/bus.hpp"
#include "gb/cpu.hpp"

namespace ezy::gb {

// The whole console: what the frontend talks to (the Game Boy counterpart of Chip8).
class GameBoy {
public:
    // Returns nullptr if the ROM header can't be parsed. Heap-allocated because the CPU
    // keeps a reference to the bus, so the object must never move.
    [[nodiscard]] static std::unique_ptr<GameBoy> create(std::span<const std::uint8_t> rom);

    GameBoy(const GameBoy&) = delete;
    GameBoy& operator=(const GameBoy&) = delete;

    // Runs until the PPU finishes a frame (or one frame's worth of cycles if the LCD is off).
    void run_frame();
    void step() { cpu_.step(); }

    void set_button(Button button, bool pressed) { bus_.set_button(button, pressed); }
    [[nodiscard]] std::span<const std::uint8_t> framebuffer() const { return bus_.ppu().framebuffer(); }

    [[nodiscard]] Cpu& cpu() { return cpu_; }
    [[nodiscard]] const Cpu& cpu() const { return cpu_; }
    [[nodiscard]] Bus& bus() { return bus_; }
    [[nodiscard]] const Bus& bus() const { return bus_; }

private:
    explicit GameBoy(Cartridge cartridge);

    Bus bus_;
    Cpu cpu_;
};

}  // namespace ezy::gb
