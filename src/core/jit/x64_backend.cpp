#include "jit/x64_backend.hpp"

#if EZY_JIT_AVAILABLE
#include <asmjit/x86.h>

#include <atomic>
#include <cstddef>
#include <vector>

#include "jit/code_buffer_impl.hpp"
#endif

namespace ezy::jit {

#if EZY_JIT_AVAILABLE
namespace {

using namespace asmjit;

static_assert(sizeof(std::atomic<bool>) == 1, "the barrier reads PageBarrierView::failed as one byte");

struct Errors final : ErrorHandler {
    bool failed = false;
    void handle_error(Error, const char*, BaseEmitter*) override { failed = true; }
};

x86::CondCode condition(Cond cond) {
    switch (cond) {
        case Cond::Eq: return x86::CondCode::kE;
        case Cond::Ne: return x86::CondCode::kNE;
        case Cond::LtS: return x86::CondCode::kL;
        case Cond::LtU: return x86::CondCode::kB;
        case Cond::LeS: return x86::CondCode::kLE;
        case Cond::GtS: return x86::CondCode::kG;
        case Cond::GeS: return x86::CondCode::kGE;
        case Cond::GeU: return x86::CondCode::kAE;
    }
    return x86::CondCode::kE;
}

constexpr std::int32_t field(std::size_t offset) { return static_cast<std::int32_t>(offset); }

// One virtual register per IR value; AsmJit's allocator assigns the host registers.
class Emitter {
public:
    Emitter(x86::Compiler& cc, const IrBlock& block) : cc_(cc), block_(block), values_(block.value_count()) {}

    bool emit() {
        for (std::size_t id = 0; id < block_.value_count(); ++id) {
            const auto type = block_.type_of(static_cast<std::uint32_t>(id));
            if (type != Type::I32 && type != Type::I64) return false;
        }
        auto* function = cc_.add_func(FuncSignature::build<std::uint32_t, Context*>());
        if (function == nullptr) return false;
        ctx_ = cc_.new_gp_ptr("ctx");
        function->set_arg(0, ctx_);
        bool guest = false, ram = false;
        const auto scan = [&](const std::vector<Inst>& ops) {
            for (const auto& inst : ops) {
                guest = guest || inst.op == Op::LoadGuest || inst.op == Op::StoreGuest;
                ram = ram || inst.op == Op::LoadRam || inst.op == Op::StoreRam;
            }
        };
        scan(block_.body());
        for (ExitId exit = 0; exit < block_.exit_count(); ++exit) scan(block_.exit_ops(exit));
        if (guest) {
            guest_ = cc_.new_gp_ptr("guest");
            cc_.mov(guest_, x86::qword_ptr(ctx_, field(offsetof(Context, guest))));
        }
        if (ram) {
            ram_ = cc_.new_gp_ptr("ram");
            cc_.mov(ram_, x86::qword_ptr(ctx_, field(offsetof(Context, ram))));
        }
        for (ExitId exit = 0; exit < block_.exit_count(); ++exit) exits_.push_back(cc_.new_label());
        for (const auto& inst : block_.body()) {
            if (!op(inst)) return false;
        }
        // Each exit region sits after the body: it runs its own ops and returns its id.
        for (ExitId exit = 0; exit < block_.exit_count(); ++exit) {
            cc_.bind(exits_[exit]);
            for (const auto& inst : block_.exit_ops(exit)) {
                if (!op(inst)) return false;
            }
            auto id = cc_.new_gp32();
            cc_.mov(id, Imm(exit));
            cc_.ret(id);
        }
        cc_.end_func();
        return true;
    }

private:
    x86::Gp temp(Type type) { return type == Type::I64 ? cc_.new_gp64() : cc_.new_gp32(); }

    x86::Gp& def(const Inst& inst) { return values_[inst.result] = temp(inst.type); }

    const x86::Gp& v(std::uint32_t id) const { return values_[id]; }

    x86::Mem sized(const x86::Gp& base, const x86::Gp& index, Type type) const {
        return type == Type::I64 ? x86::qword_ptr(base, index) : x86::dword_ptr(base, index);
    }

    x86::Mem guest(const Inst& inst, Type type) const {
        const auto offset = static_cast<std::int32_t>(inst.imm);
        return type == Type::I64 ? x86::qword_ptr(guest_, offset) : x86::dword_ptr(guest_, offset);
    }

