#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "gc/gekko/cpu_bus.hpp"

namespace ezy::gc {

// The emulator stops with this instead of guessing: an unimplemented instruction or an
// unknown SPR (word_or_address = the instruction word), an address with nothing behind it
// (word_or_address = the address).
struct GekkoError : std::runtime_error {
    GekkoError(const std::string& what, std::uint32_t pc_, std::uint32_t word_or_address_);
    std::uint32_t pc;
    std::uint32_t word_or_address;
};

// Special-purpose register numbers (as mfspr/mtspr encode them, halves already swapped).
namespace spr {
inline constexpr int kXer = 1, kLr = 8, kCtr = 9, kDsisr = 18, kDar = 19, kDec = 22, kSrr0 = 26, kSrr1 = 27;
inline constexpr int kTbl = 268, kTbu = 269;  // time base reads (mftb)
inline constexpr int kSprg0 = 272;            // SPRG0..3 = 272..275
inline constexpr int kTblWrite = 284, kTbuWrite = 285, kPvr = 287;
inline constexpr int kIbat0u = 528;  // IBAT0U, IBAT0L, IBAT1U ... IBAT3L = 528..535
inline constexpr int kDbat0u = 536;  // DBAT0U ... DBAT3L = 536..543
inline constexpr int kIbat0l = kIbat0u + 1, kDbat0l = kDbat0u + 1, kDbat1u = kDbat0u + 2, kDbat1l = kDbat0u + 3;
inline constexpr int kGqr0 = 912;  // GQR0..7 = 912..919
inline constexpr int kHid2 = 920, kWpar = 921, kDmaU = 922, kDmaL = 923;
inline constexpr int kHid0 = 1008, kHid1 = 1009, kHid4 = 1011, kL2cr = 1017;
}  // namespace spr

// One floating-point register: the Gekko's FPRs hold two halves. Double-precision
// instructions use ps0 only; single-precision ones write their result to both; the paired
// singles work on both lanes. Kept as host doubles whose bits are the register's bits (NaN
// payloads included: values are only copied, never converted, unless an instruction says so).
struct PairedSingle {
    double ps0 = 0.0;
    double ps1 = 0.0;
};

// The architected state, kept in one plain struct so a future JIT can address it directly.
struct GekkoState {
    std::array<std::uint32_t, 32> gpr{};
    std::array<PairedSingle, 32> fpr{};
    std::uint32_t fpscr = 0;
    std::uint32_t pc = 0;   // the instruction executing (between steps: the next to execute)
    std::uint32_t npc = 0;  // while an instruction executes: the one after it
    std::uint32_t cr = 0, xer = 0, lr = 0, ctr = 0, msr = 0;
    std::uint32_t srr0 = 0, srr1 = 0, dar = 0, dsisr = 0, dec = 0;
    std::uint64_t tb = 0;
    std::array<std::uint32_t, 4> sprg{};
    std::array<std::uint32_t, 8> ibat{}, dbat{};  // [2i] = upper, [2i + 1] = lower word of BAT i
    std::array<std::uint32_t, 8> gqr{};           // stored here; used by the paired singles
    std::array<std::uint32_t, 16> sr{};           // stored only: no page tables (BAT-only MMU)
    std::uint32_t hid0 = 0, hid1 = 0, hid2 = 0, hid4 = 0, wpar = 0, dmau = 0, dmal = 0, l2cr = 0;
    bool reservation = false;               // lwarx/stwcx.
    std::uint32_t reservation_address = 0;  // the EA lwarx used; stwcx. stores only to the same one
    bool external_interrupt = false;   // the level of the interrupt line (PI)
    bool decrementer_pending = false;  // DEC went from >= 0 to negative; taken when MSR[EE]
};

class Gekko;
// An instruction handler (gekko_integer.cpp, gekko_fpu.cpp, gekko_paired.cpp).
using GekkoOp = void (*)(Gekko&, std::uint32_t);
struct GekkoDecodeTables;

// The IBM Gekko (PowerPC 750CXe derivative), interpreted one instruction per step(). This is
// the reference the future JIT is checked against, so semantics follow the PowerPC
// Programming Environments manual (PEM) and the 750 user manual; where the Gekko differs,
// the code says so.
class Gekko {
public:
    static constexpr std::uint32_t kPvr = 0x00083214;

