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

// Behaviours that differ between CHIP-8 interpreters. Defaults match the original COSMAC VIP.
struct Quirks {
    bool vf_reset = true;          // 8XY1/8XY2/8XY3 set VF = 0
    bool memory_increment = true;  // FX55/FX65 leave I = I + X + 1
    bool display_wait = true;      // DXYN waits for the next 60 Hz tick (max one draw per frame)
    bool clipping = true;          // sprites are clipped at the screen edge instead of wrapping
    bool shift_uses_vy = true;     // 8XY6/8XYE shift VY into VX (false: shift VX in place)
    bool jump_uses_v0 = true;      // BNNN jumps to NNN + V0 (false: BXNN jumps to XNN + VX)

    static constexpr Quirks cosmac_vip() { return {}; }
    static constexpr Quirks superchip_modern() { return {false, false, false, true, false, false}; }
};

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

    void set_quirks(const Quirks& quirks) { quirks_ = quirks; }
    [[nodiscard]] const Quirks& quirks() const { return quirks_; }

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
    bool drew_this_frame_ = false;      // display-wait quirk

    Quirks quirks_{};

    std::mt19937 rng_;
    std::uniform_int_distribution<int> byte_dist_{0, 255};
};

}  // namespace ezy::chip8
