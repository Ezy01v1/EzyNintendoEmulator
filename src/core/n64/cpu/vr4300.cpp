// VR4300: the step loop, address translation and the load/store instructions.
#include "n64/cpu/vr4300.hpp"

#include <cstdio>

namespace ezy::n64 {

namespace {

constexpr std::uint64_t sext32(std::uint32_t v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(v))); }

}  // namespace

int Vr4300::step() {
    cycles_ = 1;
    current_pc_ = pc_;
    in_delay_slot_ = branch_taken_;
    branch_taken_ = false;

    std::uint32_t instr = 0;
    if (!load(pc_, instr)) return cycles_;
    pc_ = next_pc_;
    next_pc_ += 4;
    execute(instr);
    gpr_[0] = 0;
    return cycles_;
}

// Fixed segments of the 32-bit address space (the CPU sign-extends 32-bit addresses):
//   kseg0 0xFFFFFFFF80000000-9FFFFFFF  physical = vaddr & 0x1FFFFFFF, cached
//   kseg1 0xFFFFFFFFA0000000-BFFFFFFF  physical = vaddr & 0x1FFFFFFF, uncached
bool Vr4300::translate(std::uint64_t vaddr, bool /*write*/, Translation& out) {
    if (vaddr >= 0xFFFFFFFF80000000ULL && vaddr < 0xFFFFFFFFC0000000ULL) {
        out.paddr = static_cast<std::uint32_t>(vaddr & 0x1FFFFFFF);
        out.cached = vaddr < 0xFFFFFFFFA0000000ULL;
        return true;
    }
    return false;
}

template <typename T>
bool Vr4300::load(std::uint64_t vaddr, T& value) {
    Translation t{};
    if (!translate(vaddr, false, t)) return false;
    if (!t.cached) cycles_ += static_cast<int>(bus_.uncached_cycles(t.paddr));
    if constexpr (sizeof(T) == 1) value = bus_.read8(t.paddr);
    else if constexpr (sizeof(T) == 2) value = bus_.read16(t.paddr);
    else if constexpr (sizeof(T) == 4) value = bus_.read32(t.paddr);
    else value = bus_.read64(t.paddr);
    return true;
}

template <typename T>
bool Vr4300::store(std::uint64_t vaddr, T value) {
    Translation t{};
    if (!translate(vaddr, true, t)) return false;
    if (!t.cached) cycles_ += static_cast<int>(bus_.uncached_cycles(t.paddr));
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
            if (load(vaddr, v)) { set_gpr(rt, sext32(v)); llbit_ = true; }
            break;
        }
        case 0x34: {  // LLD
            std::uint64_t v;
            if (load(vaddr, v)) { set_gpr(rt, v); llbit_ = true; }
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
            if (!load(aligned, word)) break;
            const unsigned shift = static_cast<unsigned>(vaddr & 3) * 8;
            const std::uint32_t mask = 0xFFFFFFFFU >> shift;
            store(aligned, (word & ~mask) | (static_cast<std::uint32_t>(reg) >> shift));
            break;
        }
        case 0x2E: {  // SWR
            std::uint32_t word;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{3};
            if (!load(aligned, word)) break;
            const unsigned shift = (3 - static_cast<unsigned>(vaddr & 3)) * 8;
            const std::uint32_t keep = static_cast<std::uint32_t>((std::uint64_t{1} << shift) - 1);
            store(aligned, (word & keep) | (static_cast<std::uint32_t>(reg) << shift));
            break;
        }
        case 0x2C: {  // SDL
            std::uint64_t dword;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{7};
            if (!load(aligned, dword)) break;
            const unsigned shift = static_cast<unsigned>(vaddr & 7) * 8;
            const std::uint64_t mask = ~std::uint64_t{0} >> shift;
            store(aligned, (dword & ~mask) | (reg >> shift));
            break;
        }
        case 0x2D: {  // SDR
            std::uint64_t dword;
            const std::uint64_t aligned = vaddr & ~std::uint64_t{7};
            if (!load(aligned, dword)) break;
            const unsigned shift = (7 - static_cast<unsigned>(vaddr & 7)) * 8;
            const std::uint64_t keep = shift == 0 ? 0 : (std::uint64_t{1} << shift) - 1;
            store(aligned, (dword & keep) | (reg << shift));
            break;
        }
        default: break;
    }
}

}  // namespace ezy::n64
