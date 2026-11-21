// VR4300 JIT: the branches as native code, with the interpreter's exact results
// (Vr4300::branch and its callers in vr4300_ops.cpp):
//  - targets come from the virtual address of the delay slot (the block's kseg0 address);
//  - the links (JAL, JALR, the REGIMM -AL forms) are the branch's address + 8, written
//    after the compared or jumped-to register was read (JALR rd == rs jumps to the old
//    value), and by the -AL forms even when the branch is not taken;
//  - a taken branch to its own address arms idle-loop skipping (idle_candidate_);
//  - a likely branch not taken nullifies its delay slot: the block ends right after it.
// The branch only computes N, the next_pc_ it leaves; the builder stores the pc state
// (vr4300_jit_builder.hpp) before the slot's helper, on the slot's exits and at the end.
#include "n64/cpu/vr4300_jit_builder.hpp"

namespace ezy::n64 {

namespace {

using jit::Cond;
using jit::Value;

Cond inverse(Cond cond) {
    switch (cond) {
        case Cond::Eq: return Cond::Ne;
        case Cond::Ne: return Cond::Eq;
        case Cond::LtS: return Cond::GeS;
        case Cond::GeS: return Cond::LtS;
        case Cond::LeS: return Cond::GtS;
        case Cond::GtS: return Cond::LeS;
        case Cond::LtU: return Cond::GeU;
        case Cond::GeU: return Cond::LtU;
    }
    return Cond::Eq;
}

// A conditional branch's test: rs <cond> rt (two_regs) or rs <cond> 0. `fixed` when the
// outcome does not depend on the registers (BEQ r, r; BLTZ r0; ...), then `taken`.
struct Test {
    Cond cond = Cond::Eq;
    bool two_regs = false;
    bool fixed = false;
    bool taken = false;
};

Test test_of(std::uint32_t instr) {
    const std::uint32_t op = instr >> 26;
    const std::uint32_t rs = (instr >> 21) & 31, rt = (instr >> 16) & 31;
    Test t;
    if (op == 0x01) {  // REGIMM: bit 0 of rt picks BGEZ over BLTZ
        t.cond = (rt & 1) ? Cond::GeS : Cond::LtS;
    } else {
        switch (op & 3) {  // BEQ BNE BLEZ BGTZ, and their likely forms
            case 0: t.cond = Cond::Eq; t.two_regs = true; break;
            case 1: t.cond = Cond::Ne; t.two_regs = true; break;
            case 2: t.cond = Cond::LeS; break;
            default: t.cond = Cond::GtS; break;
        }
    }
    if (t.two_regs ? rs == rt : rs == 0) {  // x <cond> x
        t.fixed = true;
        t.taken = t.cond == Cond::Eq || t.cond == Cond::LeS || t.cond == Cond::GeS;
    }
    return t;
}

}  // namespace

bool emit_branch(BlockBuilder& b, std::uint32_t instr) {
    if (!is_branch(instr) || (instr >> 26) == 0x11) return false;  // BC1* runs in place
    jit::IrBuilder& ir = b.ir();
    const std::uint32_t op = instr >> 26;
    const int rs = static_cast<int>((instr >> 21) & 31);
    const int rt = static_cast<int>((instr >> 16) & 31);
    const int rd = static_cast<int>((instr >> 11) & 31);
    const std::uint64_t pc = b.address(), slot = pc + 4, link = pc + 8;

    if (op == 0x02 || op == 0x03) {  // J JAL
        const std::uint64_t target = (slot & ~std::uint64_t{0x0FFFFFFF}) | (std::uint64_t{instr & 0x3FFFFFF} << 2);
        if (op == 0x03) b.set_gpr(31, b.i64(link));
        if (target == pc) b.set_idle_candidate(b.i64(target));
        b.end_branch(target);
        return true;
    }
    if (op == 0x00) {  // JR JALR: any target, an unaligned one included (the next fetch raises AdEL)
        const Value target = b.gpr(rs);
        if ((instr & 0x3F) == 0x09) b.set_gpr(rd, b.i64(link));
        b.set_idle_candidate(ir.select(ir.compare(Cond::Eq, target, b.i64(pc)), target, b.idle_candidate()));
        b.end_branch(target);
        return true;
    }

    const bool likely = op >= 0x14 || (op == 0x01 && (rt & 2) != 0);
    const auto offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(instr & 0xFFFF)));
    const std::uint64_t target = slot + (offset << 2);
    const Test test = test_of(instr);
    const Value s = b.gpr(rs);
    const Value t = test.two_regs ? b.gpr(rt) : b.i64(0);
    if (op == 0x01 && (rt & 0x10) != 0) b.set_gpr(31, b.i64(link));  // -AL: even when not taken

    if (test.fixed) {
        if (test.taken) {
            if (target == pc) b.set_idle_candidate(b.i64(target));
            b.end_branch(target);
        } else {
            if (likely) b.skip_slot_if(b.i32(1));  // the rest of the block never runs
            b.end_branch(slot + 4);
        }
        return true;
    }
    if (likely) {
        b.skip_slot_if(ir.compare(inverse(test.cond), s, t));
        if (target == pc) b.set_idle_candidate(b.i64(target));  // taken from here on
        b.end_branch(target);
        return true;
    }
    const Value taken = ir.compare(test.cond, s, t);
    if (target == pc) b.set_idle_candidate(ir.select(taken, b.i64(target), b.idle_candidate()));
    b.end_branch(ir.select(taken, b.i64(target), b.i64(slot + 4)));
    return true;
}

}  // namespace ezy::n64
