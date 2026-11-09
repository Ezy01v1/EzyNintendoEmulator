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
// bit 3 is the IPL3's "boot done" acknowledge.
//
// Channel 4 is the cartridge's 4 Kbit EEPROM (64 blocks of 8 bytes): status 0x00,
// read block 0x04, write block 0x05. Games that save to it (Mario Kart 64) also wait for it
// at boot before they poll the controllers.
class Pif {
public:
    [[nodiscard]] std::span<std::uint8_t, 64> ram() { return ram_; }
    [[nodiscard]] std::span<const std::uint8_t, 64> ram() const { return ram_; }

    void run_commands();
    // Handles the control byte after the CPU (or an SI DMA) wrote PIF RAM.
    void after_write();
    // Port 0..3: A B Z Start Up Down Left Right, -, -, L R, C-Up C-Down C-Left C-Right.
    void set_buttons(int port, std::uint16_t buttons, std::int8_t x, std::int8_t y);
    [[nodiscard]] std::span<std::uint8_t, 512> eeprom() { return eeprom_; }

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        ar(ram_), ar(buttons_), ar(stick_);
        if (version >= 12) ar(eeprom_);
    }

private:
    void run_eeprom(std::uint8_t cmd, std::size_t data, std::size_t tx_len, std::size_t resp, std::size_t rx_len,
                    std::uint8_t& rx_byte);

    std::array<std::uint8_t, 64> ram_{};
    std::array<std::uint16_t, 4> buttons_{};
    std::array<std::int8_t, 8> stick_{};  // x, y per port
    std::array<std::uint8_t, 512> eeprom_ = [] {  // blank parts read back 0xFF
        std::array<std::uint8_t, 512> blank{};
        blank.fill(0xFF);
        return blank;
    }();
};

}  // namespace ezy::n64
