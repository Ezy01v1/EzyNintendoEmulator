// ARM (32-bit) instruction set of the ARM7TDMI. GBATEK "ARM Opcodes" describes each group.

#include <bit>

#include "gba/bus.hpp"  // rotate_misaligned_word
#include "gba/cpu/arm7tdmi.hpp"
#include "gba/cpu/shifter.hpp"

namespace ezy::gba {

namespace {

constexpr std::uint32_t bit(std::uint32_t op, std::uint32_t n) { return (op >> n) & 1; }
constexpr std::uint32_t field(std::uint32_t op, std::uint32_t shift, std::uint32_t mask) { return (op >> shift) & mask; }

struct AddResult {
    std::uint32_t value;
    bool carry;
    bool overflow;
};

// a + b + carry_in, with ARM carry/overflow. Subtraction is a + ~b + 1 (carry = no borrow).
constexpr AddResult add_with_carry(std::uint32_t a, std::uint32_t b, bool carry_in) {
    const std::uint64_t wide = std::uint64_t{a} + b + (carry_in ? 1 : 0);
    const auto value = static_cast<std::uint32_t>(wide);
    return {value, (wide >> 32) != 0, ((~(a ^ b) & (a ^ value)) >> 31) != 0};
}

constexpr std::uint32_t rotated_immediate(std::uint32_t op) {
    return std::rotr(op & 0xFF, static_cast<int>(field(op, 8, 0xF) * 2));
}

enum DataOp : std::uint32_t {
    kAnd, kEor, kSub, kRsb, kAdd, kAdc, kSbc, kRsc, kTst, kTeq, kCmp, kCmn, kOrr, kMov, kBic, kMvn,
};

constexpr bool is_test(std::uint32_t opcode) { return opcode >= kTst && opcode <= kCmn; }

}  // namespace

void Arm7tdmi::execute_arm(std::uint32_t op) {
    // Order matters: the multiply/swap/halfword encodings sit inside the data processing
    // space (bits 27-26 = 00) and are told apart by bits 7-4.
    if ((op & 0x0FFFFFF0) == 0x012FFF10) return arm_bx(op);
    if ((op & 0x0FC000F0) == 0x00000090) return arm_multiply(op);
    if ((op & 0x0F8000F0) == 0x00800090) return arm_multiply_long(op);
    if ((op & 0x0FB00FF0) == 0x01000090) return arm_swap(op);
    if ((op & 0x0E000090) == 0x00000090 && (op & 0x60) != 0) return arm_halfword_transfer(op);
    if ((op & 0x0C000000) == 0x00000000) {
        // TST/TEQ/CMP/CMN without S are the PSR transfer instructions.
        if (is_test(field(op, 21, 0xF)) && bit(op, 20) == 0) {
            return bit(op, 21) != 0 ? arm_msr(op) : arm_mrs(op);
        }
        return arm_data_processing(op);
    }
    if ((op & 0x0E000010) == 0x06000010) return arm_undefined(op);
    if ((op & 0x0C000000) == 0x04000000) return arm_single_transfer(op);
    if ((op & 0x0E000000) == 0x08000000) return arm_block_transfer(op);
    if ((op & 0x0E000000) == 0x0A000000) return arm_branch(op);
    if ((op & 0x0F000000) == 0x0F000000) return arm_swi(op);
    return arm_undefined(op);  // coprocessor instructions: the GBA has no coprocessor
}

// --- Branches ---

void Arm7tdmi::arm_branch(std::uint32_t op) {
    // 24-bit signed word offset, relative to PC (+8).
    const auto offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(op << 8) >> 6);
    if (bit(op, 24) != 0) r_[14] = r_[15] - 4;  // BL: return to the next instruction
    jump(r_[15] + offset);
}

void Arm7tdmi::arm_bx(std::uint32_t op) {
    const std::uint32_t target = r_[op & 0xF];
    set_flag(psr::kT, (target & 1) != 0);
    jump(target & ~1U);
}

// --- Data processing ---