    // MSR bits (PEM numbering: bit 0 is the MSB).
    static constexpr std::uint32_t kMsrPow = 1u << 18, kMsrIle = 1u << 16, kMsrEe = 1u << 15, kMsrPr = 1u << 14,
                                   kMsrFp = 1u << 13, kMsrMe = 1u << 12, kMsrFe0 = 1u << 11, kMsrSe = 1u << 10,
                                   kMsrBe = 1u << 9, kMsrFe1 = 1u << 8, kMsrIp = 1u << 6, kMsrIr = 1u << 5,
                                   kMsrDr = 1u << 4, kMsrRi = 1u << 1, kMsrLe = 1u;
    // XER bits.
    static constexpr std::uint32_t kXerSo = 0x80000000, kXerOv = 0x40000000, kXerCa = 0x20000000;
    static constexpr std::uint32_t kXerMask = 0xE000007F;  // SO, OV, CA and the string byte count
    // SRR1 cause bits of a program exception.
    static constexpr std::uint32_t kProgramIllegal = 0x00080000, kProgramPrivileged = 0x00040000,
                                   kProgramTrap = 0x00020000;
    // DSISR bits of a DSI.
    static constexpr std::uint32_t kDsisrNoTranslation = 0x40000000, kDsisrProtection = 0x08000000,
                                   kDsisrStore = 0x02000000;
    // HID0[ICE]/[DCE]: instruction and data caches enabled. No cache is modeled, but dcbz and
    // dcbz_l raise alignment while DCE = 0, as on the 750.
    static constexpr std::uint32_t kHid0Ice = 0x00008000, kHid0Dce = 0x00004000;
    // HID0[ICFI]/[DCFI]: flash-invalidate the instruction / data cache. They clear themselves
    // once the invalidation starts, so they always read as 0.
    static constexpr std::uint32_t kHid0Icfi = 0x00000800, kHid0Dcfi = 0x00000400;
    // HID2: LSQE (psq_l[u]/psq_st[u] legal), WPE (write-gather pipe), PSE (paired singles), LCE
    // (the locked cache is enabled: dcbz_l is legal).
    static constexpr std::uint32_t kHid2Lsqe = 0x80000000, kHid2Wpe = 0x40000000, kHid2Pse = 0x20000000,
                                   kHid2Lce = 0x10000000;
    // FPSCR bits (PEM 2.1.4). FEX and VX are summaries the CPU computes; the rest are stored.
    static constexpr std::uint32_t kFpscrFx = 0x80000000, kFpscrFex = 0x40000000, kFpscrVx = 0x20000000,
                                   kFpscrOx = 0x10000000, kFpscrUx = 0x08000000, kFpscrZx = 0x04000000,
                                   kFpscrXx = 0x02000000, kFpscrVxsnan = 0x01000000, kFpscrVxisi = 0x00800000,
                                   kFpscrVxidi = 0x00400000, kFpscrVxzdz = 0x00200000, kFpscrVximz = 0x00100000,
                                   kFpscrVxvc = 0x00080000, kFpscrFr = 0x00040000, kFpscrFi = 0x00020000,
                                   kFpscrFprf = 0x0001F000, kFpscrFpcc = 0x0000F000, kFpscrVxsoft = 0x00000400,
                                   kFpscrVxsqrt = 0x00000200, kFpscrVxcvi = 0x00000100, kFpscrVe = 0x00000080,
                                   kFpscrOe = 0x00000040, kFpscrUe = 0x00000020, kFpscrZe = 0x00000010,
                                   kFpscrXe = 0x00000008, kFpscrNi = 0x00000004, kFpscrRn = 0x00000003;
    // The invalid-operation bits (any of them sets VX), and every sticky exception bit.
    static constexpr std::uint32_t kFpscrVxAny = kFpscrVxsnan | kFpscrVxisi | kFpscrVxidi | kFpscrVxzdz |
                                                 kFpscrVximz | kFpscrVxvc | kFpscrVxsoft | kFpscrVxsqrt |
                                                 kFpscrVxcvi;
    static constexpr std::uint32_t kFpscrAnyException = kFpscrOx | kFpscrUx | kFpscrZx | kFpscrXx | kFpscrVxAny;
    // Bit 20 is reserved and reads as 0.
    static constexpr std::uint32_t kFpscrWritable = 0xFFFFF7FF;
    // FPSCR with its summary bits recomputed: VX = any VX* bit; FEX = any exception bit whose
    // enable (VE, OE, UE, ZE, XE: 22 bits lower) is set.
    static constexpr std::uint32_t fpscr_with_summary(std::uint32_t value) {
        value &= ~(kFpscrVx | kFpscrFex);
        if ((value & kFpscrVxAny) != 0) value |= kFpscrVx;
        if (((value >> 22) & value & (kFpscrVe | kFpscrOe | kFpscrUe | kFpscrZe | kFpscrXe)) != 0) value |= kFpscrFex;
        return value;
    }

