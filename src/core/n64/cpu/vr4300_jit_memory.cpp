// VR4300 JIT: the aligned loads and stores as native code, with the interpreter's exact
// results (Vr4300::load/store and execute_load_store in vr4300.cpp):
//  - vaddr = base + the sign-extended offset, in 64 bits;
//  - only kseg0 inside direct RAM, aligned, runs natively: the CPU JIT runs kernel-mode
//    code only, where kseg0 is paddr = vaddr - 0xFFFFFFFF80000000, cached (no extra
//    cycles), and an aligned access never crosses a 4 KiB page. Anything else (kseg1, MMIO,
//    the TLB segments, an address error) leaves the block before the instruction, which
//    the interpreter then runs (and raises its exception);
//  - the threaded RDP's barrier comes first, inline and call-free (nothing without a
//    barrier view): when Vr4300::load/store's guard_memory() would have to look further
//    (the page has a newer packet than completed_seen, or the renderer failed), the block
//    leaves before the instruction, and the interpreter's step() then waits (or rethrows
//    the renderer's failure) and makes the access. That happens a few times per run, and
//    keeps calls, and the spills around them, out of the native code;
//  - LB LH LW sign-extend, LBU LHU LWU zero-extend, LD loads 64 bits; a load into $0
//    still touches memory;
//  - a store reports its range to the code watch (note_ram_write); when that dropped
//    compiled code, the block leaves right after the store, as the in-place path does.
// Vr4300::load() also sets last_paddr_, which is not serialized and is read only by
// LL/LLD; they stay in place and set it themselves, so native loads leave it alone.
#include "n64/cpu/vr4300_jit_builder.hpp"

#include <algorithm>

namespace ezy::n64 {

namespace {

using jit::Cond;
using jit::Op;
using jit::Type;
using jit::Value;

constexpr std::uint64_t kKseg0 = 0xFFFFFFFF80000000ULL;

struct NativeAccess {
    unsigned bytes = 0;  // 0: not a native access
    bool store = false;
    Op extend = Op::Const;  // a load's sign extension (Sext8/16/32), Op::Const for none
};

NativeAccess native_access(std::uint32_t instr, bool allow_64bit) {
    switch (instr >> 26) {
        case 0x20: return {1, false, Op::Sext8};   // LB
        case 0x24: return {1, false, Op::Const};   // LBU
        case 0x21: return {2, false, Op::Sext16};  // LH
        case 0x25: return {2, false, Op::Const};   // LHU
        case 0x23: return {4, false, Op::Sext32};  // LW
        case 0x28: return {1, true};               // SB
        case 0x29: return {2, true};               // SH
        case 0x2B: return {4, true};               // SW
        // The 64-bit ones raise RI without allow_64bit: in place then, like the ALU's.
        case 0x27: return allow_64bit ? NativeAccess{4, false, Op::Const} : NativeAccess{};  // LWU
        case 0x37: return allow_64bit ? NativeAccess{8, false, Op::Const} : NativeAccess{};  // LD
        case 0x3F: return allow_64bit ? NativeAccess{8, true} : NativeAccess{};              // SD
        default: return {};
    }
}

}  // namespace

bool is_native_memory(std::uint32_t instr, bool allow_64bit) { return native_access(instr, allow_64bit).bytes != 0; }

bool emit_memory(BlockBuilder& b, std::uint32_t instr) {
    const NativeAccess access = native_access(instr, b.allow_64bit());
    if (access.bytes == 0) return false;
    jit::IrBuilder& ir = b.ir();
    if (b.ram_size() < access.bytes) {  // never in RAM (a test bus; not an N64)
        b.exit_before_if(b.i32(1));
        return true;
    }
    const int base = static_cast<int>((instr >> 21) & 31);
    const int rt = static_cast<int>((instr >> 16) & 31);
    const auto offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(instr & 0xFFFF)));

    // paddr = vaddr - kseg0 = base + (offset - kseg0), in 64 bits; it must be at most RAM's
    // end - bytes (one unsigned compare also rejects every address outside kseg0's 512 MiB),
    // and aligned.
    const std::uint64_t end = std::min<std::uint64_t>(b.ram_size(), 0x20000000);
    const Value from_kseg0 = ir.binary(Op::Add, b.gpr(base), b.i64(offset - kKseg0));
    const Value paddr = ir.convert(Op::Trunc, Type::I32, from_kseg0);
    const jit::ExitId before = b.exit_before();
    ir.exit_if(ir.compare(Cond::GeU, from_kseg0, b.i64(end - access.bytes + 1)), before);
    if (access.bytes > 1) ir.exit_if(ir.binary(Op::And, paddr, b.i32(access.bytes - 1)), before);  // misaligned
    ir.barrier(paddr, access.bytes, access.store, before);

    if (!access.store) {
        Value value = ir.load_ram(Type::I64, paddr, access.bytes);  // zero-extended
        if (access.extend != Op::Const) value = ir.convert(access.extend, Type::I64, value);
        b.set_gpr(rt, value);
        return true;
    }
    ir.store_ram(paddr, b.gpr(rt), access.bytes);
    b.exit_after_if(ir.note_ram_write(paddr, access.bytes), 1);
    return true;
}

}  // namespace ezy::n64