void Arm7tdmi::arm_data_processing(std::uint32_t op) {
    const std::uint32_t opcode = field(op, 21, 0xF);
    const bool set_flags = bit(op, 20) != 0;
    const std::uint32_t rn = field(op, 16, 0xF);
    const std::uint32_t rd = field(op, 12, 0xF);
    bool shifter_carry = flag(psr::kC);

    std::uint32_t operand2 = 0;
    bool register_shift = false;
    if (bit(op, 25) != 0) {
        operand2 = rotated_immediate(op);
        if (field(op, 8, 0xF) != 0) shifter_carry = (operand2 >> 31) != 0;
    } else {
        const auto type = static_cast<shifter::Type>(field(op, 5, 3));
        const std::uint32_t rm = op & 0xF;
        if (bit(op, 4) != 0) {
            // Shift by register takes an extra internal cycle, during which PC moves on:
            // reading r15 as an operand then gives +12 instead of +8.
            register_shift = true;
            bus_.idle();
            const std::uint32_t value = r_[rm] + (rm == 15 ? 4 : 0);
            operand2 = shifter::by_reg(type, value, r_[field(op, 8, 0xF)], shifter_carry);
        } else {
            operand2 = shifter::by_imm(type, r_[rm], field(op, 7, 0x1F), shifter_carry);
        }
    }
    const std::uint32_t operand1 = r_[rn] + (rn == 15 && register_shift ? 4 : 0);

    std::uint32_t result = 0;
    bool logical = true;
    AddResult sum{};
    const bool c = flag(psr::kC);
    switch (opcode) {
        case kAnd: case kTst: result = operand1 & operand2; break;
        case kEor: case kTeq: result = operand1 ^ operand2; break;
        case kOrr: result = operand1 | operand2; break;
        case kMov: result = operand2; break;
        case kBic: result = operand1 & ~operand2; break;
        case kMvn: result = ~operand2; break;
        default:
            logical = false;
            switch (opcode) {
                case kSub: case kCmp: sum = add_with_carry(operand1, ~operand2, true); break;
                case kRsb: sum = add_with_carry(operand2, ~operand1, true); break;
                case kAdd: case kCmn: sum = add_with_carry(operand1, operand2, false); break;
                case kAdc: sum = add_with_carry(operand1, operand2, c); break;
                case kSbc: sum = add_with_carry(operand1, ~operand2, c); break;
                default: sum = add_with_carry(operand2, ~operand1, c); break;  // RSC
            }
            result = sum.value;
            break;
    }

    if (set_flags) {
        if (rd == 15 && bank_of(cpsr_) != kBankUser) {
            // S with PC as destination returns from an exception: CPSR = SPSR. (In User and
            // System mode there is no SPSR and the flags are set as usual.)
            set_cpsr(spsr_[bank_of(cpsr_)]);
        } else {
            set_nz(result);
            if (logical) {
                set_flag(psr::kC, shifter_carry);
            } else {
                set_flag(psr::kC, sum.carry);
                set_flag(psr::kV, sum.overflow);
            }
        }
    }
    if (!is_test(opcode)) {
        r_[rd] = result;
        if (rd == 15) flush();
    }
}

// --- PSR transfer ---

void Arm7tdmi::arm_mrs(std::uint32_t op) {
    r_[field(op, 12, 0xF)] = bit(op, 22) != 0 ? spsr_ref() : cpsr_;
}

void Arm7tdmi::arm_msr(std::uint32_t op) {
    const std::uint32_t value = bit(op, 25) != 0 ? rotated_immediate(op) : r_[op & 0xF];
    std::uint32_t mask = 0;
    if (bit(op, 16) != 0) mask |= 0x000000FF;  // c: control (mode, T, I, F)
    if (bit(op, 17) != 0) mask |= 0x0000FF00;  // x
    if (bit(op, 18) != 0) mask |= 0x00FF0000;  // s
    if (bit(op, 19) != 0) mask |= 0xFF000000;  // f: flags

    if (bit(op, 22) != 0) {
        const Bank bank = bank_of(cpsr_);
        if (bank == kBankUser) return;  // User/System have no SPSR
        spsr_[bank] = (spsr_[bank] & ~mask) | (value & mask);
        return;
    }
    if (mode() == Mode::User) mask &= 0xFF000000;  // User mode may only change the flags
    std::uint32_t new_cpsr = (cpsr_ & ~mask) | (value & mask);
    if ((mask & 0xFF) != 0) new_cpsr |= 0x10;  // mode bit 4 is hardwired to 1
    if ((mask & 0xFF) != 0) switch_mode(new_cpsr);
    cpsr_ = new_cpsr;
}

// --- Multiply ---

void Arm7tdmi::arm_multiply(std::uint32_t op) {
    const std::uint32_t rd = field(op, 16, 0xF);
    const auto read = [&](std::uint32_t n) { return r_[n] + (n == 15 ? 4 : 0); };  // PC: +12
    std::uint32_t result = read(op & 0xF) * read(field(op, 8, 0xF));
    if (bit(op, 21) != 0) {  // MLA: the accumulate is read a cycle later (r15 = +12)
        const std::uint32_t rn = field(op, 12, 0xF);
        result += r_[rn] + (rn == 15 ? 4 : 0);
    }
    r_[rd] = result;
    // C gets an implementation-defined value from the Booth multiplier's last step; we
    // leave it unchanged (games don't rely on it). SingleStepTests checks it, so some
    // MUL/MULL cases there fail on C alone.
    if (bit(op, 20) != 0) set_nz(result);
}

