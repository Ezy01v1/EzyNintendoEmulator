#pragma once

namespace ezy::n64 {

class Bus;
class Cartridge;
class Vr4300;

// Does what the PIF ROM (IPL1/IPL2) does before the cartridge's own boot code runs:
// copies the IPL3 (ROM 0x40-0xFFF) to SP DMEM, leaves the CPU registers as the PIF
// does (n64brew "PIF-NUS", "Initial Program Load") and jumps to 0xA4000040. The real
// IPL3 then runs from there and loads the game.
void hle_boot(Vr4300& cpu, Bus& bus, const Cartridge& cart);

}  // namespace ezy::n64