    // The exceptions this core takes, by vector offset.
    enum class Exception : std::uint32_t {
        SystemReset = 0x100,
        Dsi = 0x300,
        Isi = 0x400,
        External = 0x500,
        Alignment = 0x600,
        Program = 0x700,
        FpUnavailable = 0x800,
        Decrementer = 0x900,
        Syscall = 0xC00,
    };

    explicit Gekko(CpuBus& bus);

    // Power-on state: pc at the system reset vector 0xFFF00100, MSR[IP] set, the rest zero.
    void reset();
    // Takes a pending interrupt (external, then decrementer) if MSR[EE] allows it, otherwise
    // fetches and executes one instruction. Returns the CPU cycles used (1).
    int step();

    // The external interrupt line (level-sensitive): taken while high and MSR[EE] = 1.
    void set_external_interrupt(bool level) { s_.external_interrupt = level; }
    // The time base and decrementer advance by `ticks` (one tick per 12 CPU cycles). DEC
    // going from >= 0 to negative requests the decrementer exception.
    void advance_timebase(std::uint64_t ticks);

    // BAT translation of an effective address. Identity when MSR[IR] (instruction) or MSR[DR]
    // (data) is off. Otherwise the first of the 4 IBAT or DBAT pairs that matches; empty
    // when none does or its protection forbids the access. No side effects: the CPU's own
    // accesses turn an empty result into ISI or DSI.
    [[nodiscard]] std::optional<std::uint32_t> translate(std::uint32_t ea, bool instruction, bool write) const;

    [[nodiscard]] std::uint32_t gpr(int index) const { return s_.gpr[static_cast<std::size_t>(index)]; }
    void set_gpr(int index, std::uint32_t value) { s_.gpr[static_cast<std::size_t>(index)] = value; }
    [[nodiscard]] std::uint32_t pc() const { return s_.pc; }
    void set_pc(std::uint32_t pc) {
        s_.pc = pc;
        s_.npc = pc + 4;
    }
    [[nodiscard]] std::uint32_t msr() const { return s_.msr; }
    // Sets MSR as mtmsr would; a pending interrupt it enables is taken by the next step().
    // POW, ILE/LE, SE/BE and FE0/FE1 are accepted, no effect (see op_mtmsr).
    void set_msr(std::uint32_t value) { s_.msr = value; }
    [[nodiscard]] std::uint32_t cr() const { return s_.cr; }
    void set_cr(std::uint32_t value) { s_.cr = value; }
    [[nodiscard]] std::uint32_t xer() const { return s_.xer; }
    void set_xer(std::uint32_t value) { s_.xer = value & kXerMask; }
    [[nodiscard]] std::uint32_t lr() const { return s_.lr; }
    void set_lr(std::uint32_t value) { s_.lr = value; }
    [[nodiscard]] std::uint32_t ctr() const { return s_.ctr; }
    void set_ctr(std::uint32_t value) { s_.ctr = value; }
    // Floating-point registers: both halves of FPR `index`.
    [[nodiscard]] double ps0(int index) const { return s_.fpr[static_cast<std::size_t>(index)].ps0; }
    [[nodiscard]] double ps1(int index) const { return s_.fpr[static_cast<std::size_t>(index)].ps1; }
    void set_ps(int index, double ps0, double ps1) { s_.fpr[static_cast<std::size_t>(index)] = {ps0, ps1}; }
    [[nodiscard]] std::uint32_t fpscr() const { return s_.fpscr; }
    // Sets FPSCR as mtfsf with every field would: the reserved bit is dropped and FEX/VX are
    // recomputed from the other bits.
    void set_fpscr(std::uint32_t value) { s_.fpscr = fpscr_with_summary(value & kFpscrWritable); }
    // The quantization registers GQR0..7 (also SPRs 912..919).
    [[nodiscard]] std::uint32_t gqr(int index) const { return s_.gqr[static_cast<std::size_t>(index)]; }
    void set_gqr(int index, std::uint32_t value) { s_.gqr[static_cast<std::size_t>(index)] = value; }
    // mfspr/mtspr by number, with their effects (a DEC write can request the exception).
    // Throw GekkoError (word_or_address = the SPR number) for an SPR this core does not
    // model, or a write to a read-only one (PVR, the TBL/TBU read numbers).
    [[nodiscard]] std::uint32_t spr(int number) const;
    void set_spr(int number, std::uint32_t value);
    [[nodiscard]] GekkoState& state() { return s_; }
    [[nodiscard]] const GekkoState& state() const { return s_; }
    // The physical bus, for code outside the CPU that needs guest memory (the HLE boot, the
    // runner, debuggers): physical addresses, no translation, no exceptions. For an effective
    // address, translate() it first.
    [[nodiscard]] CpuBus& bus() { return bus_; }