void Arm7tdmi::arm_multiply_long(std::uint32_t op) {
    const std::uint32_t rd_hi = field(op, 16, 0xF);
    const std::uint32_t rd_lo = field(op, 12, 0xF);
    const std::uint32_t rm = r_[op & 0xF];
    const std::uint32_t rs = r_[field(op, 8, 0xF)];
    std::uint64_t result = 0;
    if (bit(op, 22) != 0) {  // signed
        result = static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(rm)} *
                                            std::int64_t{static_cast<std::int32_t>(rs)});
    } else {
        result = std::uint64_t{rm} * rs;
    }
    if (bit(op, 21) != 0) result += (std::uint64_t{r_[rd_hi]} << 32) | r_[rd_lo];  // accumulate
    r_[rd_lo] = static_cast<std::uint32_t>(result);
    r_[rd_hi] = static_cast<std::uint32_t>(result >> 32);
    if (bit(op, 20) != 0) {
        set_flag(psr::kN, (result >> 63) != 0);
        set_flag(psr::kZ, result == 0);
    }
}

// --- Memory ---

void Arm7tdmi::arm_single_transfer(std::uint32_t op) {
    const bool pre = bit(op, 24) != 0;
    const bool up = bit(op, 23) != 0;
    const bool byte = bit(op, 22) != 0;
    const bool writeback = bit(op, 21) != 0 || !pre;  // post-indexing always writes back
    const bool load = bit(op, 20) != 0;
    const std::uint32_t rn = field(op, 16, 0xF);
    const std::uint32_t rd = field(op, 12, 0xF);

    std::uint32_t offset = op & 0xFFF;
    if (bit(op, 25) != 0) {
        bool unused_carry = flag(psr::kC);
        offset = shifter::by_imm(static_cast<shifter::Type>(field(op, 5, 3)), r_[op & 0xF], field(op, 7, 0x1F),
                                 unused_carry);
    }
    const std::uint32_t base = r_[rn];
    const std::uint32_t moved = up ? base + offset : base - offset;
    const std::uint32_t addr = pre ? moved : base;

    if (load) {
        const std::uint32_t value =
            byte ? bus_.read8(addr, kNonseq) : rotate_misaligned_word(bus_.read32(addr, kNonseq), addr);
        bus_.idle();
        if (writeback) r_[rn] = moved + (rn == 15 ? 4 : 0);  // PC has advanced by now
        r_[rd] = value;  // the loaded value wins over writeback when rd == rn
        if (rd == 15 || (writeback && rn == 15)) flush();
    } else {
        const std::uint32_t value = r_[rd] + (rd == 15 ? 4 : 0);  // PC is stored as +12
        if (byte) {
            bus_.write8(addr, static_cast<std::uint8_t>(value), kNonseq);
        } else {
            bus_.write32(addr, value, kNonseq);
        }
        if (writeback) {
            // The PC has already advanced by the time a store writes the base back.
            r_[rn] = moved + (rn == 15 ? 4 : 0);
            if (rn == 15) flush();
        }
    }
}

void Arm7tdmi::arm_halfword_transfer(std::uint32_t op) {
    const bool pre = bit(op, 24) != 0;
    const bool up = bit(op, 23) != 0;
    const bool writeback = bit(op, 21) != 0 || !pre;
    const bool load = bit(op, 20) != 0;
    const std::uint32_t rn = field(op, 16, 0xF);
    const std::uint32_t rd = field(op, 12, 0xF);
    const std::uint32_t kind = field(op, 5, 3);  // 1: H, 2: SB, 3: SH

    const std::uint32_t offset = bit(op, 22) != 0 ? (field(op, 8, 0xF) << 4) | (op & 0xF) : r_[op & 0xF];
    const std::uint32_t base = r_[rn];
    const std::uint32_t moved = up ? base + offset : base - offset;
    const std::uint32_t addr = pre ? moved : base;

    if (load) {
        std::uint32_t value = 0;
        if (kind == 1) {
            // LDRH from an odd address: the halfword is rotated, like LDR.
            value = std::rotr(std::uint32_t{bus_.read16(addr, kNonseq)}, static_cast<int>((addr & 1) * 8));
        } else if (kind == 2) {
            value = static_cast<std::uint32_t>(static_cast<std::int8_t>(bus_.read8(addr, kNonseq)));
        } else if ((addr & 1) != 0) {
            // LDRSH from an odd address: a halfword access whose upper byte (the addressed
            // one) is sign-extended, as if it were LDRSB.
            const auto half = bus_.read16(addr, kNonseq);
            value = static_cast<std::uint32_t>(static_cast<std::int8_t>(half >> 8));
        } else {
            value = static_cast<std::uint32_t>(static_cast<std::int16_t>(bus_.read16(addr, kNonseq)));
        }
        bus_.idle();
        if (writeback) r_[rn] = moved + (rn == 15 ? 4 : 0);  // PC has advanced by now
        r_[rd] = value;
        if (rd == 15 || (writeback && rn == 15)) flush();
        return;
    }
    if (kind == 1) {
        const std::uint32_t value = r_[rd] + (rd == 15 ? 4 : 0);
        bus_.write16(addr, static_cast<std::uint16_t>(value), kNonseq);
    }
    // kind 2/3 without L are LDRD/STRD on ARMv5; the ARMv4 ARM7TDMI transfers nothing.
    if (writeback) {
        r_[rn] = moved + (rn == 15 ? 4 : 0);
        if (rn == 15) flush();
    }
}

