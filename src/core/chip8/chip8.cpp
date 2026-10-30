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
    const std::size_t x = (opcode >> 8) & 0xF;
    const std::size_t y = (opcode >> 4) & 0xF;
    const std::uint8_t n = opcode & 0xF;
    const auto nn = static_cast<std::uint8_t>(opcode & 0xFF);
    const std::uint16_t nnn = opcode & 0xFFF;

    // EXECUTE: dispatch on the top nibble.
    switch (opcode >> 12) {
        case 0x0:
            if (opcode == 0x00E0) {  // 00E0: clear screen
                display_.fill(0);
            }
            break;
        case 0x1:  // 1NNN: jump
            pc_ = nnn;
            break;
        case 0x6:  // 6XNN: VX = NN
            v_[x] = nn;
            break;
        case 0x7:  // 7XNN: VX += NN (wraps, VF untouched)
            v_[x] = static_cast<std::uint8_t>(v_[x] + nn);
            break;
        case 0xA:  // ANNN: I = NNN
            i_ = nnn;
            break;
        case 0xD:  // DXYN: draw N-row sprite from memory[I] at (VX, VY)
            draw_sprite(v_[x], v_[y], n);
            break;
        default:
            break;  // unknown/unimplemented opcode: ignored for now
    }
}

void Chip8::draw_sprite(std::uint8_t vx, std::uint8_t vy, std::uint8_t height) {
    // The start position wraps around the screen; pixels past the edge are clipped.
    const int start_x = vx % kScreenWidth;
    const int start_y = vy % kScreenHeight;
    v_[0xF] = 0;

    for (int row = 0; row < height; ++row) {
        const int py = start_y + row;
        if (py >= kScreenHeight) break;

        const std::uint8_t sprite_byte = memory_[(i_ + row) & 0x0FFF];
        for (int col = 0; col < 8; ++col) {
            const int px = start_x + col;
            if (px >= kScreenWidth) break;

            if ((sprite_byte >> (7 - col)) & 1) {
                auto& pixel = display_[static_cast<std::size_t>(py * kScreenWidth + px)];
                if (pixel) v_[0xF] = 1;  // a lit pixel is being turned off: collision
                pixel ^= 1;
            }
        }
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
