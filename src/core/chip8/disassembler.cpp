#include "chip8/disassembler.hpp"

#include <format>

namespace ezy::chip8 {

std::string disassemble(std::uint16_t opcode) {
    const unsigned x = (opcode >> 8) & 0xF;
    const unsigned y = (opcode >> 4) & 0xF;
    const unsigned n = opcode & 0xF;
    const unsigned nn = opcode & 0xFF;
    const unsigned nnn = opcode & 0xFFF;

    switch (opcode >> 12) {
        case 0x0:
            if (opcode == 0x00E0) return "CLS";
            if (opcode == 0x00EE) return "RET";
            return std::format("SYS  {:03X}", nnn);
        case 0x1: return std::format("JP   {:03X}", nnn);
        case 0x2: return std::format("CALL {:03X}", nnn);
        case 0x3: return std::format("SE   V{:X}, {:02X}", x, nn);
        case 0x4: return std::format("SNE  V{:X}, {:02X}", x, nn);
        case 0x5:
            if (n == 0) return std::format("SE   V{:X}, V{:X}", x, y);
            break;
        case 0x6: return std::format("LD   V{:X}, {:02X}", x, nn);
        case 0x7: return std::format("ADD  V{:X}, {:02X}", x, nn);
        case 0x8:
            switch (n) {
                case 0x0: return std::format("LD   V{:X}, V{:X}", x, y);
                case 0x1: return std::format("OR   V{:X}, V{:X}", x, y);
                case 0x2: return std::format("AND  V{:X}, V{:X}", x, y);
                case 0x3: return std::format("XOR  V{:X}, V{:X}", x, y);
                case 0x4: return std::format("ADD  V{:X}, V{:X}", x, y);
                case 0x5: return std::format("SUB  V{:X}, V{:X}", x, y);
                case 0x6: return std::format("SHR  V{:X}, V{:X}", x, y);
                case 0x7: return std::format("SUBN V{:X}, V{:X}", x, y);
                case 0xE: return std::format("SHL  V{:X}, V{:X}", x, y);
                default: break;
            }
            break;
        case 0x9:
            if (n == 0) return std::format("SNE  V{:X}, V{:X}", x, y);
            break;
        case 0xA: return std::format("LD   I, {:03X}", nnn);
        case 0xB: return std::format("JP   V0, {:03X}", nnn);
        case 0xC: return std::format("RND  V{:X}, {:02X}", x, nn);
        case 0xD: return std::format("DRW  V{:X}, V{:X}, {:X}", x, y, n);
        case 0xE:
            if (nn == 0x9E) return std::format("SKP  V{:X}", x);
            if (nn == 0xA1) return std::format("SKNP V{:X}", x);
            break;
        case 0xF:
            switch (nn) {
                case 0x07: return std::format("LD   V{:X}, DT", x);
                case 0x0A: return std::format("LD   V{:X}, K", x);
                case 0x15: return std::format("LD   DT, V{:X}", x);
                case 0x18: return std::format("LD   ST, V{:X}", x);
                case 0x1E: return std::format("ADD  I, V{:X}", x);
                case 0x29: return std::format("LD   F, V{:X}", x);
                case 0x33: return std::format("LD   B, V{:X}", x);
                case 0x55: return std::format("LD   [I], V{:X}", x);
                case 0x65: return std::format("LD   V{:X}, [I]", x);
                default: break;
            }
            break;
        default:
            break;
    }
    return std::format("DW   {:04X}", opcode);  // not a valid instruction: raw data
}

}  // namespace ezy::chip8
