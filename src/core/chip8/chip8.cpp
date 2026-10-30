#include "chip8/chip8.hpp"

#include <algorithm>

namespace ezy::chip8 {

namespace {

// Built-in hex font: 16 characters (0-F), 5 bytes each, 4 pixels wide.
constexpr std::array<std::uint8_t, 80> kFont = {
    0xF0, 0x90, 0x90, 0x90, 0xF0,  // 0
    0x20, 0x60, 0x20, 0x20, 0x70,  // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0,  // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0,  // 3
    0x90, 0x90, 0xF0, 0x10, 0x10,  // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0,  // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0,  // 6
    0xF0, 0x10, 0x20, 0x40, 0x40,  // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0,  // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0,  // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90,  // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0,  // B
    0xF0, 0x80, 0x80, 0x80, 0xF0,  // C
    0xE0, 0x90, 0x90, 0x90, 0xE0,  // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0,  // E
    0xF0, 0x80, 0xF0, 0x80, 0x80,  // F
};

}  // namespace

Chip8::Chip8() { reset(); }

void Chip8::reset() {
    memory_.fill(0);
    v_.fill(0);
    stack_.fill(0);
    display_.fill(0);
    keys_.fill(false);
    pc_ = kProgramStart;
    i_ = 0;
    sp_ = 0;
    delay_timer_ = 0;
    sound_timer_ = 0;
    std::ranges::copy(kFont, memory_.begin() + kFontStart);
}

bool Chip8::load_rom(std::span<const std::uint8_t> rom) {
    if (rom.empty() || rom.size() > kMaxRomSize) {
        return false;
    }
    reset();
    std::ranges::copy(rom, memory_.begin() + kProgramStart);
    return true;
}

void Chip8::step() {
    // FETCH: two bytes, big-endian.
    const auto opcode = static_cast<std::uint16_t>((memory_[pc_] << 8) | memory_[pc_ + 1]);
    pc_ = static_cast<std::uint16_t>((pc_ + 2) & 0x0FFF);

    // DECODE: split the opcode into its fields.
    // TODO(exercise 1): extract x, y, n, nn, nnn from `opcode`.

    // EXECUTE: dispatch on the top nibble.
    switch (opcode >> 12) {
        // TODO(exercise 1): implement 00E0, 1NNN, 6XNN, 7XNN, ANNN, DXYN.
        default:
            break;  // unknown/unimplemented opcode: ignored for now
    }
}

void Chip8::tick_timers() {
    if (delay_timer_ > 0) --delay_timer_;
    if (sound_timer_ > 0) --sound_timer_;
}

void Chip8::set_key(std::uint8_t key, bool pressed) {
    if (key < keys_.size()) {
        keys_[key] = pressed;
    }
}

}  // namespace ezy::chip8
