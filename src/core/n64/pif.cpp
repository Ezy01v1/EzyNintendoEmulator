#include "n64/pif.hpp"

#include <cstdio>

namespace ezy::n64 {

namespace {

constexpr int kConnectedPorts = 1;  // one standard controller in port 0, no pak
constexpr int kEepromChannel = 4;

}  // namespace

void Pif::set_buttons(int port, std::uint16_t buttons, std::int8_t x, std::int8_t y) {
    if (port < 0 || port > 3) return;
    buttons_[static_cast<std::size_t>(port)] = buttons;
    stick_[static_cast<std::size_t>(port) * 2] = x;
    stick_[static_cast<std::size_t>(port) * 2 + 1] = y;
}

void Pif::run_commands() {
    int channel = 0;
    std::size_t i = 0;
    while (i < 63 && channel < 5) {
        const std::uint8_t tx = ram_[i];
        if (tx == 0xFE) break;
        if (tx == 0xFF) {  // padding
            ++i;
            continue;
        }
        if (tx == 0x00) {  // skip channel
            ++channel;
            ++i;
            continue;
        }
        if (i + 1 >= 63) break;
        const std::size_t tx_len = tx & 0x3F;
        std::uint8_t& rx_byte = ram_[i + 1];
        const std::size_t rx_len = rx_byte & 0x3F;
        const std::size_t data = i + 2;
        const std::size_t resp = data + tx_len;
        if (resp + rx_len > 63) break;
        const std::uint8_t cmd = tx_len > 0 ? ram_[data] : 0xFF;

        if (channel == kEepromChannel) {
            run_eeprom(cmd, data, tx_len, resp, rx_len, rx_byte);
        } else if (channel >= kConnectedPorts) {
            rx_byte |= 0x80;  // no device
        } else if ((cmd == 0x00 || cmd == 0xFF) && rx_len >= 3) {  // status
            ram_[resp] = 0x05;      // standard controller
            ram_[resp + 1] = 0x00;
            ram_[resp + 2] = 0x02;  // no pak
        } else if (cmd == 0x01 && rx_len >= 4) {  // read buttons
            const auto p = static_cast<std::size_t>(channel);
            ram_[resp] = static_cast<std::uint8_t>(buttons_[p] >> 8);
            ram_[resp + 1] = static_cast<std::uint8_t>(buttons_[p]);
            ram_[resp + 2] = static_cast<std::uint8_t>(stick_[p * 2]);
            ram_[resp + 3] = static_cast<std::uint8_t>(stick_[p * 2 + 1]);
        } else {
            rx_byte |= 0x80;  // controller paks are not emulated yet
        }
        i = resp + rx_len;
        ++channel;
    }
}

void Pif::run_eeprom(std::uint8_t cmd, std::size_t data, std::size_t tx_len, std::size_t resp, std::size_t rx_len,
                     std::uint8_t& rx_byte) {
    constexpr std::size_t kBlocks = 64;  // 4 Kbit
    if ((cmd == 0x00 || cmd == 0xFF) && rx_len >= 3) {  // status: 4 Kbit, idle
        ram_[resp] = 0x00, ram_[resp + 1] = 0x80, ram_[resp + 2] = 0x00;
    } else if (cmd == 0x04 && tx_len >= 2 && rx_len >= 8) {  // read a block
        const std::size_t block = (ram_[data + 1] % kBlocks) * 8;
        for (std::size_t k = 0; k < 8; ++k) ram_[resp + k] = eeprom_[block + k];
    } else if (cmd == 0x05 && tx_len >= 10 && rx_len >= 1) {  // write a block
        const std::size_t block = (ram_[data + 1] % kBlocks) * 8;
        for (std::size_t k = 0; k < 8; ++k) eeprom_[block + k] = ram_[data + 2 + k];
        ram_[resp] = 0x00;  // not busy
    } else {
        rx_byte |= 0x80;
    }
}

void Pif::after_write() {
    std::uint8_t& control = ram_[63];
    if (control & 0x01) {
        run_commands();
        control &= static_cast<std::uint8_t>(~0x01);
    }
    if (control & 0x08) control &= static_cast<std::uint8_t>(~0x08);  // boot done: acknowledged
    if (control & 0x02) {
        // CIC challenge (only CIC 6105 games): not emulated in this subproject.
        static bool logged = false;
        if (!logged) std::fprintf(stderr, "N64: CIC challenge requested (not emulated)\n");
        logged = true;
        control &= static_cast<std::uint8_t>(~0x02);
    }
}

}  // namespace ezy::n64
