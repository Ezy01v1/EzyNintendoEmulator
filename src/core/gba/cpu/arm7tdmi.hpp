#pragma once

#include <array>
#include <cstdint>

#include "gba/cpu/alu.hpp"
#include "gba/cpu/cpu_bus.hpp"

namespace ezy::gba {

// Processor modes (CPSR bits 0-4).
enum class Mode : std::uint32_t {
    User = 0x10,
    Fiq = 0x11,
    Irq = 0x12,
    Supervisor = 0x13,
    Abort = 0x17,
    Undefined = 0x1B,
    System = 0x1F,
};

namespace psr {
inline constexpr std::uint32_t kN = 1U << 31;
inline constexpr std::uint32_t kZ = 1U << 30;
inline constexpr std::uint32_t kC = 1U << 29;
inline constexpr std::uint32_t kV = 1U << 28;
inline constexpr std::uint32_t kI = 1U << 7;  // IRQ disabled
inline constexpr std::uint32_t kF = 1U << 6;  // FIQ disabled
inline constexpr std::uint32_t kT = 1U << 5;  // Thumb state
inline constexpr std::uint32_t kModeMask = 0x1F;
}  // namespace psr

// Register banks. Each mode with its own r13/r14 has a slot; User and System share one.
// Invalid mode values also map to the User bank (and, like on NanoBoyAdvance, have no SPSR).
enum Bank : std::size_t { kBankUser, kBankFiq, kBankSvc, kBankAbt, kBankIrq, kBankUnd, kBankCount };

[[nodiscard]] Bank bank_of(std::uint32_t mode);

// Lets the system emulate BIOS calls in C++ (HLE). Called when a SWI executes, with the
// comment field the BIOS would read (ARM: bits 16-23, Thumb: bits 0-7). Return true if
// handled: the SWI then completes like a normal instruction instead of entering
// Supervisor mode at 0x08. The hook may change registers or jump.
class SwiHook {
public:
    virtual ~SwiHook() = default;
    virtual bool on_swi(std::uint8_t number) = 0;
};

// The ARM7TDMI. One step() executes one instruction, ARM or Thumb depending on CPSR.T.
//
// Design:
// - Banking by copying (option A): r_ is always the visible register set; switching mode
//   saves the outgoing r8-r14 into their bank and loads the incoming ones.
// - A real 2-entry pipeline: pipeline_[0] is about to execute, pipeline_[1] was fetched
//   after it, and r15 already points past both. Reading r15 therefore gives +8 (ARM)
//   naturally, and branches just flush() the pipeline.
class Arm7tdmi {
public:
    explicit Arm7tdmi(CpuBus& bus) : bus_(bus) {}

    void reset();  // enter Supervisor at 0x00000000 with IRQ/FIQ masked, like power-on
    void step();   // execute one instruction

    // Takes an IRQ exception now (between instructions): IRQ mode, vector 0x18, LR = the
    // next instruction + 4 in both states, so handlers return with SUBS pc, lr, #4.
    // The caller checks IME/IE/IF; this only checks CPSR.I and returns whether it entered.
    bool irq();
    void set_swi_hook(SwiHook* hook) { swi_hook_ = hook; }
    // Address of the instruction that executes next (pipeline_[0]).
    [[nodiscard]] std::uint32_t next_instruction_address() const { return r_[15] - (thumb() ? 4U : 8U); }

    // --- State access (debugger, tests, BIOS-less boot) ---
    [[nodiscard]] std::uint32_t reg(std::size_t n) const { return r_[n]; }
    void set_reg(std::size_t n, std::uint32_t value) { r_[n] = value; }
    [[nodiscard]] std::uint32_t cpsr() const { return cpsr_; }
    [[nodiscard]] Mode mode() const { return static_cast<Mode>(cpsr_ & psr::kModeMask); }
    [[nodiscard]] bool thumb() const { return (cpsr_ & psr::kT) != 0; }
    // Writes CPSR, switching the register bank if the mode changes (like MSR does).
    void set_cpsr(std::uint32_t value);
    // Writes CPSR without touching the banks: for loading a saved state together with r_.
    void set_cpsr_raw(std::uint32_t value) { cpsr_ = value; }
    // SPSR of a bank (meaningless for kBankUser, which has none).
    [[nodiscard]] std::uint32_t spsr(Bank bank) const { return spsr_[bank]; }
    void set_spsr(Bank bank, std::uint32_t value) { spsr_[bank] = value; }
    // Saved copies of banked registers: r8-r12 (index 0-4) for User/FIQ, r13/r14 (5, 6) for
    // every bank. Only meaningful for banks other than the current one.
    [[nodiscard]] std::uint32_t banked(Bank bank, std::size_t index) const;
    void set_banked(Bank bank, std::size_t index, std::uint32_t value);
    [[nodiscard]] const std::array<std::uint32_t, 2>& pipeline() const { return pipeline_; }
    void set_pipeline(std::uint32_t first, std::uint32_t second) { pipeline_ = {first, second}; }
    // Jumps to an address and refills the pipeline (like a branch).
    void jump(std::uint32_t addr);

