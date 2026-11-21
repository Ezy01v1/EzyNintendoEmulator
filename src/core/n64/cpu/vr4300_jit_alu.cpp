// VR4300 JIT: the integer ALU as native code, with the interpreter's exact results
// (vr4300_ops.cpp): 32-bit ops sign-extend their result to 64 bits, MULT/MULTU leave both
// halves sign-extended in LO/HI, and the trapping adds/subtracts leave the block before
// themselves on overflow, so that the interpreter raises Ov.
#include "n64/cpu/vr4300_jit_builder.hpp"

namespace ezy::n64 {

namespace {

using jit::Cond;
using jit::Op;
using jit::Type;
using jit::Value;

constexpr std::uint32_t kMultCycles = 1 + 5;   // kMultExtra
constexpr std::uint32_t kDmultCycles = 1 + 8;  // kDmultExtra

std::uint32_t special_cycles(std::uint32_t funct, bool allow_64bit) {
    switch (funct) {
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:  // SLL SRL SRA SLLV SRLV SRAV
        case 0x10: case 0x11: case 0x12: case 0x13:                        // MFHI MTHI MFLO MTLO
        case 0x20: case 0x21: case 0x22: case 0x23:                        // ADD ADDU SUB SUBU
        case 0x24: case 0x25: case 0x26: case 0x27:                        // AND OR XOR NOR
        case 0x2A: case 0x2B:                                              // SLT SLTU
            return 1;
        case 0x18: case 0x19: return kMultCycles;  // MULT MULTU
        case 0x14: case 0x16: case 0x17:           // DSLLV DSRLV DSRAV
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:  // DADD DADDU DSUB DSUBU
        case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:  // DSLL DSRL DSRA, the -32 forms
            return allow_64bit ? 1 : 0;
        case 0x1C: case 0x1D: return allow_64bit ? kDmultCycles : 0;  // DMULT DMULTU
        default: return 0;  // DIV/DDIV and family, branches, traps, SYSCALL, BREAK, SYNC, RI
    }
}

}  // namespace

std::uint32_t alu_cycles(std::uint32_t instr, bool allow_64bit) {
    switch (instr >> 26) {
        case 0x00: return special_cycles(instr & 0x3F, allow_64bit);
        case 0x08: case 0x09: case 0x0A: case 0x0B:  // ADDI ADDIU SLTI SLTIU
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:  // ANDI ORI XORI LUI
            return 1;
        case 0x18: case 0x19: return allow_64bit ? 1 : 0;  // DADDI DADDIU
        default: return 0;
    }
}

bool emit_alu(BlockBuilder& b, std::uint32_t instr) {
    if (alu_cycles(instr, b.allow_64bit()) == 0) return false;
    jit::IrBuilder& ir = b.ir();
    const int rs = static_cast<int>((instr >> 21) & 31);
    const int rt = static_cast<int>((instr >> 16) & 31);
    const int rd = static_cast<int>((instr >> 11) & 31);
    const std::uint32_t sa = (instr >> 6) & 31;
    const auto imm = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(instr & 0xFFFF)));
    const std::uint64_t zimm = instr & 0xFFFF;

    const auto bin = [&ir](Op op, Value x, Value y) { return ir.binary(op, x, y); };
    const auto sext32 = [&ir](Value v) { return ir.convert(Op::Sext32, Type::I64, v); };  // of the low 32 bits
    const auto zext32 = [&ir](Value v) { return ir.convert(Op::Zext32, Type::I64, v); };
    const auto low32 = [&ir](Value v) { return ir.convert(Op::Trunc, Type::I32, v); };
    const auto flag = [&](Cond cond, Value x, Value y) { return zext32(ir.compare(cond, x, y)); };
    // The trapping forms check before writing; their $0 forms still trap.
    const auto checked = [&](Op op, Value x, Value y) { b.exit_before_if(ir.overflow(op, x, y)); };

    if ((instr >> 26) == 0x00) {
        const std::uint32_t funct = instr & 0x3F;
        switch (funct) {
            case 0x11: b.set_hi(b.gpr(rs)); return true;  // MTHI
            case 0x13: b.set_lo(b.gpr(rs)); return true;  // MTLO
            case 0x18: {                                  // MULT
                const Value p = bin(Op::Mul, sext32(b.gpr(rs)), sext32(b.gpr(rt)));
                b.set_lo(sext32(p));
                b.set_hi(bin(Op::Sar, p, b.i64(32)));  // = sext32(p >> 32)
                return true;
            }
            case 0x19: {  // MULTU
                const Value p = bin(Op::Mul, zext32(b.gpr(rs)), zext32(b.gpr(rt)));
                b.set_lo(sext32(p));
                b.set_hi(sext32(bin(Op::Shr, p, b.i64(32))));
                return true;
            }
            case 0x1C: case 0x1D: {  // DMULT DMULTU: the low half is the same for both
                const Value s = b.gpr(rs), t = b.gpr(rt);
                b.set_lo(bin(Op::Mul, s, t));
                b.set_hi(bin(funct == 0x1C ? Op::MulHiS : Op::MulHiU, s, t));
                return true;
            }
            case 0x20: case 0x22: {  // ADD SUB
                const Op op = funct == 0x20 ? Op::Add : Op::Sub;
                const Value s = low32(b.gpr(rs)), t = low32(b.gpr(rt));
                checked(op, s, t);
                if (rd != 0) b.set_gpr(rd, sext32(bin(op, s, t)));
                return true;
            }
            case 0x2C: case 0x2E: {  // DADD DSUB
                const Op op = funct == 0x2C ? Op::Add : Op::Sub;
                const Value s = b.gpr(rs), t = b.gpr(rt);
                checked(op, s, t);
                if (rd != 0) b.set_gpr(rd, bin(op, s, t));
                return true;
            }
            default: break;
        }
        if (rd == 0) return true;  // the rest only write rd
        Value v;
        switch (funct) {
            case 0x00: v = sext32(bin(Op::Shl, b.gpr(rt), b.i64(sa))); break;                    // SLL
            case 0x02: v = sext32(bin(Op::Shr, low32(b.gpr(rt)), b.i32(sa))); break;             // SRL
            case 0x03: v = sext32(bin(Op::Sar, b.gpr(rt), b.i64(sa))); break;                    // SRA (64-bit shift)
            case 0x04: v = sext32(bin(Op::Shl, low32(b.gpr(rt)), low32(b.gpr(rs)))); break;      // SLLV (amount & 31)
            case 0x06: v = sext32(bin(Op::Shr, low32(b.gpr(rt)), low32(b.gpr(rs)))); break;      // SRLV
            case 0x07:                                                                           // SRAV (64-bit shift)
                v = sext32(bin(Op::Sar, b.gpr(rt), bin(Op::And, b.gpr(rs), b.i64(31))));
                break;
            case 0x10: v = b.hi(); break;  // MFHI
            case 0x12: v = b.lo(); break;  // MFLO
            case 0x14: v = bin(Op::Shl, b.gpr(rt), b.gpr(rs)); break;  // DSLLV (amount & 63)
            case 0x16: v = bin(Op::Shr, b.gpr(rt), b.gpr(rs)); break;  // DSRLV
            case 0x17: v = bin(Op::Sar, b.gpr(rt), b.gpr(rs)); break;  // DSRAV
            case 0x21: v = sext32(bin(Op::Add, b.gpr(rs), b.gpr(rt))); break;  // ADDU
            case 0x23: v = sext32(bin(Op::Sub, b.gpr(rs), b.gpr(rt))); break;  // SUBU
            case 0x24: v = bin(Op::And, b.gpr(rs), b.gpr(rt)); break;
            case 0x25: v = bin(Op::Or, b.gpr(rs), b.gpr(rt)); break;
            case 0x26: v = bin(Op::Xor, b.gpr(rs), b.gpr(rt)); break;
            case 0x27: v = bin(Op::Nor, b.gpr(rs), b.gpr(rt)); break;
            case 0x2A: v = flag(Cond::LtS, b.gpr(rs), b.gpr(rt)); break;  // SLT
            case 0x2B: v = flag(Cond::LtU, b.gpr(rs), b.gpr(rt)); break;  // SLTU
            case 0x2D: v = bin(Op::Add, b.gpr(rs), b.gpr(rt)); break;     // DADDU
            case 0x2F: v = bin(Op::Sub, b.gpr(rs), b.gpr(rt)); break;     // DSUBU
            case 0x38: v = bin(Op::Shl, b.gpr(rt), b.i64(sa)); break;       // DSLL
            case 0x3A: v = bin(Op::Shr, b.gpr(rt), b.i64(sa)); break;       // DSRL
            case 0x3B: v = bin(Op::Sar, b.gpr(rt), b.i64(sa)); break;       // DSRA
            case 0x3C: v = bin(Op::Shl, b.gpr(rt), b.i64(sa + 32)); break;  // DSLL32
            case 0x3E: v = bin(Op::Shr, b.gpr(rt), b.i64(sa + 32)); break;  // DSRL32
            case 0x3F: v = bin(Op::Sar, b.gpr(rt), b.i64(sa + 32)); break;  // DSRA32
            default: return false;  // not reached: alu_cycles accepted it
        }
        b.set_gpr(rd, v);
        return true;
    }

    switch (instr >> 26) {
        case 0x08: {  // ADDI
            const Value s = low32(b.gpr(rs)), t = b.i32(static_cast<std::uint32_t>(imm));
            checked(Op::Add, s, t);
            if (rt != 0) b.set_gpr(rt, sext32(bin(Op::Add, s, t)));
            return true;
        }
        case 0x18: {  // DADDI
            const Value s = b.gpr(rs), t = b.i64(imm);
            checked(Op::Add, s, t);
            if (rt != 0) b.set_gpr(rt, bin(Op::Add, s, t));
            return true;
        }
        default: break;
    }
    if (rt == 0) return true;  // the rest only write rt
    Value v;
    switch (instr >> 26) {
        case 0x09: v = sext32(bin(Op::Add, b.gpr(rs), b.i64(imm))); break;  // ADDIU
        case 0x0A: v = flag(Cond::LtS, b.gpr(rs), b.i64(imm)); break;       // SLTI
        case 0x0B: v = flag(Cond::LtU, b.gpr(rs), b.i64(imm)); break;       // SLTIU (sign-extended immediate)
        case 0x0C: v = bin(Op::And, b.gpr(rs), b.i64(zimm)); break;         // ANDI
        case 0x0D: v = bin(Op::Or, b.gpr(rs), b.i64(zimm)); break;          // ORI
        case 0x0E: v = bin(Op::Xor, b.gpr(rs), b.i64(zimm)); break;         // XORI
        case 0x0F:                                                          // LUI
            v = b.i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(zimm << 16))));
            break;
        case 0x19: v = bin(Op::Add, b.gpr(rs), b.i64(imm)); break;  // DADDIU
        default: return false;  // not reached
    }
    b.set_gpr(rt, v);
    return true;
}

}  // namespace ezy::n64
