// VR4300: the step loop, address translation and the load/store instructions.
#include "n64/cpu/vr4300.hpp"

#include <cstdio>

namespace ezy::n64 {

namespace {

constexpr std::uint64_t sext32(std::uint32_t v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(v))); }

}  // namespace

// An idle loop is a branch to itself with a NOP in its delay slot, in cached kseg0 RDRAM:
// libultra's idle thread ("b ." / "j .") spins there until an interrupt. Each iteration
// is two one-cycle instructions that change nothing but the time, Count and Random.
int Vr4300::idle_loop_cycles() const {
    if (pc_ != idle_candidate_) return 0;  // only after a branch that jumped to itself
    if (branch_taken_ || next_pc_ != pc_ + 4) return 0;  // must be at the branch itself
    const std::uint32_t status = cop0_.status();
    const std::uint32_t ksu = (status >> 3) & 3;
    if (!(ksu == 0 || ksu == 3 || (status & 6) != 0)) return 0;  // kernel mode
    if (pc_ - 0xFFFFFFFF80000000ULL >= 0x20000000ULL) return 0;  // kseg0 only (cached)
    const auto paddr = static_cast<std::uint32_t>(pc_ & 0x1FFFFFFF);
    if (paddr + 8 > bus_.direct_ram_size()) return 0;
    if (interrupt_pending()) return 0;  // the next step leaves the loop
    bus_.guard_memory(paddr, 8, false);
    const std::uint8_t* m = bus_.direct_ram() + paddr;
    const std::uint32_t branch = (std::uint32_t{m[0]} << 24) | (m[1] << 16) | (m[2] << 8) | m[3];
    const std::uint32_t slot = (std::uint32_t{m[4]} << 24) | (m[5] << 16) | (m[6] << 8) | m[7];
    if (slot != 0) return 0;
    const bool beq_self = branch == 0x1000FFFF;  // BEQ $0, $0, -1
    const bool j_self = (branch >> 26) == 2 &&
                        (((pc_ + 4) & ~std::uint64_t{0x0FFFFFFF}) | ((branch & 0x3FFFFFF) << 2)) == pc_;
    return beq_self || j_self ? 2 : 0;
}

bool Vr4300::interrupt_pending() const {
    const std::uint32_t status = cop0_.status();
    const std::uint32_t cause = (cop0_.cause() & ~Cop0::kIP2) | (bus_.interrupt_line() ? Cop0::kIP2 : 0);
    return (status & Cop0::kIE) && !(status & (Cop0::kEXL | Cop0::kERL)) && (cause & status & 0xFF00);
}

// Each cycle is one instruction of the loop. An odd count ends after the branch, with the
// NOP in its delay slot next.
void Vr4300::skip_idle_cycles(std::uint64_t cycles) {
    cop0_.retire_many(cycles, cycles);
    if (cycles & 1) {
        current_pc_ = pc_;
        pc_ = next_pc_;
        next_pc_ = current_pc_;
        branch_taken_ = true;
    }
}

int Vr4300::step() {
    cycles_ = 1;
    exception_ = false;
    current_pc_ = pc_;
    in_delay_slot_ = branch_taken_;
    branch_taken_ = false;

    cop0_.set_ip2(bus_.interrupt_line());
    const std::uint32_t status = cop0_.status();
    if ((status & Cop0::kIE) && !(status & (Cop0::kEXL | Cop0::kERL)) && (cop0_.cause() & status & 0xFF00)) {
        raise(ExceptionCode::Int);
    } else {
        std::uint32_t instr = 0;
        if (load(pc_, instr)) {
            pc_ = next_pc_;
            next_pc_ += 4;
            execute(instr);
        }
    }
    gpr_[0] = 0;
    cop0_.retire(cycles_);
    return cycles_;
}

Vr4300::Mode Vr4300::mode() const {
    const std::uint32_t status = cop0_.status();
    if (status & (Cop0::kEXL | Cop0::kERL)) return Mode::Kernel;
    switch ((status >> 3) & 3) {
        case 1: return Mode::Supervisor;
        case 2: return Mode::User;
        default: return Mode::Kernel;
    }
}

