#include "gb/bus.hpp"

namespace ezy::gb {

namespace {

constexpr std::uint16_t kJoypad = 0xFF00;         // P1/JOYP
constexpr std::uint16_t kSerialData = 0xFF01;     // SB
constexpr std::uint16_t kSerialControl = 0xFF02;  // SC
constexpr std::uint16_t kInterruptFlag = 0xFF0F;  // IF
constexpr std::uint16_t kOamDma = 0xFF46;
constexpr std::uint16_t kInterruptEnable = 0xFFFF;

constexpr bool is_timer(std::uint16_t addr) { return addr >= 0xFF04 && addr <= 0xFF07; }
constexpr bool is_apu(std::uint16_t addr) { return addr >= 0xFF10 && addr <= 0xFF3F; }
constexpr bool is_ppu(std::uint16_t addr) { return addr >= 0xFF40 && addr <= 0xFF4B && addr != kOamDma; }

}  // namespace

std::uint8_t Bus::read(std::uint16_t addr) {
    advance();
    return peek(addr);
}

void Bus::write(std::uint16_t addr, std::uint8_t value) {
    advance();
    poke(addr, value);
}

void Bus::tick() { advance(); }

void Bus::advance() {
    t_cycles_ += 4;
    const std::uint16_t counter_before = timer_.counter();
    timer_.tick();
    clock_frame_sequencer(counter_before);
    apu_.tick();
    cartridge_.tick(4);
    if (timer_.take_interrupt()) request_interrupt(interrupt::kTimer);
    ppu_.tick();
    request_interrupt(ppu_.take_interrupts());
}

std::uint8_t Bus::peek(std::uint16_t addr) const {
    if (addr < 0x8000) return cartridge_.read_rom(addr);
    if (addr < 0xA000) return ppu_.read_vram(addr);
    if (addr < 0xC000) return cartridge_.read_ram(addr);
    if (addr < 0xE000) return wram_[addr - 0xC000];
    if (addr < 0xFE00) return wram_[addr - 0xE000];  // echo RAM mirrors C000-DDFF
    if (addr < 0xFEA0) return ppu_.read_oam(addr);
    if (addr < 0xFF00) return 0xFF;  // unusable area
    if (addr < 0xFF80) return read_io(addr);
    if (addr < 0xFFFF) return hram_[addr - 0xFF80];
    return ie_;
}

void Bus::poke(std::uint16_t addr, std::uint8_t value) {
    if (addr < 0x8000) { cartridge_.write_rom(addr, value); return; }  // MBC registers
    if (addr < 0xA000) { ppu_.write_vram(addr, value); return; }
    if (addr < 0xC000) { cartridge_.write_ram(addr, value); return; }
    if (addr < 0xE000) { wram_[addr - 0xC000] = value; return; }
    if (addr < 0xFE00) { wram_[addr - 0xE000] = value; return; }
    if (addr < 0xFEA0) { ppu_.write_oam(addr, value); return; }
    if (addr < 0xFF00) return;  // unusable area
    if (addr < 0xFF80) { write_io(addr, value); return; }
    if (addr < kInterruptEnable) { hram_[addr - 0xFF80] = value; return; }
    ie_ = value;
}

void Bus::clock_frame_sequencer(std::uint16_t counter_before) {
    constexpr std::uint16_t kBit = 1U << 12;
    if ((counter_before & kBit) != 0 && (timer_.counter() & kBit) == 0) apu_.div_falling_edge();
}

void Bus::set_button(Button button, bool pressed) {
    if (joypad_.set(button, pressed)) request_interrupt(interrupt::kJoypad);
}

std::uint8_t Bus::read_io(std::uint16_t addr) const {
    if (is_timer(addr)) return timer_.read(addr);
    if (is_apu(addr)) return apu_.read(addr);
    if (is_ppu(addr)) return ppu_.read_register(addr);
    switch (addr) {
        case kJoypad: return joypad_.read();
        case kSerialControl: return io_[addr - 0xFF00] | 0x7E;  // unused bits read as 1
        case kInterruptFlag: return if_ | 0xE0;                 // upper 3 bits read as 1
        case kOamDma: return dma_;
        default: return io_[addr - 0xFF00];
    }
}

void Bus::write_io(std::uint16_t addr, std::uint8_t value) {
    if (is_timer(addr)) {
        const std::uint16_t counter_before = timer_.counter();
        timer_.write(addr, value);
        clock_frame_sequencer(counter_before);  // writing DIV can clock it
        if (timer_.take_interrupt()) request_interrupt(interrupt::kTimer);
        return;
    }
    if (is_apu(addr)) {
        apu_.write(addr, value);
        return;
    }
    if (is_ppu(addr)) {
        ppu_.write_register(addr, value);
        request_interrupt(ppu_.take_interrupts());
        return;
    }
    switch (addr) {
        case kJoypad: joypad_.write(value); break;
        case kSerialControl:
            // Transfer start with internal clock: there is no link cable, so we complete
            // the transfer instantly, log the byte and receive 0xFF.
            if ((value & 0x81) == 0x81) {
                serial_output_.push_back(static_cast<char>(io_[kSerialData - 0xFF00]));
                io_[kSerialData - 0xFF00] = 0xFF;
                io_[addr - 0xFF00] = value & 0x7F;  // transfer done: clear bit 7
                request_interrupt(interrupt::kSerial);
            } else {
                io_[addr - 0xFF00] = value;
            }
            break;
        case kInterruptFlag: if_ = value & 0x1F; break;
        case kOamDma: oam_dma(value); break;
        default: io_[addr - 0xFF00] = value; break;
    }
}

// Copies 160 bytes from page `page` (XX00-XX9F) to OAM.
// Simplification: done instantly; on hardware it takes 160 M-cycles during which the
// CPU can only access HRAM (games wait in HRAM, so this rarely matters).
void Bus::oam_dma(std::uint8_t page) {
    dma_ = page;
    const auto source = static_cast<std::uint16_t>(page << 8);
    for (std::uint16_t i = 0; i < 0xA0; ++i) {
        ppu_.write_oam(static_cast<std::uint16_t>(0xFE00 + i), peek(static_cast<std::uint16_t>(source + i)));
    }
}

}  // namespace ezy::gb
