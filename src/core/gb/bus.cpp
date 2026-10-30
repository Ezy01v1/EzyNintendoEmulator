#include "gb/bus.hpp"

namespace ezy::gb {

namespace {

constexpr std::uint16_t kSerialData = 0xFF01;     // SB
constexpr std::uint16_t kSerialControl = 0xFF02;  // SC
constexpr std::uint16_t kInterruptFlag = 0xFF0F;  // IF
constexpr std::uint16_t kInterruptEnable = 0xFFFF;

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
    timer_.tick();
    if (timer_.take_interrupt()) request_interrupt(interrupt::kTimer);
    // PPU and DMA will be stepped here.
}

std::uint8_t Bus::peek(std::uint16_t addr) const {
    if (addr < 0x8000) return cartridge_.read_rom(addr);
    if (addr < 0xA000) return vram_[addr - 0x8000];
    if (addr < 0xC000) return 0xFF;  // external RAM: not implemented until MBCs
    if (addr < 0xE000) return wram_[addr - 0xC000];
    if (addr < 0xFE00) return wram_[addr - 0xE000];  // echo RAM mirrors C000-DDFF
    if (addr < 0xFEA0) return oam_[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;  // unusable area
    if (addr < 0xFF80) return read_io(addr);
    if (addr < 0xFFFF) return hram_[addr - 0xFF80];
    return ie_;
}

void Bus::poke(std::uint16_t addr, std::uint8_t value) {
    if (addr < 0x8000) { cartridge_.write_rom(addr, value); return; }  // MBC registers
    if (addr < 0xA000) { vram_[addr - 0x8000] = value; return; }
    if (addr < 0xC000) return;  // external RAM: not implemented until MBCs
    if (addr < 0xE000) { wram_[addr - 0xC000] = value; return; }
    if (addr < 0xFE00) { wram_[addr - 0xE000] = value; return; }
    if (addr < 0xFEA0) { oam_[addr - 0xFE00] = value; return; }
    if (addr < 0xFF00) return;  // unusable area
    if (addr < 0xFF80) { write_io(addr, value); return; }
    if (addr < kInterruptEnable) { hram_[addr - 0xFF80] = value; return; }
    ie_ = value;
}

std::uint8_t Bus::read_io(std::uint16_t addr) const {
    if (addr >= 0xFF04 && addr <= 0xFF07) return timer_.read(addr);
    switch (addr) {
        case kSerialControl: return io_[addr - 0xFF00] | 0x7E;  // unused bits read as 1
        case kInterruptFlag: return if_ | 0xE0;                 // upper 3 bits read as 1
        default: return io_[addr - 0xFF00];
    }
}

void Bus::write_io(std::uint16_t addr, std::uint8_t value) {
    if (addr >= 0xFF04 && addr <= 0xFF07) {
        timer_.write(addr, value);
        if (timer_.take_interrupt()) request_interrupt(interrupt::kTimer);
        return;
    }
    switch (addr) {
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
        case kInterruptFlag:
            if_ = value & 0x1F;
            break;
        default:
            io_[addr - 0xFF00] = value;
            break;
    }
}

}  // namespace ezy::gb