bool Vr4300::allow_64bit_ops() const {
    switch (mode()) {
        case Mode::Supervisor: return (cop0_.status() & Cop0::kSX) != 0;
        case Mode::User: return (cop0_.status() & Cop0::kUX) != 0;
        default: return true;
    }
}

void Vr4300::enter_exception(ExceptionCode code, int coprocessor, std::uint32_t offset) {
    std::uint64_t& status = cop0_.raw(Cop0::kStatus);
    std::uint64_t& cause = cop0_.raw(Cop0::kCause);
    if (!(status & Cop0::kEXL)) {
        // EPC points at the branch when the faulting instruction is in its delay slot.
        cop0_.raw(Cop0::kEpc) = in_delay_slot_ ? current_pc_ - 4 : current_pc_;
        cause = in_delay_slot_ ? (cause | Cop0::kBD) : (cause & ~std::uint64_t{Cop0::kBD});
    } else {
        offset = 0x180;  // nested: TLB refills go to the general vector too
    }
    cause = (cause & ~std::uint64_t{0x3000007C}) | (static_cast<std::uint64_t>(code) << 2) |
            (static_cast<std::uint64_t>(coprocessor & 3) << 28);
    status |= Cop0::kEXL;
    const std::uint64_t base = (status & Cop0::kBEV) ? 0xFFFFFFFFBFC00200ULL : 0xFFFFFFFF80000000ULL;
    set_pc(base + offset);
    exception_ = true;
}

void Vr4300::raise(ExceptionCode code, int coprocessor) { enter_exception(code, coprocessor, 0x180); }

void Vr4300::raise_address_error(std::uint64_t vaddr, bool write) {
    cop0_.raw(Cop0::kBadVAddr) = vaddr;
    // Like a TLB miss, an address error fills Context and XContext (n64-systemtest
    // "Unaligned LW exception").
    std::uint64_t& context = cop0_.raw(Cop0::kContext);
    context = (context & 0xFFFFFFFFFF800000ULL) | (((vaddr >> 13) & 0x7FFFF) << 4);
    std::uint64_t& xcontext = cop0_.raw(Cop0::kXContext);
    xcontext = (xcontext & 0xFFFFFFFE00000000ULL) | ((vaddr >> 62) << 31) | (((vaddr >> 13) & 0x7FFFFFF) << 4);
    raise(write ? ExceptionCode::AdES : ExceptionCode::AdEL);
}

void Vr4300::eret() {
    std::uint64_t& status = cop0_.raw(Cop0::kStatus);
    if (status & Cop0::kERL) {
        status &= ~std::uint64_t{Cop0::kERL};
        set_pc(cop0_.raw(Cop0::kErrorEpc));
    } else {
        status &= ~std::uint64_t{Cop0::kEXL};
        set_pc(cop0_.raw(Cop0::kEpc));
    }
    llbit_ = false;
}

// COP0 (op 0x10). Usable in kernel mode, or with Status.CU0.
void Vr4300::execute_cop0(std::uint32_t instr) {
    if (mode() != Mode::Kernel && !(cop0_.status() & Cop0::kCU0)) {
        raise(ExceptionCode::CpU, 0);
        return;
    }
    const int rt = static_cast<int>((instr >> 16) & 31);
    const int rd = static_cast<int>((instr >> 11) & 31);
    switch ((instr >> 21) & 31) {
        case 0x00: set_gpr(rt, sext32(static_cast<std::uint32_t>(cop0_.read(rd)))); break;  // MFC0
        case 0x01: set_gpr(rt, cop0_.read(rd)); break;                                      // DMFC0
        case 0x04: cop0_.write(rd, sext32(static_cast<std::uint32_t>(gpr(rt)))); break;      // MTC0
        case 0x05: cop0_.write(rd, gpr(rt)); break;                                          // DMTC0
        default:
            if (instr & (1U << 25)) {  // CO
                if ((instr & 0x3F) == 0x18) eret();
                else execute_tlb(instr);
                return;
            }
            raise(ExceptionCode::RI);
            break;
    }
}

