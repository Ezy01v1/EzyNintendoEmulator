#pragma once

#include <cstdint>

namespace ezy::gb {

enum class Button : std::uint8_t { Right, Left, Up, Down, A, B, Select, Start };

// P1/JOYP (0xFF00). The 8 buttons are read as two groups of 4 through a 2x4 matrix:
// the game selects a group by writing 0 to bit 4 (d-pad) or bit 5 (buttons), then reads
// the low nibble, where 0 means pressed.
class Joypad {
public:
    [[nodiscard]] std::uint8_t read() const {
        std::uint8_t low = 0x0F;
        if (!(select_ & 0x10)) low &= static_cast<std::uint8_t>(~(pressed_ & 0x0F));  // d-pad
        if (!(select_ & 0x20)) low &= static_cast<std::uint8_t>(~(pressed_ >> 4));    // buttons
        return static_cast<std::uint8_t>(0xC0 | select_ | low);
    }

    void write(std::uint8_t value) { select_ = value & 0x30; }

    // Returns true when a button goes from released to pressed (joypad interrupt).
    bool set(Button button, bool pressed) {
        const auto bit = static_cast<std::uint8_t>(1 << static_cast<int>(button));
        const bool newly_pressed = pressed && !(pressed_ & bit);
        pressed_ = static_cast<std::uint8_t>(pressed ? (pressed_ | bit) : (pressed_ & ~bit));
        return newly_pressed;
    }

    template <class Ar>
    void serialize(Ar& ar) {
        ar(select_), ar(pressed_);
    }

private:
    std::uint8_t select_ = 0x30;
    std::uint8_t pressed_ = 0;  // bit set = pressed; low nibble d-pad, high nibble buttons
};

}  // namespace ezy::gb