void Arm7tdmi::arm_block_transfer(std::uint32_t op) {
    const bool pre = bit(op, 24) != 0;
    const bool up = bit(op, 23) != 0;
    const bool psr_or_user = bit(op, 22) != 0;
    const bool writeback = bit(op, 21) != 0;
    const bool load = bit(op, 20) != 0;
    const std::uint32_t rn = field(op, 16, 0xF);
    std::uint32_t list = op & 0xFFFF;

    // An empty list transfers only r15 but moves the base as if all 16 registers had been.
    std::uint32_t bytes = static_cast<std::uint32_t>(std::popcount(list)) * 4;
    if (list == 0) {
        list = 1U << 15;
        bytes = 0x40;
    }
    const std::uint32_t base = r_[rn];
    const std::uint32_t new_base = up ? base + bytes : base - bytes;
    // Registers always go to ascending addresses, lowest register first.
    std::uint32_t addr = up ? (pre ? base + 4 : base) : (pre ? base - bytes : base - bytes + 4);

    // ^ without loading PC: transfer the User bank registers instead of the current ones.
    const bool pc_in_list = (list & 0x8000) != 0;
    const bool user_bank = psr_or_user && !(load && pc_in_list);
    const std::uint32_t saved_mode = cpsr_ & psr::kModeMask;
    if (user_bank) switch_mode(static_cast<std::uint32_t>(Mode::User));

    bool first = true;
    Access access = kNonseq;
    for (std::uint32_t i = 0; i < 16; ++i) {
        if ((list & (1U << i)) == 0) continue;
        if (load) {
            const std::uint32_t value = bus_.read32(addr, access);
            // The base is written back after the first access, so a loaded base wins.
            if (first && writeback) r_[rn] = new_base;
            r_[i] = value;
        } else {
            // A base stored after the first register is already the written-back value.
            bus_.write32(addr, r_[i] + (i == 15 ? 4 : 0), access);
            if (first && writeback) r_[rn] = new_base;
        }
        first = false;
        access = kSeq;
        addr += 4;
    }

    if (user_bank) switch_mode(saved_mode);
    // Writing the base back into r15 is a jump (unless a loaded r15 replaced it).
    if (writeback && rn == 15 && !(load && pc_in_list)) {
        r_[15] = new_base;
        flush();
    }
    if (load) {
        bus_.idle();
        if (pc_in_list) {
            if (psr_or_user) set_cpsr(spsr_ref());  // LDM ..., {.., pc}^ returns from an exception
            flush();
        }
    }
}

void Arm7tdmi::arm_swap(std::uint32_t op) {
    const std::uint32_t rn = field(op, 16, 0xF);
    const std::uint32_t addr = r_[rn] + (rn == 15 ? 4 : 0);
    const std::uint32_t source = r_[op & 0xF] + ((op & 0xF) == 15 ? 4 : 0);
    const std::uint32_t rd = field(op, 12, 0xF);
    std::uint32_t old = 0;
    if (bit(op, 22) != 0) {
        old = bus_.read8(addr, kNonseq);
        bus_.write8(addr, static_cast<std::uint8_t>(source), kLock);
    } else {
        old = rotate_misaligned_word(bus_.read32(addr, kNonseq), addr);
        bus_.write32(addr, source, kLock);
    }
    bus_.idle();
    r_[rd] = old;
    if (rd == 15) flush();
}

// --- Exceptions ---

void Arm7tdmi::arm_swi(std::uint32_t /*op*/) { enter_exception(Mode::Supervisor, 0x08, r_[15] - 4); }

void Arm7tdmi::arm_undefined(std::uint32_t /*op*/) { enter_exception(Mode::Undefined, 0x04, r_[15] - 4); }

}  // namespace ezy::gba