// Fixed segments of the 32-bit address space (the CPU sign-extends 32-bit addresses):
//   kseg0 0xFFFFFFFF80000000-9FFFFFFF  physical = vaddr & 0x1FFFFFFF, cached
//   kseg1 0xFFFFFFFFA0000000-BFFFFFFF  physical = vaddr & 0x1FFFFFFF, uncached
// Only kernel mode may use them. Everything else goes through the TLB.
bool Vr4300::translate(std::uint64_t vaddr, bool write, Translation& out) {
    // Kernel mode: KSU = 0, or EXL/ERL set.
    const std::uint32_t status = cop0_.status();
    const std::uint32_t ksu = (status >> 3) & 3;  // 0 and 3 are kernel, like mode()
    if (vaddr - 0xFFFFFFFF80000000ULL < 0x40000000ULL && (ksu == 0 || ksu == 3 || (status & 6) != 0)) {
        out.paddr = static_cast<std::uint32_t>(vaddr & 0x1FFFFFFF);
        out.cached = vaddr < 0xFFFFFFFFA0000000ULL;
        return true;
    }
    return translate_mapped(vaddr, write, out);
}

bool Vr4300::wide_addressing() const {
    switch (mode()) {
        case Mode::Kernel: return (cop0_.status() & Cop0::kKX) != 0;
        case Mode::Supervisor: return (cop0_.status() & Cop0::kSX) != 0;
        default: return (cop0_.status() & Cop0::kUX) != 0;
    }
}

// Every address outside kernel-mode kseg0/kseg1. Segments (VR4300 manual, chapter 5):
//   32-bit: kuseg 0-7FFFFFFF (all modes), ksseg C0000000-DFFFFFFF (supervisor, kernel),
//           kseg3 E0000000-FFFFFFFF (kernel); all mapped. With ERL set, kernel kuseg is
//           unmapped and uncached.
//   64-bit: xkuseg below 2^40, xksseg 0x4000... (supervisor, kernel), xkphys 0x8000...-
//           0xBFFF... (kernel; unmapped, bits 61:59 pick the cache mode), xkseg 0xC000...
//           (kernel), plus the 32-bit compatibility segments at the top.
// In 32-bit addressing an address must be a sign-extended 32-bit value.
bool Vr4300::translate_mapped(std::uint64_t vaddr, bool write, Translation& out) {
    const Mode m = mode();
    const bool wide = wide_addressing();
    const auto sext = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(vaddr)));
    bool ok = false;
    if (!wide || vaddr == sext) {
        if (vaddr != sext) {
            ok = false;
        } else if (vaddr < 0x80000000ULL) {
            if (m == Mode::Kernel && (cop0_.status() & Cop0::kERL)) {
                out.paddr = static_cast<std::uint32_t>(vaddr);
                out.cached = false;
                return true;
            }
            ok = true;
        } else if (vaddr >= 0xFFFFFFFFC0000000ULL && vaddr < 0xFFFFFFFFE0000000ULL) {
            ok = m != Mode::User;
        } else if (vaddr >= 0xFFFFFFFFE0000000ULL) {
            ok = m == Mode::Kernel;
        }  // kseg0/kseg1 outside kernel mode: address error
    } else {
        switch (vaddr >> 62) {
            case 0: ok = vaddr < (1ULL << 40); break;
            case 1: ok = m != Mode::User && (vaddr & 0x3FFFFFFFFFFFFFFFULL) < (1ULL << 40); break;
            case 2:
                if (m != Mode::Kernel || (vaddr & 0x07FFFFFF00000000ULL) != 0) break;
                out.paddr = static_cast<std::uint32_t>(vaddr);
                out.cached = ((vaddr >> 59) & 7) != 2;
                return true;
            default: ok = m == Mode::Kernel && (vaddr & 0x3FFFFFFFFFFFFFFFULL) < (1ULL << 40); break;
        }
    }
    if (!ok) {
        raise_address_error(vaddr, write);
        return false;
    }
    const auto asid = static_cast<std::uint8_t>(cop0_.read(Cop0::kEntryHi));
    const Tlb::Result r = tlb_.translate(vaddr, asid);
    if (!r.hit) {
        raise_tlb(write ? ExceptionCode::TlbS : ExceptionCode::TlbL, vaddr, true, wide);
        return false;
    }
    if (!r.valid) {
        raise_tlb(write ? ExceptionCode::TlbS : ExceptionCode::TlbL, vaddr, false, wide);
        return false;
    }
    if (write && !r.dirty) {
        raise_tlb(ExceptionCode::Mod, vaddr, false, wide);
        return false;
    }
    out.paddr = r.paddr;
    out.cached = r.cached;
    return true;
}