    void store_guest(const Inst& inst) {
        const auto offset = static_cast<std::int32_t>(inst.imm);
        const auto& value = v(inst.a);
        switch (inst.aux) {
            case 1: cc_.mov(x86::byte_ptr(guest_, offset), value.r8()); break;
            case 2: cc_.mov(x86::word_ptr(guest_, offset), value.r16()); break;
            case 4: cc_.mov(x86::dword_ptr(guest_, offset), value.r32()); break;
            default: cc_.mov(guest(inst, inst.type), value); break;  // 0 or the whole I64
        }
    }

    // Zero-extended physical address, usable as a 64-bit index.
    x86::Gp index(std::uint32_t paddr) {
        auto index = cc_.new_gp64();
        cc_.mov(index.r32(), v(paddr));
        return index;
    }

    x86::Gp page(std::uint32_t paddr) {
        auto page = index(paddr);
        cc_.shr(page.r32(), 12);
        return page;
    }

    bool op(const Inst& inst) {
        switch (inst.op) {
            case Op::Const: {
                auto& d = def(inst);
                if (inst.imm == 0) cc_.xor_(d.r32(), d.r32());
                else cc_.mov(d, Imm(inst.imm));
                return true;
            }
            case Op::LoadGuest: cc_.mov(def(inst), guest(inst, inst.type)); return true;
            case Op::StoreGuest: store_guest(inst); return true;
            case Op::LoadContext:
                cc_.mov(def(inst), x86::qword_ptr(ctx_, field(context_offset(static_cast<ContextField>(inst.aux)))));
                return true;
            case Op::StoreContext:
                cc_.mov(x86::qword_ptr(ctx_, field(context_offset(static_cast<ContextField>(inst.aux)))), v(inst.a));
                return true;
            case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: case Op::Nor:
            case Op::Shl: case Op::Shr: case Op::Sar: case Op::Mul:
                binary(inst);
                return true;
            case Op::MulHiS: case Op::MulHiU: mul_high(inst); return true;
            case Op::Sext8: case Op::Sext16: case Op::Sext32: case Op::Zext32: case Op::Trunc:
                convert(inst);
                return true;
            case Op::Compare: {
                auto& d = def(inst);
                cc_.xor_(d, d);
                cc_.cmp(v(inst.a), v(inst.b));
                cc_.set(condition(static_cast<Cond>(inst.aux)), d.r8());
                return true;
            }
            case Op::Select: {
                auto& d = def(inst);
                cc_.mov(d, v(inst.c));
                cc_.test(v(inst.a), v(inst.a));
                cc_.cmovnz(d, v(inst.b));
                return true;
            }
            case Op::OverflowAdd: case Op::OverflowSub: case Op::CarryAdd: flag(inst); return true;
            case Op::LoadRam: load_ram(inst); return true;
            case Op::StoreRam: store_ram(inst); return true;
            case Op::Barrier: return barrier(inst);
            case Op::NoteRamWrite: return note_ram_write(inst);
            case Op::Call: return call(inst);
            case Op::ExitIf:
                cc_.test(v(inst.a), v(inst.a));
                cc_.jnz(exits_[inst.imm]);
                return true;
            case Op::Exit: cc_.jmp(exits_[inst.imm]); return true;
        }
        return false;
    }

    void binary(const Inst& inst) {
        auto& d = def(inst);
        const auto& a = v(inst.a);
        const auto& b = v(inst.b);
        if (inst.op == Op::Shl || inst.op == Op::Shr || inst.op == Op::Sar) {
            // x86 masks a CL count by 31 or 63: the IR's width-1 mask.
            auto count = cc_.new_gp32();
            cc_.mov(count, b.r32());
            cc_.mov(d, a);
            if (inst.op == Op::Shl) cc_.shl(d, count.r8());
            else if (inst.op == Op::Shr) cc_.shr(d, count.r8());
            else cc_.sar(d, count.r8());
            return;
        }
        cc_.mov(d, a);
        switch (inst.op) {
            case Op::Add: cc_.add(d, b); break;
            case Op::Sub: cc_.sub(d, b); break;
            case Op::And: cc_.and_(d, b); break;
            case Op::Or: cc_.or_(d, b); break;
            case Op::Xor: cc_.xor_(d, b); break;
            case Op::Nor: cc_.or_(d, b); cc_.not_(d); break;
            case Op::Mul: cc_.imul(d, b); break;
            default: break;
        }
    }

