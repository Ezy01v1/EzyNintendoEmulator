// Thumb (16-bit) instruction set of the ARM7TDMI: GBATEK "THUMB Opcodes", formats 1-19.
// Same registers, ALU, shifter and memory helpers as ARM; only the encoding differs.
// While a Thumb instruction executes, r15 is its address + 4.

#include "gba/cpu/alu.hpp"
#include "gba/cpu/arm7tdmi.hpp"
#include "gba/cpu/shifter.hpp"

namespace ezy::gba {

namespace {

constexpr std::uint32_t bits(std::uint32_t op, std::uint32_t shift, std::uint32_t mask) { return (op >> shift) & mask; }

constexpr std::uint32_t sign_extend(std::uint32_t value, int bit_count) {
    const int shift = 32 - bit_count;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value << shift) >> shift);
}

// Thumb LDM/STM/PUSH/POP reuse the ARM block transfer, re-encoded as the ARM instruction
// (condition AL). Flags: P (pre), U (up), W (writeback), L (load).
constexpr std::uint32_t arm_block(bool pre, bool up, bool load, std::uint32_t rn, std::uint32_t list) {
    return 0xE8000000 | (pre ? 1U << 24 : 0) | (up ? 1U << 23 : 0) | (1U << 21) | (load ? 1U << 20 : 0) |
           (rn << 16) | list;
}

}  // namespace

