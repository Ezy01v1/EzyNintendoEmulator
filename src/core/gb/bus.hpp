#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "gb/apu.hpp"
#include "gb/cartridge.hpp"
#include "gb/cpu_bus.hpp"
#include "gb/interrupts.hpp"
#include "gb/joypad.hpp"
#include "gb/model.hpp"
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
    explicit Bus(Cartridge cartridge, Model model = Model::Dmg)
        : cartridge_(std::move(cartridge)),
          model_(model),
          cgb_mode_(model == Model::Cgb && cartridge_.header().supports_cgb()),
          opri_(cgb_mode_ ? 0 : 1) {}

    [[nodiscard]] Model model() const { return model_; }
    // True when CGB features are enabled: a CGB game on CGB hardware. A DMG game on a CGB
    // runs in DMG compatibility mode, where most CGB registers are hidden.
    [[nodiscard]] bool cgb_mode() const { return cgb_mode_; }

    // WRAM bank mapped at 0xD000-0xDFFF (1-7; always 1 outside CGB mode).
    [[nodiscard]] int wram_bank() const { return svbk_ == 0 ? 1 : svbk_; }
    // OPRI bit 0: 0 = sprites ordered by OAM index (CGB), 1 = by X coordinate (DMG).
    [[nodiscard]] std::uint8_t opri() const { return opri_; }

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
    [[nodiscard]] Apu& apu() { return apu_; }
    [[nodiscard]] const Apu& apu() const { return apu_; }
    [[nodiscard]] Ppu& ppu() { return ppu_; }
    [[nodiscard]] const Ppu& ppu() const { return ppu_; }

private:
    void advance();  // advances timer, APU, PPU and cartridge by one M-cycle
    // The APU frame sequencer runs off DIV: bit 12 of the timer counter falling.
    void clock_frame_sequencer(std::uint16_t counter_before);
    void write_io(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_io(std::uint16_t addr) const;
    void oam_dma(std::uint8_t page);

    Cartridge cartridge_;
    Model model_;
    bool cgb_mode_;
    Timer timer_;
    Ppu ppu_;
    Apu apu_;
    Joypad joypad_;
    [[nodiscard]] std::size_t wram_index(std::uint16_t addr) const;  // addr in 0xC000-0xDFFF
    [[nodiscard]] std::optional<std::uint8_t> read_cgb_io(std::uint16_t addr) const;
    [[nodiscard]] bool write_cgb_io(std::uint16_t addr, std::uint8_t value);

    std::array<std::uint8_t, 0x8000> wram_{};  // 8 banks of 4 KiB (banks 2-7 only on CGB)
    // CGB registers.
    std::uint8_t svbk_ = 1;  // 0xFF70, bits 0-2
    std::uint8_t opri_;      // 0xFF6C, bit 0
    std::array<std::uint8_t, 4> undocumented_{};  // 0xFF72-0xFF75
    std::uint8_t rp_ = 0;    // 0xFF56, writable bits 0 and 6-7
    std::array<std::uint8_t, 0x80> io_{};  // raw storage for I/O registers not modelled yet
    std::array<std::uint8_t, 0x7F> hram_{};
    std::uint8_t if_ = 0;
    std::uint8_t ie_ = 0;
    std::uint8_t dma_ = 0xFF;

    std::string serial_output_;
    std::uint64_t t_cycles_ = 0;
};

}  // namespace ezy::gb
