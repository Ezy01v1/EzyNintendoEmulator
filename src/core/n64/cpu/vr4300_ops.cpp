// VR4300: decoding, ALU, multiply/divide and branches.
#include <cstdio>

#include "n64/cpu/vr4300.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ezy::n64 {

namespace {

constexpr std::uint64_t sext32(std::uint64_t v) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v))));
}
constexpr std::uint64_t sext16(std::uint32_t instr) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(instr & 0xFFFF)));
}

// Extra cycles beyond the 1 every instruction takes (VR4300 manual, chapter 7).
constexpr int kMultExtra = 5;
constexpr int kDmultExtra = 8;
constexpr int kDivExtra = 37;
constexpr int kDdivExtra = 69;

// Full 128-bit products for DMULT/DMULTU: {hi, lo}.
void mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) {
#if defined(_MSC_VER)
    lo = _umul128(a, b, &hi);
#else
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
    hi = static_cast<std::uint64_t>(p >> 64);
    lo = static_cast<std::uint64_t>(p);
#endif
}
void mul_s64(std::int64_t a, std::int64_t b, std::uint64_t& hi, std::uint64_t& lo) {
#if defined(_MSC_VER)
    std::int64_t h = 0;
    lo = static_cast<std::uint64_t>(_mul128(a, b, &h));
    hi = static_cast<std::uint64_t>(h);
#else
    const __int128 p = static_cast<__int128>(a) * b;
    hi = static_cast<std::uint64_t>(p >> 64);
    lo = static_cast<std::uint64_t>(p);
#endif
}

}  // namespace

void Vr4300::branch(bool taken, std::uint64_t target, bool likely) {
    if (taken) {
        next_pc_ = target;
        branch_taken_ = true;
    } else if (likely) {
        // A branch-likely that isn't taken skips (nullifies) its delay slot.
        pc_ = next_pc_;
        next_pc_ += 4;
    }
}

void Vr4300::execute(std::uint32_t instr) {
    const int op = static_cast<int>(instr >> 26);
    const int rs = static_cast<int>((instr >> 21) & 31);
    const int rt = static_cast<int>((instr >> 16) & 31);
    const std::uint64_t s = gpr(rs);
    const std::uint64_t t = gpr(rt);
    const std::uint64_t imm = sext16(instr);
    const std::uint64_t zimm = instr & 0xFFFF;
    const std::uint64_t branch_target = pc_ + (imm << 2);  // pc_ is the delay slot here

    switch (op) {
        case 0x00: execute_special(instr); break;
        case 0x01: execute_regimm(instr); break;
        case 0x02:  // J
            branch(true, (pc_ & ~std::uint64_t{0x0FFFFFFF}) | ((instr & 0x3FFFFFF) << 2), false);
            break;
        case 0x03:  // JAL
            set_gpr(31, pc_ + 4);
            branch(true, (pc_ & ~std::uint64_t{0x0FFFFFFF}) | ((instr & 0x3FFFFFF) << 2), false);
            break;
        case 0x04: branch(s == t, branch_target, false); break;  // BEQ
        case 0x05: branch(s != t, branch_target, false); break;  // BNE
        case 0x06: branch(static_cast<std::int64_t>(s) <= 0, branch_target, false); break;  // BLEZ
        case 0x07: branch(static_cast<std::int64_t>(s) > 0, branch_target, false); break;   // BGTZ
        case 0x14: branch(s == t, branch_target, true); break;   // BEQL
        case 0x15: branch(s != t, branch_target, true); break;   // BNEL
        case 0x16: branch(static_cast<std::int64_t>(s) <= 0, branch_target, true); break;  // BLEZL
        case 0x17: branch(static_cast<std::int64_t>(s) > 0, branch_target, true); break;   // BGTZL
        case 0x08:  // ADDI (overflow trap: see COP0)
        case 0x09: set_gpr(rt, sext32(s + imm)); break;  // ADDIU
        case 0x0A: set_gpr(rt, static_cast<std::int64_t>(s) < static_cast<std::int64_t>(imm) ? 1 : 0); break;  // SLTI
        case 0x0B: set_gpr(rt, s < imm ? 1 : 0); break;  // SLTIU
        case 0x0C: set_gpr(rt, s & zimm); break;         // ANDI
        case 0x0D: set_gpr(rt, s | zimm); break;         // ORI
        case 0x0E: set_gpr(rt, s ^ zimm); break;         // XORI
        case 0x0F: set_gpr(rt, sext32(zimm << 16)); break;  // LUI
        case 0x18:  // DADDI
        case 0x19: set_gpr(rt, s + imm); break;  // DADDIU
        case 0x2F: break;                        // CACHE: the caches aren't emulated
        default:
            if ((op >= 0x20 && op <= 0x3F) || op == 0x1A || op == 0x1B) {
                execute_load_store(instr);
            } else {
                std::fprintf(stderr, "VR4300: unimplemented opcode %02X at %016llX\n", op,
                             static_cast<unsigned long long>(current_pc_));
            }
            break;
    }
}