    // --- Services for the instruction handlers that cannot fault (the faulting ones are
    // private, reached through GekkoOps in gekko_ops.hpp) ---

    // Stops emulation: the executing instruction is not implemented.
    [[noreturn]] void unimplemented(std::uint32_t word) const;
    // Enters an exception handler: SRR0 = srr0; SRR1 = MSR bits 0, 5-9 and 16-31 | cause;
    // MSR keeps ILE, ME and IP, LE = ILE, the rest cleared; pc = vector (0 or 0xFFF00000 base
    // by MSR[IP]). Does not end an executing instruction (raise() does).
    void enter_exception(Exception kind, std::uint32_t srr0, std::uint32_t srr1_cause = 0);
    // CR0 = LT/GT/EQ of the signed result, plus XER[SO].
    void update_cr0(std::uint32_t result) {
        const auto v = static_cast<std::int32_t>(result);
        const std::uint32_t field = v < 0 ? 8u : v > 0 ? 4u : 2u;
        s_.cr = (s_.cr & 0x0FFFFFFF) | ((field | (s_.xer >> 31)) << 28);
    }
    // mfspr/mtspr without the privilege check; false for an unknown (or, for writes,
    // read-only) SPR.
    bool try_read_spr(int number, std::uint32_t& value) const;
    bool try_write_spr(int number, std::uint32_t value);

private:
    // Instruction handlers reach the services below through GekkoOps (gekko_ops.hpp).
    friend struct GekkoOps;

    // These are valid only while step() executes an instruction: a fault enters the exception
    // (SRR0/SRR1/MSR/pc change) and ends the instruction by throwing an internal exception
    // that only step() catches. Called from anywhere else they would corrupt the state and
    // throw an unknown type, hence private; outside code uses bus() and translate().

    // Data accesses at an effective address, translated by the DBATs. A miss or a protection
    // violation raises DSI, which ends the instruction (nothing after it runs). Unaligned
    // accesses are split (a doubleword at a word boundary into two words, anything else into
    // bytes), as the 750 handles them in hardware.
    std::uint8_t read8(std::uint32_t ea);
    std::uint16_t read16(std::uint32_t ea);
    std::uint32_t read32(std::uint32_t ea);
    std::uint64_t read64(std::uint32_t ea);
    void write8(std::uint32_t ea, std::uint8_t value);
    void write16(std::uint32_t ea, std::uint16_t value);
    void write32(std::uint32_t ea, std::uint32_t value);
    void write64(std::uint32_t ea, std::uint64_t value);

    // Takes an exception caused by the executing instruction and ends it: SRR0 = that
    // instruction (the next one for Syscall), SRR1 = MSR | srr1_cause.
    [[noreturn]] void raise(Exception kind, std::uint32_t srr1_cause = 0);
    // Alignment exception for the executing instruction: DAR = ea, DSISR from its encoding.
    [[noreturn]] void raise_alignment(std::uint32_t ea, std::uint32_t word);
    // Program exception (privileged) unless MSR[PR] = 0.
    void require_supervisor() {
        if ((s_.msr & kMsrPr) != 0) raise(Exception::Program, kProgramPrivileged);
    }

    std::uint32_t data_address(std::uint32_t ea, bool write);

    CpuBus& bus_;
    GekkoState s_;
    const GekkoDecodeTables* tables_;
};

}  // namespace ezy::gc
