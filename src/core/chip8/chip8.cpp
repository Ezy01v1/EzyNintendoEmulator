#include "chip8/chip8.hpp"

#include <algorithm>
#include <cassert>

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

constexpr std::uint16_t kAddressMask = 0x0FFF;

}  // namespace

Chip8::Chip8(std::uint32_t seed) : rng_(seed) { reset(); }

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
    waiting_for_key_ = false;
    released_key_ = kNoKey;
    drew_this_frame_ = false;
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
    const auto opcode = static_cast<std::uint16_t>((memory_[pc_] << 8) | memory_[(pc_ + 1) & kAddressMask]);
    pc_ = static_cast<std::uint16_t>((pc_ + 2) & kAddressMask);

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
            } else if (opcode == 0x00EE) {  // 00EE: return from subroutine
                assert(sp_ > 0 && "stack underflow");
                pc_ = stack_[--sp_ & 0xF];
            }
            break;
        case 0x1:  // 1NNN: jump
            pc_ = nnn;
            break;
        case 0x2:  // 2NNN: call subroutine
            assert(sp_ < stack_.size() && "stack overflow");
            stack_[sp_++ & 0xF] = pc_;
            pc_ = nnn;
            break;
        case 0x3:  // 3XNN: skip if VX == NN
            if (v_[x] == nn) skip_next();
            break;
        case 0x4:  // 4XNN: skip if VX != NN
            if (v_[x] != nn) skip_next();
            break;
        case 0x5:  // 5XY0: skip if VX == VY
            if (n == 0 && v_[x] == v_[y]) skip_next();
            break;
        case 0x6:  // 6XNN: VX = NN
            v_[x] = nn;
            break;
        case 0x7:  // 7XNN: VX += NN (wraps, VF untouched)
            v_[x] = static_cast<std::uint8_t>(v_[x] + nn);
            break;
        case 0x8:
            execute_alu(x, y, n);
            break;
        case 0x9:  // 9XY0: skip if VX != VY
            if (n == 0 && v_[x] != v_[y]) skip_next();
            break;
        case 0xA:  // ANNN: I = NNN
            i_ = nnn;
            break;
        case 0xB:  // BNNN: jump to NNN + V0 (quirk off: BXNN jumps to XNN + VX)
            pc_ = static_cast<std::uint16_t>((nnn + v_[quirks_.jump_uses_v0 ? 0 : x]) & kAddressMask);
            break;
        case 0xC:  // CXNN: VX = random byte AND NN
            v_[x] = static_cast<std::uint8_t>(byte_dist_(rng_) & nn);
            break;
        case 0xD:  // DXYN: draw N-row sprite from memory[I] at (VX, VY)
            if (quirks_.display_wait && drew_this_frame_) {
                pc_ = static_cast<std::uint16_t>((pc_ - 2) & kAddressMask);  // stall until the next tick
                break;
            }
            draw_sprite(v_[x], v_[y], n);
            drew_this_frame_ = true;
            break;
        case 0xE:
            if (nn == 0x9E && keys_[v_[x] & 0xF]) skip_next();   // EX9E: skip if key VX down
            if (nn == 0xA1 && !keys_[v_[x] & 0xF]) skip_next();  // EXA1: skip if key VX up
            break;
        case 0xF:
            execute_misc(x, nn);
            break;
        default:
            break;
    }
}

void Chip8::execute_alu(std::size_t x, std::size_t y, std::uint8_t n) {
    // The flag is always written after the result, so VF as destination ends up holding the flag.
    const std::uint8_t vx = v_[x];
    const std::uint8_t vy = v_[y];
    const std::uint8_t shift_src = quirks_.shift_uses_vy ? vy : vx;
    switch (n) {
        case 0x0: v_[x] = vy; break;  // 8XY0: VX = VY
        case 0x1:                     // 8XY1: OR
            v_[x] = vx | vy;
            if (quirks_.vf_reset) v_[0xF] = 0;
            break;
        case 0x2:  // 8XY2: AND
            v_[x] = vx & vy;
            if (quirks_.vf_reset) v_[0xF] = 0;
            break;
        case 0x3:  // 8XY3: XOR
            v_[x] = vx ^ vy;
            if (quirks_.vf_reset) v_[0xF] = 0;
            break;
        case 0x4: {                                                       // 8XY4: add, VF = carry
            const int sum = vx + vy;
            v_[x] = static_cast<std::uint8_t>(sum);
            v_[0xF] = sum > 0xFF ? 1 : 0;
            break;
        }
        case 0x5:                                                         // 8XY5: VX - VY, VF = no borrow
            v_[x] = static_cast<std::uint8_t>(vx - vy);
            v_[0xF] = vx >= vy ? 1 : 0;
            break;
        case 0x6:                                                         // 8XY6: shift right, VF = lost bit
            v_[x] = static_cast<std::uint8_t>(shift_src >> 1);
            v_[0xF] = shift_src & 0x1;
            break;
        case 0x7:                                                         // 8XY7: VY - VX, VF = no borrow
            v_[x] = static_cast<std::uint8_t>(vy - vx);
            v_[0xF] = vy >= vx ? 1 : 0;
            break;
        case 0xE:                                                         // 8XYE: shift left, VF = lost bit
            v_[x] = static_cast<std::uint8_t>(shift_src << 1);
            v_[0xF] = (shift_src >> 7) & 0x1;
            break;
        default:
            break;
    }
}