void Vr4300::execute_special(std::uint32_t instr) {
    const int rs = static_cast<int>((instr >> 21) & 31);
    const int rt = static_cast<int>((instr >> 16) & 31);
    const int rd = static_cast<int>((instr >> 11) & 31);
    const unsigned sa = (instr >> 6) & 31;
    const std::uint64_t s = gpr(rs);
    const std::uint64_t t = gpr(rt);

    switch (instr & 0x3F) {
        case 0x00: set_gpr(rd, sext32(t << sa)); break;                                    // SLL
        case 0x02: set_gpr(rd, sext32(static_cast<std::uint32_t>(t) >> sa)); break;        // SRL
        // SRA/SRAV shift the whole 64-bit register, then keep the low 32 bits.
        case 0x03: set_gpr(rd, sext32(static_cast<std::uint64_t>(static_cast<std::int64_t>(t) >> sa))); break;  // SRA
        case 0x04: set_gpr(rd, sext32(t << (s & 31))); break;                              // SLLV
        case 0x06: set_gpr(rd, sext32(static_cast<std::uint32_t>(t) >> (s & 31))); break;  // SRLV
        case 0x07: set_gpr(rd, sext32(static_cast<std::uint64_t>(static_cast<std::int64_t>(t) >> (s & 31)))); break;  // SRAV
        case 0x08: branch(true, s, false); break;  // JR
        case 0x09:                                 // JALR
            set_gpr(rd, pc_ + 4);
            branch(true, s, false);
            break;
        case 0x0F: break;  // SYNC
        case 0x10: set_gpr(rd, hi_); break;  // MFHI
        case 0x11: hi_ = s; break;           // MTHI
        case 0x12: set_gpr(rd, lo_); break;  // MFLO
        case 0x13: lo_ = s; break;           // MTLO
        case 0x14: set_gpr(rd, t << (s & 63)); break;  // DSLLV
        case 0x16: set_gpr(rd, t >> (s & 63)); break;  // DSRLV
        case 0x17: set_gpr(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(t) >> (s & 63))); break;  // DSRAV
        case 0x18: {  // MULT
            const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(s)) * static_cast<std::int32_t>(t);
            lo_ = sext32(static_cast<std::uint64_t>(p));
            hi_ = sext32(static_cast<std::uint64_t>(p) >> 32);
            cycles_ += kMultExtra;
            break;
        }
        case 0x19: {  // MULTU
            const std::uint64_t p = (s & 0xFFFFFFFF) * (t & 0xFFFFFFFF);
            lo_ = sext32(p);
            hi_ = sext32(p >> 32);
            cycles_ += kMultExtra;
            break;
        }
        case 0x1A: {  // DIV
            const auto n = static_cast<std::int32_t>(s);
            const auto d = static_cast<std::int32_t>(t);
            if (d == 0) {  // no exception: LO = -1 or 1 by the dividend's sign, HI = dividend
                lo_ = n >= 0 ? ~std::uint64_t{0} : 1;
                hi_ = sext32(static_cast<std::uint32_t>(n));
            } else if (n == INT32_MIN && d == -1) {
                lo_ = sext32(static_cast<std::uint32_t>(INT32_MIN));
                hi_ = 0;
            } else {
                lo_ = sext32(static_cast<std::uint32_t>(n / d));
                hi_ = sext32(static_cast<std::uint32_t>(n % d));
            }
            cycles_ += kDivExtra;
            break;
        }
        case 0x1B: {  // DIVU
            const auto n = static_cast<std::uint32_t>(s);
            const auto d = static_cast<std::uint32_t>(t);
            lo_ = d == 0 ? ~std::uint64_t{0} : sext32(n / d);
            hi_ = d == 0 ? sext32(n) : sext32(n % d);
            cycles_ += kDivExtra;
            break;
        }
        case 0x1C: mul_s64(static_cast<std::int64_t>(s), static_cast<std::int64_t>(t), hi_, lo_); cycles_ += kDmultExtra; break;  // DMULT
        case 0x1D: mul_u64(s, t, hi_, lo_); cycles_ += kDmultExtra; break;  // DMULTU
        case 0x1E: {  // DDIV
            const auto n = static_cast<std::int64_t>(s);
            const auto d = static_cast<std::int64_t>(t);
            if (d == 0) {
                lo_ = n >= 0 ? ~std::uint64_t{0} : 1;
                hi_ = s;
            } else if (n == INT64_MIN && d == -1) {
                lo_ = s;
                hi_ = 0;
            } else {
                lo_ = static_cast<std::uint64_t>(n / d);
                hi_ = static_cast<std::uint64_t>(n % d);
            }
            cycles_ += kDdivExtra;
            break;
        }
        case 0x1F:  // DDIVU
            lo_ = t == 0 ? ~std::uint64_t{0} : s / t;
            hi_ = t == 0 ? s : s % t;
            cycles_ += kDdivExtra;
            break;
        case 0x20:  // ADD (overflow trap: see COP0)
        case 0x21: set_gpr(rd, sext32(s + t)); break;  // ADDU
        case 0x22:  // SUB
        case 0x23: set_gpr(rd, sext32(s - t)); break;  // SUBU
        case 0x24: set_gpr(rd, s & t); break;     // AND
        case 0x25: set_gpr(rd, s | t); break;     // OR
        case 0x26: set_gpr(rd, s ^ t); break;     // XOR
        case 0x27: set_gpr(rd, ~(s | t)); break;  // NOR
        case 0x2A: set_gpr(rd, static_cast<std::int64_t>(s) < static_cast<std::int64_t>(t) ? 1 : 0); break;  // SLT
        case 0x2B: set_gpr(rd, s < t ? 1 : 0); break;  // SLTU
        case 0x2C:  // DADD
        case 0x2D: set_gpr(rd, s + t); break;  // DADDU
        case 0x2E:  // DSUB
        case 0x2F: set_gpr(rd, s - t); break;  // DSUBU
        case 0x38: set_gpr(rd, t << sa); break;         // DSLL
        case 0x3A: set_gpr(rd, t >> sa); break;         // DSRL
        case 0x3B: set_gpr(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(t) >> sa)); break;  // DSRA
        case 0x3C: set_gpr(rd, t << (sa + 32)); break;  // DSLL32
        case 0x3E: set_gpr(rd, t >> (sa + 32)); break;  // DSRL32
        case 0x3F: set_gpr(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(t) >> (sa + 32))); break;  // DSRA32
        default:
            std::fprintf(stderr, "VR4300: unimplemented SPECIAL %02X at %016llX\n", instr & 0x3F,
                         static_cast<unsigned long long>(current_pc_));
            break;
    }
}

void Vr4300::execute_regimm(std::uint32_t instr) {
    const int rs = static_cast<int>((instr >> 21) & 31);
    const auto s = static_cast<std::int64_t>(gpr(rs));
    const std::uint64_t target = pc_ + (sext16(instr) << 2);
    const int rt = static_cast<int>((instr >> 16) & 31);
    // The -AL forms link even when the branch isn't taken.
    if (rt >= 0x10 && rt <= 0x13) set_gpr(31, pc_ + 4);
    switch (rt) {
        case 0x00: case 0x10: branch(s < 0, target, false); break;   // BLTZ, BLTZAL
        case 0x01: case 0x11: branch(s >= 0, target, false); break;  // BGEZ, BGEZAL
        case 0x02: case 0x12: branch(s < 0, target, true); break;    // BLTZL, BLTZALL
        case 0x03: case 0x13: branch(s >= 0, target, true); break;   // BGEZL, BGEZALL
        default:
            std::fprintf(stderr, "VR4300: unimplemented REGIMM %02X at %016llX\n", rt,
                         static_cast<unsigned long long>(current_pc_));
            break;
    }
}

}  // namespace ezy::n64
