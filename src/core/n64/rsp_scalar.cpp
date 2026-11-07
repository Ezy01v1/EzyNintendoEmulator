#include "n64/rsp_regs.hpp"

namespace ezy::n64 {

void RspRegs::step(Mi& mi, Rdp& rdp, Scheduler& scheduler, std::uint64_t when) {
    if (sp_status_ & kHalt) return;

    const std::uint32_t pc = sp_pc_ & kPcMask;
    std::uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) word = (word << 8) | mem_[0x1000 + pc + i];

    sp_pc_ = next_pc_ & kPcMask;
    next_pc_ = (sp_pc_ + 4) & kPcMask;
    execute_scalar(word, pc, mi, rdp, scheduler, when);
    gpr_[0] = 0;

    if (sp_status_ & kSingleStep) sp_status_ |= kHalt;
    if (!(sp_status_ & kHalt)) {
        scheduler.schedule(EventType::RspStep, when + (short_step_ ? 1 : 2));
        short_step_ = !short_step_;
    }
}

std::uint32_t RspRegs::load_dmem(std::uint32_t address, unsigned width) const {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8) | mem_[(address + i) & 0xFFF];
    return value;
}

void RspRegs::store_dmem(std::uint32_t address, unsigned width, std::uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        mem_[(address + i) & 0xFFF] = static_cast<std::uint8_t>(value >> (8 * (width - i - 1)));
    }
}

void RspRegs::execute_scalar(std::uint32_t word, std::uint32_t pc, Mi& mi, Rdp& rdp, Scheduler& scheduler,
                             std::uint64_t when) {
    const unsigned rs = (word >> 21) & 31, rt = (word >> 16) & 31, rd = (word >> 11) & 31;
    const std::uint32_t source = gpr_[rs];
    const auto signed_source = static_cast<std::int32_t>(source);
    std::uint32_t& target = gpr_[rt];
    const std::uint32_t immediate = word & 0xFFFF;
    const auto signed_immediate = static_cast<std::int16_t>(immediate);
    const auto offset = static_cast<std::uint32_t>(signed_immediate);
    const std::uint32_t address = source + offset;
    const std::uint32_t branch_pc = (pc + 4 + offset * 4) & kPcMask;

    switch (word >> 26) {
        case 0x00: execute_special(word, pc, mi); break;
        case 0x01:  // BLTZ, BGEZ and their link forms
            if (rt == 0 || rt == 1 || rt == 16 || rt == 17) {
                if (rt & 16) gpr_[31] = (pc + 8) & kPcMask;
                if ((signed_source >= 0) == ((rt & 1) != 0)) next_pc_ = branch_pc;
            }
            break;
        case 0x03: gpr_[31] = (pc + 8) & kPcMask; [[fallthrough]];  // JAL
        case 0x02: next_pc_ = (word << 2) & kPcMask; break;         // J
        case 0x04: if (source == target) next_pc_ = branch_pc; break;  // BEQ
        case 0x05: if (source != target) next_pc_ = branch_pc; break;  // BNE
        case 0x06: if (signed_source <= 0) next_pc_ = branch_pc; break;  // BLEZ
        case 0x07: if (signed_source > 0) next_pc_ = branch_pc; break;   // BGTZ
        case 0x08: case 0x09: target = address; break;  // ADDI / ADDIU (no overflow trap)
        case 0x0A: target = signed_source < signed_immediate; break;  // SLTI
        case 0x0B: target = source < offset; break;  // SLTIU
        case 0x0C: target = source & immediate; break;  // ANDI
        case 0x0D: target = source | immediate; break;  // ORI
        case 0x0E: target = source ^ immediate; break;  // XORI
        case 0x0F: target = immediate << 16; break;     // LUI
        case 0x10: {  // MFC0 / MTC0 access the same SP and DP registers as the CPU
            const std::uint32_t reg = (rd & 7) * 4;
            if (rs == 0) {
                target = (rd & 8) ? rdp.read(reg, when) : read_sp(reg);
            } else if (rs == 4) {
                if (rd & 8) rdp.write(reg, target, scheduler, when);
                else write_sp(reg, target, mi, scheduler, when);
            }
            break;
        }
        case 0x12: vector_.execute(word, target); break;  // COP2
        case 0x20: target = static_cast<std::uint32_t>(static_cast<std::int8_t>(load_dmem(address, 1))); break;  // LB
        case 0x21: target = static_cast<std::uint32_t>(static_cast<std::int16_t>(load_dmem(address, 2))); break;  // LH
        case 0x23: case 0x27: target = load_dmem(address, 4); break;  // LW / LWU
        case 0x24: target = load_dmem(address, 1); break;  // LBU
        case 0x25: target = load_dmem(address, 2); break;  // LHU
        case 0x28: store_dmem(address, 1, target); break;  // SB
        case 0x29: store_dmem(address, 2, target); break;  // SH
        case 0x2B: store_dmem(address, 4, target); break;  // SW
        case 0x32: case 0x3A:  // LWC2 / SWC2
            vector_.access_memory(word, source, std::span(mem_).first<0x1000>());
            break;
        default: break;
    }
}

void RspRegs::execute_special(std::uint32_t word, std::uint32_t pc, Mi& mi) {
    const std::uint32_t lhs = gpr_[(word >> 21) & 31], rhs = gpr_[(word >> 16) & 31];
    std::uint32_t& dest = gpr_[(word >> 11) & 31];
    const unsigned shift = (word >> 6) & 31, variable_shift = lhs & 31;
    const auto signed_lhs = static_cast<std::int32_t>(lhs), signed_rhs = static_cast<std::int32_t>(rhs);

    switch (word & 63) {
        case 0x00: dest = rhs << shift; break;  // SLL
        case 0x02: dest = rhs >> shift; break;  // SRL
        case 0x03: dest = static_cast<std::uint32_t>(signed_rhs >> shift); break;  // SRA
        case 0x04: dest = rhs << variable_shift; break;  // SLLV
        case 0x06: dest = rhs >> variable_shift; break;  // SRLV
        case 0x07: dest = static_cast<std::uint32_t>(signed_rhs >> variable_shift); break;  // SRAV
        case 0x09: dest = (pc + 8) & kPcMask; [[fallthrough]];  // JALR
        case 0x08: next_pc_ = lhs & kPcMask; break;  // JR
        case 0x0D:  // BREAK
            sp_status_ |= kHalt | kBroke;
            if (sp_status_ & kInterruptOnBreak) mi.raise(MiInterrupt::Sp);
            break;
        case 0x20: case 0x21: dest = lhs + rhs; break;  // ADD / ADDU
        case 0x22: case 0x23: dest = lhs - rhs; break;  // SUB / SUBU
        case 0x24: dest = lhs & rhs; break;  // AND
        case 0x25: dest = lhs | rhs; break;  // OR
        case 0x26: dest = lhs ^ rhs; break;  // XOR
        case 0x27: dest = ~(lhs | rhs); break;  // NOR
        case 0x2A: dest = signed_lhs < signed_rhs; break;  // SLT
        case 0x2B: dest = lhs < rhs; break;  // SLTU
        default: break;
    }
}

}  // namespace ezy::n64
