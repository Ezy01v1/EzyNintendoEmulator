#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ezy::n64 {

// The PIF: 64 bytes of RAM shared with the CPU (through SI) and the joybus controller
// ports (n64brew "PIF", "Joybus Protocol").
//
// A joybus command block in PIF RAM is a list of per-channel entries, starting at port 0:
//   0x00         skip this channel        0xFF  padding         0xFE  end of the list
//   tx rx data   send tx bytes, receive rx bytes into the bytes that follow
// The last byte of RAM (63) is the control byte: bit 0 runs the joybus commands,
// bit 3 is the IPL3's "boot done" acknowledge. A command block stays armed after it
// runs: every later SI read of PIF RAM runs it again, which is how libultra polls the
// controllers (osContStartReadData writes the block only when the command changes).
class Pif {
public:
    [[nodiscard]] std::span<std::uint8_t, 64> ram() { return ram_; }
    [[nodiscard]] std::span<const std::uint8_t, 64> ram() const { return ram_; }

    void run_commands();
    // Handles the control byte after the CPU (or an SI DMA) wrote PIF RAM.
    void after_write();
    // Called before an SI DMA copies PIF RAM to RDRAM.
    void before_read() { if (joybus_armed_) run_commands(); }
    // Port 0..3: A B Z Start Up Down Left Right, -, -, L R, C-Up C-Down C-Left C-Right.
    void set_buttons(int port, std::uint16_t buttons, std::int8_t x, std::int8_t y);

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        ar(ram_), ar(buttons_), ar(stick_);
        if (version >= 12) ar(joybus_armed_);
        else if constexpr (Ar::kLoading) joybus_armed_ = false;
    }

private:
    std::array<std::uint8_t, 64> ram_{};
    std::array<std::uint16_t, 4> buttons_{};
    std::array<std::int8_t, 8> stick_{};  // x, y per port
    bool joybus_armed_ = false;            // RAM holds a command block that SI reads rerun
};

}  // namespace ezy::n64