    // Save-state contents (state.hpp). The ROM, BIOS and wiring are not part of it.
    template <class Ar>
    void serialize(Ar& ar) {
        ar(r_);
        ar(cpsr_);
        ar(spsr_);
        ar(bank_r8_r12_);
        ar(bank_r13_r14_);
        ar(pipeline_);
        ar(flushed_);
    }

private:
    // --- Core ---
    void switch_mode(std::uint32_t new_mode);
    void flush();  // refill the pipeline from r15 (aligned for the current state)
    void enter_exception(Mode mode, std::uint32_t vector, std::uint32_t return_addr);
    [[nodiscard]] bool condition_passed(std::uint32_t cond) const;
    [[nodiscard]] std::uint32_t& spsr_ref();  // current mode's SPSR (CPSR if it has none)

    void set_nz(std::uint32_t result);
    void set_flag(std::uint32_t flag, bool on) { cpsr_ = on ? cpsr_ | flag : cpsr_ & ~flag; }
    [[nodiscard]] bool flag(std::uint32_t f) const { return (cpsr_ & f) != 0; }

    // --- ARM instructions (arm.cpp) ---
    void execute_arm(std::uint32_t op);
    void arm_branch(std::uint32_t op);
    void arm_bx(std::uint32_t op);
    void arm_data_processing(std::uint32_t op);
    void arm_mrs(std::uint32_t op);
    void arm_msr(std::uint32_t op);
    void arm_multiply(std::uint32_t op);
    void arm_multiply_long(std::uint32_t op);
    void arm_single_transfer(std::uint32_t op);
    void arm_halfword_transfer(std::uint32_t op);
    void arm_block_transfer(std::uint32_t op);
    void arm_swap(std::uint32_t op);
    void arm_swi(std::uint32_t op);
    void arm_undefined(std::uint32_t op);

    // --- Helpers shared by ARM and Thumb (arm.cpp) ---
    void set_cv(const alu::AddResult& sum);
    [[nodiscard]] std::uint32_t load_word(std::uint32_t addr);         // LDR, rotated if misaligned
    [[nodiscard]] std::uint32_t load_half(std::uint32_t addr);         // LDRH, rotated if odd
    [[nodiscard]] std::uint32_t load_signed_byte(std::uint32_t addr);  // LDRSB
    [[nodiscard]] std::uint32_t load_signed_half(std::uint32_t addr);  // LDRSH (LDRSB if odd)
    // LDM/STM, encoded as the ARM instruction. PC is stored as r15 + pc_store_offset.
    void block_transfer(std::uint32_t op, std::uint32_t pc_store_offset);

    // --- Thumb instructions (thumb.cpp) ---
    void execute_thumb(std::uint16_t op);

    CpuBus& bus_;
    std::array<std::uint32_t, 16> r_{};
    std::uint32_t cpsr_ = static_cast<std::uint32_t>(Mode::Supervisor) | psr::kI | psr::kF;
    std::array<std::uint32_t, kBankCount> spsr_{};
    std::array<std::array<std::uint32_t, 5>, 2> bank_r8_r12_{};       // [0]=User/others, [1]=FIQ
    std::array<std::array<std::uint32_t, 2>, kBankCount> bank_r13_r14_{};
    std::array<std::uint32_t, 2> pipeline_{};
    bool flushed_ = false;  // set when the current instruction wrote r15
    SwiHook* swi_hook_ = nullptr;
};

}  // namespace ezy::gba