void Chip8::execute_misc(std::size_t x, std::uint8_t nn) {
    switch (nn) {
        case 0x07:  // FX07: VX = delay timer
            v_[x] = delay_timer_;
            break;
        case 0x0A:  // FX0A: block until a key is pressed and released
            if (!waiting_for_key_) {
                waiting_for_key_ = true;
                released_key_ = kNoKey;
            }
            if (released_key_ == kNoKey) {
                pc_ = static_cast<std::uint16_t>((pc_ - 2) & kAddressMask);  // re-run this instruction
            } else {
                v_[x] = released_key_;
                waiting_for_key_ = false;
            }
            break;
        case 0x15:  // FX15: delay timer = VX
            delay_timer_ = v_[x];
            break;
        case 0x18:  // FX18: sound timer = VX
            sound_timer_ = v_[x];
            break;
        case 0x1E:  // FX1E: I += VX
            i_ = static_cast<std::uint16_t>((i_ + v_[x]) & kAddressMask);
            break;
        case 0x29:  // FX29: I = address of font glyph for the low nibble of VX
            i_ = static_cast<std::uint16_t>(kFontStart + (v_[x] & 0xF) * 5);
            break;
        case 0x33:  // FX33: store BCD of VX at I, I+1, I+2
            memory_[i_ & kAddressMask] = v_[x] / 100;
            memory_[(i_ + 1) & kAddressMask] = (v_[x] / 10) % 10;
            memory_[(i_ + 2) & kAddressMask] = v_[x] % 10;
            break;
        case 0x55:  // FX55: store V0..VX at I
            for (std::size_t r = 0; r <= x; ++r) {
                memory_[(i_ + r) & kAddressMask] = v_[r];
            }
            if (quirks_.memory_increment) i_ = static_cast<std::uint16_t>((i_ + x + 1) & kAddressMask);
            break;
        case 0x65:  // FX65: load V0..VX from I
            for (std::size_t r = 0; r <= x; ++r) {
                v_[r] = memory_[(i_ + r) & kAddressMask];
            }
            if (quirks_.memory_increment) i_ = static_cast<std::uint16_t>((i_ + x + 1) & kAddressMask);
            break;
        default:
            break;
    }
}

void Chip8::skip_next() {
    pc_ = static_cast<std::uint16_t>((pc_ + 2) & kAddressMask);
}

void Chip8::draw_sprite(std::uint8_t vx, std::uint8_t vy, std::uint8_t height) {
    // The start position always wraps; pixels past the edge are clipped (or wrap, with the quirk off).
    const int start_x = vx % kScreenWidth;
    const int start_y = vy % kScreenHeight;
    v_[0xF] = 0;

    for (int row = 0; row < height; ++row) {
        int py = start_y + row;
        if (py >= kScreenHeight) {
            if (quirks_.clipping) break;
            py %= kScreenHeight;
        }

        const std::uint8_t sprite_byte = memory_[(i_ + row) & kAddressMask];
        for (int col = 0; col < 8; ++col) {
            int px = start_x + col;
            if (px >= kScreenWidth) {
                if (quirks_.clipping) break;
                px %= kScreenWidth;
            }

            if ((sprite_byte >> (7 - col)) & 1) {
                auto& pixel = display_[static_cast<std::size_t>(py * kScreenWidth + px)];
                if (pixel) v_[0xF] = 1;  // a lit pixel is being turned off: collision
                pixel ^= 1;
            }
        }
    }
}

void Chip8::tick_timers() {
    drew_this_frame_ = false;
    if (delay_timer_ > 0) --delay_timer_;
    if (sound_timer_ > 0) --sound_timer_;
}

void Chip8::set_key(std::uint8_t key, bool pressed) {
    if (key >= keys_.size()) {
        return;
    }
    if (keys_[key] && !pressed) {
        released_key_ = key;  // consumed by FX0A
    }
    keys_[key] = pressed;
}

}  // namespace ezy::chip8
