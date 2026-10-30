#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>

namespace ezy::chip8 {

inline constexpr std::size_t kMemorySize = 4096;
inline constexpr std::uint16_t kProgramStart = 0x200;
inline constexpr std::uint16_t kFontStart = 0x050;
inline constexpr int kScreenWidth = 64;
inline constexpr int kScreenHeight = 32;
inline constexpr std::size_t kMaxRomSize = kMemorySize - kProgramStart;

// Pure emulation core: no SDL, no ImGui, no file I/O.
// The frontend feeds it bytes and input, and reads back pixels and sound state.
class Chip8 {
public:
    // A fixed seed makes CXNN (random) reproducible in tests.
    explicit Chip8(std::uint32_t seed = std::random_device{}());

    void reset();
    bool load_rom(std::span<const std::uint8_t> rom);

    void step();         // fetch-decode-execute one instruction
    void tick_timers();  // call at 60 Hz

    void set_key(std::uint8_t key, bool pressed);

    // One byte per pixel (0 = off, 1 = on), row-major, 64x32.
    [[nodiscard]] std::span<const std::uint8_t> framebuffer() const { return display_; }
    [[nodiscard]] bool sound_active() const { return sound_timer_ > 0; }

    // Read-only views for tests and the future debugger.
    [[nodiscard]] std::uint16_t pc() const { return pc_; }
    [[nodiscard]] std::uint16_t index() const { return i_; }
    [[nodiscard]] std::uint8_t v(std::size_t reg) const { return v_.at(reg); }
    [[nodiscard]] std::uint8_t read(std::uint16_t addr) const { return memory_.at(addr); }

private:
    static constexpr std::uint8_t kNoKey = 0xFF;

    void execute_alu(std::size_t x, std::size_t y, std::uint8_t n);  // 8XYN
    void execute_misc(std::size_t x, std::uint8_t nn);               // FXNN
    void skip_next();
    void draw_sprite(std::uint8_t vx, std::uint8_t vy, std::uint8_t height);

    std::array<std::uint8_t, kMemorySize> memory_{};
    std::array<std::uint8_t, 16> v_{};
    std::array<std::uint16_t, 16> stack_{};
    std::array<std::uint8_t, kScreenWidth * kScreenHeight> display_{};
    std::array<bool, 16> keys_{};

    std::uint16_t pc_ = kProgramStart;
    std::uint16_t i_ = 0;
    std::uint8_t sp_ = 0;
    std::uint8_t delay_timer_ = 0;
    std::uint8_t sound_timer_ = 0;

    bool waiting_for_key_ = false;      // FX0A in progress
    std::uint8_t released_key_ = kNoKey;

    std::mt19937 rng_;
    std::uniform_int_distribution<int> byte_dist_{0, 255};
};

}  // namespace ezy::chip8