void Vr4300::raise_tlb(ExceptionCode code, std::uint64_t vaddr, bool refill, bool wide) {
    cop0_.raw(Cop0::kBadVAddr) = vaddr;
    std::uint64_t& context = cop0_.raw(Cop0::kContext);
    context = (context & 0xFFFFFFFFFF800000ULL) | (((vaddr >> 13) & 0x7FFFF) << 4);
    std::uint64_t& xcontext = cop0_.raw(Cop0::kXContext);
    xcontext = (xcontext & 0xFFFFFFFE00000000ULL) | ((vaddr >> 62) << 31) | (((vaddr >> 13) & 0x7FFFFFF) << 4);
    std::uint64_t& entry_hi = cop0_.raw(Cop0::kEntryHi);
    entry_hi = (vaddr & 0xC00000FFFFFFE000ULL) | (entry_hi & 0xFF);
    // TLB refill vectors: 0x000 (32-bit addressing) or 0x080 (64-bit); otherwise 0x180.
    enter_exception(code, 0, refill ? (wide ? 0x080 : 0x000) : 0x180);
}

void Vr4300::execute_tlb(std::uint32_t instr) {
    switch (instr & 0x3F) {
        case 0x01: tlb_.read(static_cast<int>(cop0_.read(Cop0::kIndex) & 31), cop0_); break;    // TLBR
        case 0x02: tlb_.write(static_cast<int>(cop0_.read(Cop0::kIndex) & 31), cop0_); break;   // TLBWI
        case 0x06: tlb_.write(static_cast<int>(cop0_.read(Cop0::kRandom) & 31), cop0_); break;  // TLBWR
        case 0x08: {                                                                            // TLBP
            const int index = tlb_.probe(cop0_);
            cop0_.raw(Cop0::kIndex) = index < 0 ? 0x80000000ULL : static_cast<std::uint64_t>(index);
            break;
        }
        default: break;  // the other COP0 functions are undefined and do nothing
    }
}

bool Vr4300::require_64bit() {
    if (allow_64bit_ops()) return true;
    raise(ExceptionCode::RI);
    return false;
}

template <typename T>
bool Vr4300::load(std::uint64_t vaddr, T& value, bool for_store) {
    if (vaddr & (sizeof(T) - 1)) {
        raise_address_error(vaddr, for_store);
        return false;
    }
    Translation t{};
    if (!translate(vaddr, for_store, t)) return false;
    last_paddr_ = t.paddr;
    if (!t.cached) cycles_ += static_cast<int>(bus_.uncached_cycles(t.paddr));
    if (t.paddr < bus_.direct_ram_size()) {  // RDRAM: read it in place (big-endian)
        bus_.guard_memory(t.paddr, sizeof(T), false);
        const std::uint8_t* m = bus_.direct_ram() + t.paddr;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v = (v << 8) | m[i];
        value = static_cast<T>(v);
        return true;
    }
    if constexpr (sizeof(T) == 1) value = bus_.read8(t.paddr);
    else if constexpr (sizeof(T) == 2) value = bus_.read16(t.paddr);
    else if constexpr (sizeof(T) == 4) value = bus_.read32(t.paddr);
    else value = bus_.read64(t.paddr);
    return true;
}

template <typename T>
bool Vr4300::store(std::uint64_t vaddr, T value) {
    if (vaddr & (sizeof(T) - 1)) {
        raise_address_error(vaddr, true);
        return false;
    }
    Translation t{};
    if (!translate(vaddr, true, t)) return false;
    if (!t.cached) cycles_ += static_cast<int>(bus_.uncached_cycles(t.paddr));
    if (t.paddr < bus_.direct_ram_size()) {
        bus_.guard_memory(t.paddr, sizeof(T), true);
        std::uint8_t* m = bus_.direct_ram() + t.paddr;
        for (std::size_t i = 0; i < sizeof(T); ++i) m[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (8 * (sizeof(T) - 1 - i)));
        return true;
    }
    if constexpr (sizeof(T) == 1) bus_.write8(t.paddr, value);
    else if constexpr (sizeof(T) == 2) bus_.write16(t.paddr, value);
    else if constexpr (sizeof(T) == 4) bus_.write32(t.paddr, value);
    else bus_.write64(t.paddr, value);
    return true;
}