    void mul_high(const Inst& inst) {
        auto& d = def(inst);
        const bool is_signed = inst.op == Op::MulHiS;
        if (inst.type == Type::I64) {
            auto low = cc_.new_gp64();
            cc_.mov(low, v(inst.a));
            if (is_signed) cc_.imul(d, low, v(inst.b));  // rdx:rax <- rax * b
            else cc_.mul(d, low, v(inst.b));
            return;
        }
        // 32 bits: the full product fits a 64-bit register.
        auto x = cc_.new_gp64(), y = cc_.new_gp64();
        if (is_signed) {
            cc_.movsxd(x, v(inst.a));
            cc_.movsxd(y, v(inst.b));
        } else {
            cc_.mov(x.r32(), v(inst.a));
            cc_.mov(y.r32(), v(inst.b));
        }
        cc_.imul(x, y);
        if (is_signed) cc_.sar(x, 32);
        else cc_.shr(x, 32);
        cc_.mov(d, x.r32());
    }

    void convert(const Inst& inst) {
        auto& d = def(inst);
        const auto& s = v(inst.a);
        switch (inst.op) {
            case Op::Sext8: cc_.movsx(d, s.r8()); break;
            case Op::Sext16: cc_.movsx(d, s.r16()); break;
            case Op::Sext32: cc_.movsxd(d, s.r32()); break;
            case Op::Zext32: cc_.mov(d.r32(), s.r32()); break;  // a 32-bit write clears the upper half
            case Op::Trunc: cc_.mov(d, s.r32()); break;
            default: break;
        }
    }

    void flag(const Inst& inst) {
        auto& d = def(inst);
        auto scratch = temp(block_.type_of(inst.a));
        cc_.xor_(d, d);
        cc_.mov(scratch, v(inst.a));
        if (inst.op == Op::OverflowSub) cc_.sub(scratch, v(inst.b));
        else cc_.add(scratch, v(inst.b));
        if (inst.op == Op::CarryAdd) cc_.setc(d.r8());
        else cc_.seto(d.r8());
    }

    void load_ram(const Inst& inst) {
        auto& d = def(inst);
        const auto at = index(inst.a);
        switch (inst.aux) {
            case 1: cc_.movzx(d.r32(), x86::byte_ptr(ram_, at)); break;
            case 2:
                cc_.movzx(d.r32(), x86::word_ptr(ram_, at));
                cc_.ror(d.r16(), 8);
                break;
            case 4:
                cc_.mov(d.r32(), x86::dword_ptr(ram_, at));
                cc_.bswap(d.r32());
                break;
            default:
                cc_.mov(d, x86::qword_ptr(ram_, at));
                cc_.bswap(d);
                break;
        }
    }

    void store_ram(const Inst& inst) {
        const auto at = index(inst.a);
        const auto& value = v(inst.b);
        if (inst.aux == 1) {
            cc_.mov(x86::byte_ptr(ram_, at), value.r8());
            return;
        }
        auto swapped = inst.aux == 8 ? cc_.new_gp64() : cc_.new_gp32();
        cc_.mov(swapped, inst.aux == 8 ? value : value.r32());
        if (inst.aux == 2) {
            cc_.ror(swapped.r16(), 8);
            cc_.mov(x86::word_ptr(ram_, at), swapped.r16());
            return;
        }
        cc_.bswap(swapped);
        cc_.mov(sized(ram_, at, inst.aux == 8 ? Type::I64 : Type::I32), swapped);
    }

    // The renderer's memory barrier, with no call: the block leaves through the barrier's
    // exit when the page has a newer packet than completed_seen, or the renderer failed.
    bool barrier(const Inst& inst) {
        const bool write = (inst.imm & 1) != 0;
        const Label slow = exits_[inst.imm >> Inst::kBarrierExitShift];
        const auto done = cc_.new_label();
        auto view = cc_.new_gp_ptr();
        cc_.mov(view, x86::qword_ptr(ctx_, field(offsetof(Context, barrier))));
        cc_.test(view, view);
        cc_.jz(done);
        const auto at = page(inst.a);
        auto table = cc_.new_gp_ptr(), sequences = cc_.new_gp_ptr();
        auto slot = cc_.new_gp64(), sequence = cc_.new_gp64();
        cc_.mov(sequences, x86::qword_ptr(view, field(offsetof(PageBarrierView, slot_sequence))));
        cc_.mov(table, x86::qword_ptr(view, field(offsetof(PageBarrierView, writers))));
        cc_.movzx(slot.r32(), x86::word_ptr(table, at, 1));
        cc_.mov(sequence, x86::qword_ptr(sequences, slot, 3));
        if (write) {
            auto reader = cc_.new_gp64();
            cc_.mov(table, x86::qword_ptr(view, field(offsetof(PageBarrierView, readers))));
            cc_.movzx(slot.r32(), x86::word_ptr(table, at, 1));
            cc_.mov(reader, x86::qword_ptr(sequences, slot, 3));
            cc_.cmp(reader, sequence);
            cc_.cmova(sequence, reader);
        }
        cc_.mov(table, x86::qword_ptr(view, field(offsetof(PageBarrierView, completed_seen))));
        cc_.cmp(sequence, x86::qword_ptr(table));
        cc_.ja(slow);
        cc_.mov(table, x86::qword_ptr(view, field(offsetof(PageBarrierView, failed))));
        cc_.cmp(x86::byte_ptr(table), 0);  // an acquire load is a plain load on x86-64
        cc_.jne(slow);
        cc_.bind(done);
        return true;
    }