void Arm7tdmi::execute_thumb(std::uint16_t op16) {
    const std::uint32_t op = op16;
    const std::uint32_t pc = r_[15];

    // Format 1: LSL/LSR/ASR Rd, Rs, #imm5
    if ((op & 0xE000) == 0x0000 && (op & 0x1800) != 0x1800) {
        bool carry = flag(psr::kC);
        const std::uint32_t result = shifter::by_imm(static_cast<shifter::Type>(bits(op, 11, 3)),
                                                     r_[bits(op, 3, 7)], bits(op, 6, 0x1F), carry);
        r_[op & 7] = result;
        set_nz(result);
        set_flag(psr::kC, carry);
        return;
    }

    // Format 2: ADD/SUB Rd, Rs, Rn|#imm3
    if ((op & 0xF800) == 0x1800) {
        const std::uint32_t operand = bits(op, 10, 1) != 0 ? bits(op, 6, 7) : r_[bits(op, 6, 7)];
        const auto sum = bits(op, 9, 1) != 0 ? alu::sub(r_[bits(op, 3, 7)], operand) : alu::add(r_[bits(op, 3, 7)], operand);
        r_[op & 7] = sum.value;
        set_nz(sum.value);
        set_cv(sum);
        return;
    }

    // Format 3: MOV/CMP/ADD/SUB Rd, #imm8
    if ((op & 0xE000) == 0x2000) {
        const std::uint32_t rd = bits(op, 8, 7);
        const std::uint32_t imm = op & 0xFF;
        switch (bits(op, 11, 3)) {
            case 0:
                r_[rd] = imm;
                set_nz(imm);
                break;
            case 1: {
                const auto sum = alu::sub(r_[rd], imm);
                set_nz(sum.value);
                set_cv(sum);
                break;
            }
            default: {
                const auto sum = bits(op, 11, 3) == 2 ? alu::add(r_[rd], imm) : alu::sub(r_[rd], imm);
                r_[rd] = sum.value;
                set_nz(sum.value);
                set_cv(sum);
                break;
            }
        }
        return;
    }

    // Format 4: ALU operations on low registers (flags always set)
    if ((op & 0xFC00) == 0x4000) {
        const std::uint32_t rd = op & 7;
        const std::uint32_t rs = r_[bits(op, 3, 7)];
        const std::uint32_t a = r_[rd];
        bool carry = flag(psr::kC);
        std::uint32_t result = 0;
        bool write = true;
        auto arith = [&](const alu::AddResult& sum) {
            set_cv(sum);
            return sum.value;
        };
        switch (bits(op, 6, 0xF)) {
            case 0x0: result = a & rs; break;                                                     // AND
            case 0x1: result = a ^ rs; break;                                                     // EOR
            case 0x2: bus_.idle(); result = shifter::by_reg(shifter::Type::Lsl, a, rs, carry); break;  // LSL
            case 0x3: bus_.idle(); result = shifter::by_reg(shifter::Type::Lsr, a, rs, carry); break;  // LSR
            case 0x4: bus_.idle(); result = shifter::by_reg(shifter::Type::Asr, a, rs, carry); break;  // ASR
            case 0x5: result = arith(alu::add_with_carry(a, rs, flag(psr::kC))); break;           // ADC
            case 0x6: result = arith(alu::add_with_carry(a, ~rs, flag(psr::kC))); break;          // SBC
            case 0x7: bus_.idle(); result = shifter::by_reg(shifter::Type::Ror, a, rs, carry); break;  // ROR
            case 0x8: result = a & rs; write = false; break;                                      // TST
            case 0x9: result = arith(alu::sub(0, rs)); break;                                     // NEG
            case 0xA: result = arith(alu::sub(a, rs)); write = false; break;                      // CMP
            case 0xB: result = arith(alu::add(a, rs)); write = false; break;                      // CMN
            case 0xC: result = a | rs; break;                                                     // ORR
            case 0xD: result = a * rs; break;  // MUL (C from the Booth multiplier: not modelled)
            case 0xE: result = a & ~rs; break;                                                    // BIC
            default: result = ~rs; break;                                                         // MVN
        }
        const std::uint32_t alu_op = bits(op, 6, 0xF);
        if (alu_op == 0x2 || alu_op == 0x3 || alu_op == 0x4 || alu_op == 0x7) set_flag(psr::kC, carry);
        set_nz(result);
        if (write) r_[rd] = result;
        return;
    }

    // Format 5: hi register ADD/CMP/MOV, and BX
    if ((op & 0xFC00) == 0x4400) {
        const std::uint32_t rd = (op & 7) | (bits(op, 7, 1) << 3);
        const std::uint32_t rs = bits(op, 3, 0xF);
        switch (bits(op, 8, 3)) {
            case 0:  // ADD (no flags)
                r_[rd] += r_[rs];
                if (rd == 15) jump(r_[15] & ~1U);
                break;
            case 1: {  // CMP
                const auto sum = alu::sub(r_[rd], r_[rs]);
                set_nz(sum.value);
                set_cv(sum);
                break;
            }
            case 2:  // MOV (no flags)
                r_[rd] = r_[rs];
                if (rd == 15) jump(r_[15] & ~1U);
                break;
            default: {  // BX
                const std::uint32_t target = r_[rs];
                set_flag(psr::kT, (target & 1) != 0);
                jump(target & ~1U);
                break;
            }
        }
        return;
    }

    // Format 6: LDR Rd, [PC, #imm8*4]  (PC word-aligned)
    if ((op & 0xF800) == 0x4800) {
        r_[bits(op, 8, 7)] = load_word((pc & ~2U) + (op & 0xFF) * 4);
        bus_.idle();
        return;
    }

    // Format 7: LDR/STR/LDRB/STRB Rd, [Rb, Ro]   Format 8: STRH/LDSB/LDRH/LDSH Rd, [Rb, Ro]
    if ((op & 0xF000) == 0x5000) {
        const std::uint32_t rd = op & 7;
        const std::uint32_t addr = r_[bits(op, 3, 7)] + r_[bits(op, 6, 7)];
        const std::uint32_t kind = bits(op, 10, 3);
        if (bits(op, 9, 1) == 0) {
            switch (kind) {
                case 0: bus_.write32(addr, r_[rd], kNonseq); break;
                case 1: bus_.write8(addr, static_cast<std::uint8_t>(r_[rd]), kNonseq); break;
                case 2: r_[rd] = load_word(addr); bus_.idle(); break;
                default: r_[rd] = bus_.read8(addr, kNonseq); bus_.idle(); break;
            }
        } else {
            switch (kind) {
                case 0: bus_.write16(addr, static_cast<std::uint16_t>(r_[rd]), kNonseq); break;
                case 1: r_[rd] = load_signed_byte(addr); bus_.idle(); break;
                case 2: r_[rd] = load_half(addr); bus_.idle(); break;
                default: r_[rd] = load_signed_half(addr); bus_.idle(); break;
            }
        }
        return;
    }

    // Format 9: LDR/STR/LDRB/STRB Rd, [Rb, #imm5]  (word offsets scaled by 4)
    if ((op & 0xE000) == 0x6000) {
        const std::uint32_t rd = op & 7;
        const bool byte = bits(op, 12, 1) != 0;
        const std::uint32_t addr = r_[bits(op, 3, 7)] + bits(op, 6, 0x1F) * (byte ? 1 : 4);
        if (bits(op, 11, 1) != 0) {
            r_[rd] = byte ? bus_.read8(addr, kNonseq) : load_word(addr);
            bus_.idle();
        } else if (byte) {
            bus_.write8(addr, static_cast<std::uint8_t>(r_[rd]), kNonseq);
        } else {
            bus_.write32(addr, r_[rd], kNonseq);
        }
        return;
    }

    // Format 10: LDRH/STRH Rd, [Rb, #imm5*2]
    if ((op & 0xF000) == 0x8000) {
        const std::uint32_t rd = op & 7;
        const std::uint32_t addr = r_[bits(op, 3, 7)] + bits(op, 6, 0x1F) * 2;
        if (bits(op, 11, 1) != 0) {
            r_[rd] = load_half(addr);
            bus_.idle();
        } else {
            bus_.write16(addr, static_cast<std::uint16_t>(r_[rd]), kNonseq);
        }
        return;
    }

    // Format 11: LDR/STR Rd, [SP, #imm8*4]
    if ((op & 0xF000) == 0x9000) {
        const std::uint32_t rd = bits(op, 8, 7);
        const std::uint32_t addr = r_[13] + (op & 0xFF) * 4;
        if (bits(op, 11, 1) != 0) {
            r_[rd] = load_word(addr);
            bus_.idle();
        } else {
            bus_.write32(addr, r_[rd], kNonseq);
        }
        return;
    }

    // Format 12: ADD Rd, PC|SP, #imm8*4
    if ((op & 0xF000) == 0xA000) {
        const std::uint32_t base = bits(op, 11, 1) != 0 ? r_[13] : (pc & ~2U);
        r_[bits(op, 8, 7)] = base + (op & 0xFF) * 4;
        return;
    }

    // Format 13: ADD SP, #+-imm7*4
    if ((op & 0xFF00) == 0xB000) {
        const std::uint32_t offset = (op & 0x7F) * 4;
        r_[13] = bits(op, 7, 1) != 0 ? r_[13] - offset : r_[13] + offset;
        return;
    }

    // Format 14: PUSH {rlist[, lr]} = STMDB sp!   POP {rlist[, pc]} = LDMIA sp!
    if ((op & 0xF600) == 0xB400) {
        const bool pop = bits(op, 11, 1) != 0;
        std::uint32_t list = op & 0xFF;
        if (bits(op, 8, 1) != 0) list |= pop ? 0x8000 : 0x4000;
        block_transfer(pop ? arm_block(false, true, true, 13, list) : arm_block(true, false, false, 13, list), 2);
        return;
    }

    // Format 15: LDMIA/STMIA Rb!, {rlist}
    if ((op & 0xF000) == 0xC000) {
        block_transfer(arm_block(false, true, bits(op, 11, 1) != 0, bits(op, 8, 7), op & 0xFF), 2);
        return;
    }

    // Format 17: SWI   (before format 16, whose condition 0xF it occupies)
    if ((op & 0xFF00) == 0xDF00) {
        enter_exception(Mode::Supervisor, 0x08, pc - 2);
        return;
    }

    // Format 16: B<cond> label. Condition 0xE is documented as undefined, but the
    // ARM7TDMI (and NanoBoyAdvance) simply treat it as AL.
    if ((op & 0xF000) == 0xD000) {
        if (condition_passed(bits(op, 8, 0xF))) jump(pc + sign_extend(op & 0xFF, 8) * 2);
        return;
    }

    // Format 18: B label
    if ((op & 0xF800) == 0xE000) {
        jump(pc + sign_extend(op & 0x7FF, 11) * 2);
        return;
    }

    // Format 19: BL label, as two instructions
    if ((op & 0xF000) == 0xF000) {
        if (bits(op, 11, 1) == 0) {
            r_[14] = pc + (sign_extend(op & 0x7FF, 11) << 12);  // first half: upper offset into LR
        } else {
            const std::uint32_t return_addr = (pc - 2) | 1;     // second half: jump, LR = next | 1
            const std::uint32_t target = r_[14] + ((op & 0x7FF) << 1);
            r_[14] = return_addr;
            jump(target & ~1U);
        }
        return;
    }

    // 0xE800-0xEFFF: BLX suffix on ARMv5, undefined on the ARM7TDMI.
    enter_exception(Mode::Undefined, 0x04, pc - 2);
}

}  // namespace ezy::gba
