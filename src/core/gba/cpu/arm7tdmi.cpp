#include "gba/cpu/arm7tdmi.hpp"

namespace ezy::gba {

namespace {

// condition_table[cond] has bit `nzcv` set when the condition passes for those flags.
constexpr std::array<std::uint16_t, 16> make_condition_table() {
    std::array<std::uint16_t, 16> table{};
    for (std::uint32_t nzcv = 0; nzcv < 16; ++nzcv) {
        const bool n = (nzcv & 8) != 0;
        const bool z = (nzcv & 4) != 0;
        const bool c = (nzcv & 2) != 0;
        const bool v = (nzcv & 1) != 0;
        const std::array<bool, 16> passes = {
            z,               // EQ
            !z,              // NE
            c,               // CS
            !c,              // CC
            n,               // MI
            !n,              // PL
            v,               // VS
            !v,              // VC
            c && !z,         // HI
            !c || z,         // LS
            n == v,          // GE
            n != v,          // LT
            !z && n == v,    // GT
            z || n != v,     // LE
            true,            // AL
            false,           // NV: never (reserved on ARMv4)
        };
        for (std::size_t cond = 0; cond < 16; ++cond) {
            if (passes[cond]) table[cond] = static_cast<std::uint16_t>(table[cond] | (1U << nzcv));
        }
    }
    return table;
}

constexpr auto kConditionTable = make_condition_table();

}  // namespace

Bank bank_of(std::uint32_t mode) {
    switch (static_cast<Mode>(mode & psr::kModeMask)) {
        case Mode::Fiq: return kBankFiq;
        case Mode::Irq: return kBankIrq;
        case Mode::Supervisor: return kBankSvc;
        case Mode::Abort: return kBankAbt;
        case Mode::Undefined: return kBankUnd;
        default: return kBankUser;  // User, System and invalid modes
    }
}

void Arm7tdmi::reset() {
    switch_mode(static_cast<std::uint32_t>(Mode::Supervisor));
    cpsr_ = static_cast<std::uint32_t>(Mode::Supervisor) | psr::kI | psr::kF;
    jump(0);
}

void Arm7tdmi::jump(std::uint32_t addr) {
    r_[15] = addr;
    flush();
}

void Arm7tdmi::step() {
    flushed_ = false;
    if (thumb()) {
        const auto op = static_cast<std::uint16_t>(pipeline_[0]);
        pipeline_[0] = pipeline_[1];
        pipeline_[1] = bus_.read16(r_[15] & ~1U, static_cast<Access>(kCode | kSeq));
        execute_thumb(op);
        if (!flushed_) r_[15] += 2;
        return;
    }

    const std::uint32_t op = pipeline_[0];
    pipeline_[0] = pipeline_[1];
    pipeline_[1] = bus_.read32(r_[15] & ~3U, static_cast<Access>(kCode | kSeq));
    if (condition_passed(op >> 28)) execute_arm(op);
    if (!flushed_) r_[15] += 4;
}

bool Arm7tdmi::condition_passed(std::uint32_t cond) const {
    return ((kConditionTable[cond & 0xF] >> (cpsr_ >> 28)) & 1) != 0;
}

void Arm7tdmi::flush() {
    // r15 keeps whatever low bits were written; only the fetch addresses are aligned. The
    // instructions that clear bit 0 (BX, Thumb BL/POP/hi-register writes) do so themselves.
    // This matches NanoBoyAdvance and the SingleStepTests.
    if (thumb()) {
        pipeline_[0] = bus_.read16(r_[15] & ~1U, kCode);
        pipeline_[1] = bus_.read16((r_[15] & ~1U) + 2, static_cast<Access>(kCode | kSeq));
        r_[15] += 4;
    } else {
        pipeline_[0] = bus_.read32(r_[15] & ~3U, kCode);
        pipeline_[1] = bus_.read32((r_[15] & ~3U) + 4, static_cast<Access>(kCode | kSeq));
        r_[15] += 8;
    }
    flushed_ = true;
}

void Arm7tdmi::switch_mode(std::uint32_t new_mode) {
    const Bank old_bank = bank_of(cpsr_);
    const Bank new_bank = bank_of(new_mode);
    cpsr_ = (cpsr_ & ~psr::kModeMask) | (new_mode & psr::kModeMask);
    if (old_bank == new_bank) return;

    const std::size_t old_fiq = old_bank == kBankFiq ? 1 : 0;
    const std::size_t new_fiq = new_bank == kBankFiq ? 1 : 0;
    if (old_fiq != new_fiq) {
        for (std::size_t i = 0; i < 5; ++i) {
            bank_r8_r12_[old_fiq][i] = r_[8 + i];
            r_[8 + i] = bank_r8_r12_[new_fiq][i];
        }
    }
    bank_r13_r14_[old_bank] = {r_[13], r_[14]};
    r_[13] = bank_r13_r14_[new_bank][0];
    r_[14] = bank_r13_r14_[new_bank][1];
}

void Arm7tdmi::set_cpsr(std::uint32_t value) {
    switch_mode(value);
    cpsr_ = value;
}

std::uint32_t& Arm7tdmi::spsr_ref() {
    const Bank bank = bank_of(cpsr_);
    return bank == kBankUser ? cpsr_ : spsr_[bank];
}

std::uint32_t Arm7tdmi::banked(Bank bank, std::size_t index) const {
    if (index < 5) return bank_r8_r12_[bank == kBankFiq ? 1 : 0][index];
    return bank_r13_r14_[bank][index - 5];
}

void Arm7tdmi::set_banked(Bank bank, std::size_t index, std::uint32_t value) {
    if (index < 5) {
        bank_r8_r12_[bank == kBankFiq ? 1 : 0][index] = value;
    } else {
        bank_r13_r14_[bank][index - 5] = value;
    }
}

void Arm7tdmi::enter_exception(Mode mode, std::uint32_t vector, std::uint32_t return_addr) {
    const std::uint32_t old_cpsr = cpsr_;
    switch_mode(static_cast<std::uint32_t>(mode));
    spsr_[bank_of(cpsr_)] = old_cpsr;
    r_[14] = return_addr;
    cpsr_ = (cpsr_ & ~psr::kT) | psr::kI;
    if (mode == Mode::Fiq) cpsr_ |= psr::kF;
    jump(vector);
}

bool Arm7tdmi::irq() {
    if (flag(psr::kI)) return false;
    // r15 is next + 8 (ARM) or next + 4 (Thumb); LR must be next + 4 either way.
    const std::uint32_t return_addr = thumb() ? r_[15] : r_[15] - 4;
    enter_exception(Mode::Irq, 0x18, return_addr);
    return true;
}

void Arm7tdmi::set_nz(std::uint32_t result) {
    set_flag(psr::kN, (result & 0x80000000U) != 0);
    set_flag(psr::kZ, result == 0);
}

}  // namespace ezy::gba
