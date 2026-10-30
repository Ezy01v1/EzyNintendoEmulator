#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "gb/cartridge.hpp"

namespace ezy::gb {

// Interrupt bits, shared by IE (0xFFFF) and IF (0xFF0F).
namespace interrupt {
inline constexpr std::uint8_t kVBlank = 1 << 0;
inline constexpr std::uint8_t kStat = 1 << 1;
inline constexpr std::uint8_t kTimer = 1 << 2;
inline constexpr std::uint8_t kSerial = 1 << 3;
inline constexpr std::uint8_t kJoypad = 1 << 4;
}  // namespace interrupt

// The memory bus: routes every CPU access to the right component.
//
// Timing model (option B): every read()/write() costs one M-cycle (4 T-cycles) and
// advances the rest of the system. peek()/poke() access memory without any timing
// side effects; they are for tests and the debugger only.
class Bus {
public:
    explicit Bus(Cartridge cartridge) : cartridge_(std::move(cartridge)) {}

    std::uint8_t read(std::uint16_t addr);
    void write(std::uint16_t addr, std::uint8_t value);
    void tick();  // one internal M-cycle with no memory access (e.g. 16-bit ALU ops)

    [[nodiscard]] std::uint8_t peek(std::uint16_t addr) const;
    void poke(std::uint16_t addr, std::uint8_t value);

    [[nodiscard]] std::uint64_t t_cycles() const { return t_cycles_; }

    void request_interrupt(std::uint8_t bits) { if_ |= bits; }
    [[nodiscard]] std::uint8_t interrupt_flags() const { return if_; }
    [[nodiscard]] std::uint8_t interrupt_enable() const { return ie_; }

    // Bytes sent over the serial port. Blargg's test ROMs print their results here.
    [[nodiscard]] const std::string& serial_output() const { return serial_output_; }

    [[nodiscard]] const Cartridge& cartridge() const { return cartridge_; }

private:
    void advance();  // advances timer/PPU/... by one M-cycle (they come later)
    void write_io(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_io(std::uint16_t addr) const;

    Cartridge cartridge_;
    std::array<std::uint8_t, 0x2000> vram_{};
    std::array<std::uint8_t, 0x2000> wram_{};
    std::array<std::uint8_t, 0xA0> oam_{};
    std::array<std::uint8_t, 0x80> io_{};  // raw storage for I/O registers not modelled yet
    std::array<std::uint8_t, 0x7F> hram_{};
    std::uint8_t if_ = 0;
    std::uint8_t ie_ = 0;

    std::string serial_output_;
    std::uint64_t t_cycles_ = 0;
};

}  // namespace ezy::gb