// Loads and stores: base + signed 16-bit offset.
//
// The unaligned ones work on the aligned word (or doubleword) that holds the address.
// Big-endian: LWL/LDL/SWL/SDL handle the bytes from the address to the end of that word
// (the most significant part of the register), LWR/LDR/SWR/SDR the bytes from its start
// up to the address (the least significant part). In 64-bit registers LWL always
// sign-extends; LWR sign-extends only when it writes the whole word, otherwise the upper
// 32 bits are kept.
void Vr4300::execute_load_store(std::uint32_t instr) {
    const int op = static_cast<int>(instr >> 26);
    const int base = static_cast<int>((instr >> 21) & 31);
    const int rt = static_cast<int>((instr >> 16) & 31);
    const std::uint64_t vaddr = gpr_[static_cast<std::size_t>(base)] + static_cast<std::uint64_t>(static_cast<std::int16_t>(instr & 0xFFFF));
    std::uint64_t& reg = gpr_[static_cast<std::size_t>(rt)];
    const bool is_64bit = op == 0x27 || op == 0x37 || op == 0x34 || op == 0x1A || op == 0x1B || op == 0x3F ||
                          op == 0x3C || op == 0x2C || op == 0x2D;  // LWU LD LLD LDL LDR SD SCD SDL SDR
    if (is_64bit && !require_64bit()) return;

    switch (op) {
        case 0x20: { std::uint8_t v; if (load(vaddr, v)) set_gpr(rt, static_cast<std::uint64_t>(static_cast<std::int8_t>(v))); break; }   // LB
        case 0x24: { std::uint8_t v; if (load(vaddr, v)) set_gpr(rt, v); break; }                                                     // LBU
        case 0x21: { std::uint16_t v; if (load(vaddr, v)) set_gpr(rt, static_cast<std::uint64_t>(static_cast<std::int16_t>(v))); break; }  // LH
        case 0x25: { std::uint16_t v; if (load(vaddr, v)) set_gpr(rt, v); break; }                                                    // LHU
        case 0x23: { std::uint32_t v; if (load(vaddr, v)) set_gpr(rt, sext32(v)); break; }                                           // LW
        case 0x27: { std::uint32_t v; if (load(vaddr, v)) set_gpr(rt, v); break; }                                                    // LWU
        case 0x37: { std::uint64_t v; if (load(vaddr, v)) set_gpr(rt, v); break; }                                                    // LD
        case 0x30: {  // LL
            std::uint32_t v;
            if (load(vaddr, v)) {
                set_gpr(rt, sext32(v));
                llbit_ = true;
                cop0_.raw(Cop0::kLLAddr) = last_paddr_ >> 4;
            }
            break;
        }
        case 0x34: {  // LLD
            std::uint64_t v;
            if (load(vaddr, v)) {
                set_gpr(rt, v);
                llbit_ = true;
                cop0_.raw(Cop0::kLLAddr) = last_paddr_ >> 4;
            }
            break;
        }
        case 0x22: {  // LWL
            std::uint32_t word;
            if (!load(vaddr & ~std::uint64_t{3}, word)) break;
            const unsigned shift = static_cast<unsigned>(vaddr & 3) * 8;
            const std::uint32_t keep = static_cast<std::uint32_t>((std::uint64_t{1} << shift) - 1);
            set_gpr(rt, sext32((static_cast<std::uint32_t>(reg) & keep) | (word << shift)));
            break;
        }
        case 0x26: {  // LWR
            std::uint32_t word;
            if (!load(vaddr & ~std::uint64_t{3}, word)) break;
            const unsigned shift = (3 - static_cast<unsigned>(vaddr & 3)) * 8;
            const std::uint32_t mask = 0xFFFFFFFFU >> shift;
            const std::uint32_t merged = (static_cast<std::uint32_t>(reg) & ~mask) | (word >> shift);
            set_gpr(rt, shift == 0 ? sext32(merged) : (reg & 0xFFFFFFFF00000000ULL) | merged);
            break;
        }
        case 0x1A: {  // LDL
            std::uint64_t dword;
            if (!load(vaddr & ~std::uint64_t{7}, dword)) break;
            const unsigned shift = static_cast<unsigned>(vaddr & 7) * 8;
            const std::uint64_t keep = shift == 0 ? 0 : (std::uint64_t{1} << shift) - 1;
            set_gpr(rt, (reg & keep) | (dword << shift));
            break;
        }
        case 0x1B: {  // LDR
            std::uint64_t dword;
            if (!load(vaddr & ~std::uint64_t{7}, dword)) break;
            const unsigned shift = (7 - static_cast<unsigned>(vaddr & 7)) * 8;
            const std::uint64_t mask = ~std::uint64_t{0} >> shift;
            set_gpr(rt, (reg & ~mask) | (dword >> shift));
            break;
        }
        case 0x28: store(vaddr, static_cast<std::uint8_t>(reg)); break;   // SB
        case 0x29: store(vaddr, static_cast<std::uint16_t>(reg)); break;  // SH
        case 0x2B: store(vaddr, static_cast<std::uint32_t>(reg)); break;  // SW
        case 0x3F: store(vaddr, reg); break;                              // SD
        case 0x38: {  // SC
            const bool ok = llbit_ && store(vaddr, static_cast<std::uint32_t>(reg));
            set_gpr(rt, ok ? 1 : 0);
            break;
        }
        case 0x3C: {  // SCD
            const bool ok = llbit_ && store(vaddr, reg);
            set_gpr(rt, ok ? 1 : 0);
            break;
        }
        case 0x2A: {  // SWL
            std::uint32_t word;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{3};
            if (!load(aligned, word, true)) break;
            const unsigned shift = static_cast<unsigned>(vaddr & 3) * 8;
            const std::uint32_t mask = 0xFFFFFFFFU >> shift;
            store(aligned, (word & ~mask) | (static_cast<std::uint32_t>(reg) >> shift));
            break;
        }
        case 0x2E: {  // SWR
            std::uint32_t word;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{3};
            if (!load(aligned, word, true)) break;
            const unsigned shift = (3 - static_cast<unsigned>(vaddr & 3)) * 8;
            const std::uint32_t keep = static_cast<std::uint32_t>((std::uint64_t{1} << shift) - 1);
            store(aligned, (word & keep) | (static_cast<std::uint32_t>(reg) << shift));
            break;
        }
        case 0x2C: {  // SDL
            std::uint64_t dword;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{7};
            if (!load(aligned, dword, true)) break;
            const unsigned shift = static_cast<unsigned>(vaddr & 7) * 8;
            const std::uint64_t mask = ~std::uint64_t{0} >> shift;
            store(aligned, (dword & ~mask) | (reg >> shift));
            break;
        }
        case 0x2D: {  // SDR
            std::uint64_t dword;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{7};
            if (!load(aligned, dword, true)) break;
            const unsigned shift = (7 - static_cast<unsigned>(vaddr & 7)) * 8;
            const std::uint64_t keep = shift == 0 ? 0 : (std::uint64_t{1} << shift) - 1;
            store(aligned, (dword & keep) | (reg << shift));
            break;
        }
        // COP1 loads and stores (Status.CU1 was checked by the caller). With FR = 0, LDC1 and
        // SDC1 use the even register of the pair.
        case 0x31: {  // LWC1
            std::uint32_t v;
            if (load(vaddr, v)) cop1_.write32(rt, v, (cop0_.status() & Cop0::kFR) != 0);
            break;
        }
        case 0x35: {  // LDC1
            std::uint64_t v;
            if (load(vaddr, v)) cop1_.write64(rt, v, (cop0_.status() & Cop0::kFR) != 0);
            break;
        }
        case 0x39: store(vaddr, cop1_.read32(rt, (cop0_.status() & Cop0::kFR) != 0)); break;  // SWC1
        case 0x3D: store(vaddr, cop1_.read64(rt, (cop0_.status() & Cop0::kFR) != 0)); break;  // SDC1
        default: break;
    }
}

}  // namespace ezy::n64
