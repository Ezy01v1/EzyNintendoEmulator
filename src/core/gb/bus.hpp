#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gb/apu.hpp"
#include "gb/cartridge.hpp"
#include "gb/cpu_bus.hpp"
#include "gb/hdma.hpp"
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
          cgb_mode_(model == Model::Cgb && cartridge_.header().supports_cgb()) {
        ppu_.set_model(model_, cgb_mode_);
        ppu_.set_opri(cgb_mode_ ? 0 : 1);
        apu_.set_model(model_);
    }

    [[nodiscard]] Model model() const { return model_; }

    // Maps a real CGB boot ROM (2304 bytes) over 0x0000-0x00FF and 0x0200-0x08FF until the
    // program writes 0xFF50. It runs in CGB mode; KEY0 (0xFF4C) bit 2, which it writes just
    // before leaving, decides whether the game then runs in DMG compatibility mode.
    void map_boot_rom(std::vector<std::uint8_t> boot_rom);
    [[nodiscard]] bool boot_rom_mapped() const { return !boot_rom_.empty(); }

    // Save states: everything but the model (checked by GameBoy) and the serial log.
    template <class Ar>
    void serialize(Ar& ar) {
        ar(cartridge_), ar(timer_), ar(ppu_), ar(apu_), ar(joypad_), ar(hdma_);
        ar(cgb_mode_), ar(wram_), ar(svbk_), ar(undocumented_), ar(rp_), ar(key0_);
        ar(io_), ar(hram_), ar(if_), ar(ie_), ar(dma_);
        ar(t_cycles_), ar(dots_), ar(double_speed_), ar(speed_switch_armed_), ar(apu_half_cycle_);
        // The boot ROM itself isn't stored, only whether it is still mapped. A state taken
        // during the boot animation can only be loaded while the boot ROM is still mapped.
        bool mapped = !boot_rom_.empty();
        ar(mapped);
        if constexpr (Ar::kLoading) {
            // Applied by finish_load() only once the whole state is known to be good, so a
            // failed load can still roll back to a machine that has its boot ROM.
            loaded_boot_mapped_ = mapped;
            if (mapped && boot_rom_.empty()) ar.fail();
        }
        ar(cpu_halted_);
    }
    // Completes a successful load_state().
    void finish_load() {
        if (!loaded_boot_mapped_) boot_rom_.clear();
    }
    // True when CGB features are enabled: a CGB game on CGB hardware. A DMG game on a CGB
    // runs in DMG compatibility mode, where most CGB registers are hidden.
    [[nodiscard]] bool cgb_mode() const { return cgb_mode_; }

    // WRAM bank mapped at 0xD000-0xDFFF (1-7; always 1 outside CGB mode).
    [[nodiscard]] int wram_bank() const { return svbk_ == 0 ? 1 : svbk_; }
    // OPRI bit 0: 0 = sprites ordered by OAM index (CGB), 1 = by X coordinate (DMG).
    [[nodiscard]] std::uint8_t opri() const { return ppu_.opri(); }

    std::uint8_t read(std::uint16_t addr) override;
    void write(std::uint16_t addr, std::uint8_t value) override;
    void tick() override;  // one internal M-cycle with no memory access (e.g. 16-bit ALU ops)
    void stop() override;
    // HALT state from the CPU: HBlank DMA does not run while the CPU is halted.
    void set_cpu_halted(bool halted) override { cpu_halted_ = halted; }

    [[nodiscard]] std::uint8_t pending_interrupts() const override { return ie_ & if_ & 0x1F; }
    void acknowledge_interrupt(std::uint8_t bit) override { if_ &= static_cast<std::uint8_t>(~bit); }

    [[nodiscard]] std::uint8_t peek(std::uint16_t addr) const;
    void poke(std::uint16_t addr, std::uint8_t value);

    // CPU clock (T-cycles): 4 per M-cycle at either speed.
    [[nodiscard]] std::uint64_t t_cycles() const { return t_cycles_; }
    // Dot clock (4.19 MHz): what the PPU, APU and frame timing run on.
    [[nodiscard]] std::uint64_t dots() const { return dots_; }
    // CGB double speed: the CPU, timer, serial and OAM DMA run twice as fast.
    [[nodiscard]] bool double_speed() const { return double_speed_; }

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
    [[nodiscard]] bool cpu_can_access(std::uint16_t addr) const;
    void advance();  // advances timer, APU, PPU and cartridge by one M-cycle
    // The APU frame sequencer runs off DIV: bit 12 of the timer counter falling.
    void clock_frame_sequencer(std::uint16_t counter_before);
    void write_io(std::uint16_t addr, std::uint8_t value);
    [[nodiscard]] std::uint8_t read_io(std::uint16_t addr) const;
    void oam_dma(std::uint8_t page);
    // Copies `blocks` VRAM DMA blocks, stalling the CPU while the rest of the system runs.
    void run_vram_dma(int blocks);

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
    std::array<std::uint8_t, 4> undocumented_{};  // 0xFF72-0xFF75
    std::uint8_t rp_ = 0;    // 0xFF56, writable bits 0 and 6-7
    Hdma hdma_;
    std::vector<std::uint8_t> boot_rom_;  // empty once unmapped (or never used)
    std::uint8_t key0_ = 0;
    bool in_vram_dma_ = false;
    bool cpu_halted_ = false;
    bool loaded_boot_mapped_ = false;  // see serialize()
    std::array<std::uint8_t, 0x80> io_{};  // raw storage for I/O registers not modelled yet
    std::array<std::uint8_t, 0x7F> hram_{};
    std::uint8_t if_ = 0;
    std::uint8_t ie_ = 0;
    std::uint8_t dma_ = 0xFF;

    std::string serial_output_;
    std::uint64_t t_cycles_ = 0;
    std::uint64_t dots_ = 0;
    bool double_speed_ = false;
    bool speed_switch_armed_ = false;  // KEY1 bit 0
    bool apu_half_cycle_ = false;      // double speed: the APU ticks every other M-cycle
};

}  // namespace ezy::gb