    bool note_ram_write(const Inst& inst) {
        auto& d = def(inst);
        const auto done = cc_.new_label();
        auto pages = cc_.new_gp_ptr();
        cc_.xor_(d, d);
        cc_.mov(pages, x86::qword_ptr(ctx_, field(offsetof(Context, code_pages))));
        const auto at = page(inst.a);
        cc_.cmp(x86::byte_ptr(pages, at), 0);
        cc_.je(done);
        auto code_write = cc_.new_gp_ptr();
        cc_.mov(code_write, x86::qword_ptr(ctx_, field(offsetof(Context, code_write))));
        InvokeNode* node = nullptr;
        cc_.invoke(Out(node), code_write, FuncSignature::build<std::uint32_t, Context*, std::uint32_t, std::uint32_t>());
        if (node == nullptr) return false;
        node->set_arg(0, ctx_);
        node->set_arg(1, v(inst.a));
        node->set_arg(2, Imm(inst.aux));
        node->set_ret(0, d);
        cc_.bind(done);
        return true;
    }

    bool call(const Inst& inst) {
        auto& d = def(inst);
        auto helper = cc_.new_gp_ptr();
        cc_.mov(helper, Imm(inst.imm));
        InvokeNode* node = nullptr;
        cc_.invoke(Out(node), helper,
                   FuncSignature::build<std::uint64_t, Context*, std::uint64_t, std::uint64_t, std::uint64_t>());
        if (node == nullptr) return false;
        node->set_arg(0, ctx_);
        node->set_arg(1, v(inst.a));
        node->set_arg(2, v(inst.b));
        node->set_arg(3, v(inst.c));
        node->set_ret(0, d);
        return true;
    }

    x86::Compiler& cc_;
    const IrBlock& block_;
    std::vector<x86::Gp> values_;
    std::vector<Label> exits_;
    x86::Gp ctx_, guest_, ram_;
};

}  // namespace

namespace detail {
// x64_assembler.cpp: the Assembler backend.
bool assemble(asmjit::CodeHolder& code, const IrBlock& block);
}  // namespace detail
#endif

Compiled compile(const IrBlock& block, CodeBuffer& buffer, Backend backend) {
#if EZY_JIT_AVAILABLE
    if (!buffer.available()) return {nullptr, CompileError::Unsupported};
    auto& impl = *buffer.impl_;
    asmjit::CodeHolder code;
    Errors errors;
    if (code.init(impl.runtime.environment(), impl.runtime.cpu_features()) != asmjit::Error::kOk) {
        return {nullptr, CompileError::Unsupported};
    }
    code.set_error_handler(&errors);
    if (backend == Backend::Assembler) {
        if (!detail::assemble(code, block) || errors.failed) return {nullptr, CompileError::Unsupported};
    } else {
        asmjit::x86::Compiler cc(&code);
        if (!Emitter(cc, block).emit() || errors.failed) return {nullptr, CompileError::Unsupported};
        if (cc.finalize() != asmjit::Error::kOk || errors.failed) return {nullptr, CompileError::Unsupported};
    }
    const auto size = code.code_size();
    if (size > impl.budget - impl.used) return {nullptr, CompileError::OutOfSpace};
    BlockFn fn = nullptr;
    const auto added = impl.runtime.add(&fn, &code);
    if (added == asmjit::Error::kOutOfMemory) return {nullptr, CompileError::OutOfSpace};
    if (added != asmjit::Error::kOk || fn == nullptr) return {nullptr, CompileError::Unsupported};
    impl.used += size;
    return {fn, CompileError::None};
#else
    (void)block;
    (void)buffer;
    (void)backend;
    return {nullptr, CompileError::Unsupported};
#endif
}

}  // namespace ezy::jit
