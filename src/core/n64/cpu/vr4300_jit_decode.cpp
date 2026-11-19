// VR4300 JIT: what a block does with each instruction (see vr4300_jit.hpp).
#include "n64/cpu/vr4300_jit.hpp"

namespace ezy::n64 {

MemoryAccess memory_access(std::uint32_t instr) {
    switch (instr >> 26) {
        case 0x20: case 0x24: return {1, false, false};  // LB LBU
        case 0x21: case 0x25: return {2, false, false};  // LH LHU
        case 0x23: case 0x27: case 0x30: case 0x31: return {4, false, false};  // LW LWU LL LWC1
        case 0x37: case 0x34: case 0x35: return {8, false, false};             // LD LLD LDC1
        case 0x22: case 0x26: return {4, true, false};   // LWL LWR
        case 0x1A: case 0x1B: return {8, true, false};   // LDL LDR
        case 0x28: return {1, false, true};              // SB
        case 0x29: return {2, false, true};              // SH
        case 0x2B: case 0x38: case 0x39: return {4, false, true};  // SW SC SWC1
        case 0x3F: case 0x3C: case 0x3D: return {8, false, true};  // SD SCD SDC1
        case 0x2A: case 0x2E: return {4, true, true};    // SWL SWR
        case 0x2C: case 0x2D: return {8, true, true};    // SDL SDR
        default: return {};
    }
}

Kind classify(std::uint32_t instr) {
    if ((instr >> 26) == 0x10) return Kind::OutOfBlock;  // COP0: MTC0/MFC0, ERET, TLB
    if (memory_access(instr).bytes != 0) return Kind::Memory;
    // Everything else, branches, SYSCALL, BREAK, traps, CACHE and COP1 included, has no
    // effect on the bus: the interpreter runs it in place and raises its exceptions itself.
    return Kind::InPlace;
}

bool is_branch(std::uint32_t instr) {
    switch (instr >> 26) {
        case 0x00: {
            const std::uint32_t funct = instr & 0x3F;
            return funct == 0x08 || funct == 0x09;  // JR JALR
        }
        case 0x01: {  // REGIMM: BLTZ BGEZ BLTZL BGEZL and the -AL forms
            const std::uint32_t rt = (instr >> 16) & 31;
            return rt <= 0x03 || (rt >= 0x10 && rt <= 0x13);
        }
        case 0x02: case 0x03:                          // J JAL
        case 0x04: case 0x05: case 0x06: case 0x07:    // BEQ BNE BLEZ BGTZ
        case 0x14: case 0x15: case 0x16: case 0x17:    // the likely forms
            return true;
        case 0x11: return ((instr >> 21) & 31) == 0x08;  // BC1F BC1T BC1FL BC1TL
        default: return false;
    }
}

}  // namespace ezy::n64
