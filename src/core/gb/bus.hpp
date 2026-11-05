#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "gb/cartridge.hpp"
#include "gb/cpu_bus.hpp"
#include "gb/interrupts.hpp"
#include "gb/joypad.hpp"
#include "gb/ppu.hpp"
#include "gb/timer.hpp"

namespace ezy::gb {

// The memory bus: routes every CPU access to the right component.
//
// Timing model (option B): every read()/write() costs one M-cycle (4 T-cycles) and
// advances the rest of the system. peek()/poke() access memory without any timing
// side effects; they are for tests and the debugger only.
class Bus final : public CpuBus {
public:
    explicit Bus(Cartridge cartridge) : cartridge_(std::move(cartridge)) {}

    std::uint8_t read(std::uint16_t addr) override;
    void write(std::uint16_t addr, std::uint8_t value) override;
    void tick() override;  // one internal M-cycle with no memory access (e.g. 16-bit ALU ops)

    [[nodiscard]] std::uint8_t pending_interrupts() const override { return ie_ & if_ & 0x1F; }
    void acknowledge_interrupt(std::uint8_t bit) override { if_ &= static_cast<std::uint8_t>(~bit); }

    [[nodiscard]] std::uint8_t peek(std::uint16_t addr) const;
    void poke(std::uint16_t addr, std::uint8_t value);

    [[nodiscard]] std::uint64_t t_cycles() const { return t_cycles_; }

    void request_interrupt(std::uint8_t bits) { if_ |= bits; }
    [[nodiscard]] std::uint8_t interrupt_flags() const { return if_; }
    [[nodiscard]] std::uint8_t interrupt_enable() const { return ie_; }

    void set_button(Button button, bool pressed);

    // Bytes sent over the serial port. Blargg's test ROMs print their results here.
    [[nodiscard]] const std::string& serial_output() const { return serial_output_; }

    [[nodiscard]] const Cartridge& cartridge() const { return cartridge_; }
    [[nodiscard]] Cartridge& cartridge() { return cartridge_; }
    [[nodiscard]] Ppu& ppu() { return ppu_; }
    [[nodiscard]] const Ppu& ppu() const { return ppu_; }

private:
    void advance();  // advances timer and PPU by one M-cycle
    void write_io(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_io(std::uint16_t addr) const;
    void oam_dma(std::uint8_t page);

    Cartridge cartridge_;
    Timer timer_;
    Ppu ppu_;
    Joypad joypad_;
    std::array<std::uint8_t, 0x2000> wram_{};
    std::array<std::uint8_t, 0x80> io_{};  // raw storage for I/O registers not modelled yet
    std::array<std::uint8_t, 0x7F> hram_{};
    std::uint8_t if_ = 0;
    std::uint8_t ie_ = 0;
    std::uint8_t dma_ = 0xFF;

    std::string serial_output_;
    std::uint64_t t_cycles_ = 0;
};

}  // namespace ezy::gb
