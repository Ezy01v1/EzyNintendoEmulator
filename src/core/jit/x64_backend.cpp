// The x86-64 backend: our own linear register allocator (reg_alloc.hpp) feeding an
// x86::Assembler. (AsmJit's x86::Compiler and its allocator made compiling expensive.)
#include "jit/x64_backend.hpp"

#if EZY_JIT_AVAILABLE
#include <asmjit/x86.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

#include "jit/code_buffer_impl.hpp"
#include "jit/reg_alloc.hpp"
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

// Host registers. Pinned: the context, the guest base and the RAM base. Allocatable in this
// order (the callee-saved first; r10/r11 are volatile). Scratch, never allocated: rax, rcx,
// rdx, r8 and r9 (shift counts, mul/imul, call arguments and results).
constexpr x86::Gp kCtx = x86::r15;
constexpr x86::Gp kGuest = x86::r14;
constexpr x86::Gp kRam = x86::r13;
constexpr unsigned kAllocatable = 7;
constexpr std::uint32_t kVolatileMask = 0b110'0000U;

constexpr std::array<x86::Gp, kAllocatable> kRegisters{x86::rbx, x86::rsi, x86::rdi, x86::rbp,
                                                       x86::r12, x86::r10, x86::r11};

x86::Gp host(unsigned index) { return kRegisters[index]; }

x86::Gp sized(const x86::Gp& reg, Type type) { return type == Type::I64 ? reg.r64() : reg.r32(); }

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

constexpr std::int32_t kShadowSpace = 32;
constexpr std::int32_t kPage = 4096;

// Code layout, by section order: the prologue (.text, emitted last, when the registers and
// slots are known), the body ending in its own exit, the shared epilogue right after it,
// then one copy of an exit's ops per jump site.
class Emitter {
public:
    Emitter(CodeHolder& code, x86::Assembler& a, const IrBlock& block)
        : code_(code), a_(a), block_(block), uses_(block), alloc_(kAllocatable, kVolatileMask, uses_) {}

    bool emit() {
        for (std::size_t id = 0; id < block_.value_count(); ++id) {
            const auto type = block_.type_of(static_cast<std::uint32_t>(id));
            if (type != Type::I32 && type != Type::I64) return false;
        }
        scan(block_.body());
        for (ExitId exit = 0; exit < block_.exit_count(); ++exit) scan(block_.exit_ops(exit));

        Section* body = nullptr;
        Section* tail = nullptr;
        Section* sites = nullptr;
        constexpr auto flags = SectionFlags::kExecutable | SectionFlags::kReadOnly;
        if (code_.new_section(Out(body), ".body", SIZE_MAX, flags, 1, 1) != Error::kOk ||
            code_.new_section(Out(tail), ".tail", SIZE_MAX, flags, 1, 2) != Error::kOk ||
            code_.new_section(Out(sites), ".sites", SIZE_MAX, flags, 1, 3) != Error::kOk) {
            return false;
        }
        epilogue_ = a_.new_label();

        a_.section(body);
        const auto& ops = block_.body();
        for (std::uint32_t p = 0; p < ops.size(); ++p) {
            if (!op(ops[p], p)) return false;
            if (ops[p].op != Op::Exit) alloc_.release_dead(p + 1);
        }
        // Op::Exit emitted its exit inline: it falls through into the epilogue.

        a_.section(sites);
        for (const auto& site : sites_) {
            a_.bind(site.label);
            alloc_.restore(site.state);
            if (!emit_exit(site.exit)) return false;
            a_.jmp(epilogue_);
        }

        // Callee-saved registers the block touched, and the frame.
        std::vector<x86::Gp> saved;
        for (unsigned r = 0; r < kAllocatable; ++r) {
            const auto bit = 1U << r;
            if ((alloc_.used_registers() & bit) != 0 && (kVolatileMask & bit) == 0) saved.push_back(host(r));
        }
        if (ram_) saved.push_back(kRam);
        if (guest_) saved.push_back(kGuest);
        if (ctx_) saved.push_back(kCtx);
        std::int32_t frame = 0;
        if (calls_ || alloc_.slot_count() != 0) {
            frame = kShadowSpace + 8 * static_cast<std::int32_t>(alloc_.slot_count());
            // rsp is 8 mod 16 at entry (the return address); 16-byte aligned at every call.
            if ((8 + 8 * static_cast<std::int32_t>(saved.size()) + frame) % 16 != 0) frame += 8;
        }

        a_.section(tail);
        a_.bind(epilogue_);
        if (frame != 0) a_.add(x86::rsp, frame);
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) a_.pop(*it);
        a_.ret();

        a_.section(code_.text_section());
        for (const auto& reg : saved) a_.push(reg);
        // Windows commits the stack one guard page at a time: touch each page of a large frame.
        for (std::int32_t probe = kPage; probe < frame; probe += kPage) {
            a_.mov(x86::eax, x86::dword_ptr(x86::rsp, -probe));
        }
        if (frame != 0) a_.sub(x86::rsp, frame);
        if (ram_) a_.mov(kRam, x86::qword_ptr(x86::rcx, field(offsetof(Context, ram))));
        if (guest_) a_.mov(kGuest, x86::qword_ptr(x86::rcx, field(offsetof(Context, guest))));
        if (ctx_) a_.mov(kCtx, x86::rcx);
        // Falls through into .body.
        return true;
    }

private:
    struct Site {
        Label label;
        ExitId exit;
        RegAlloc::Snapshot state;
    };

    void scan(const std::vector<Inst>& ops) {
        for (const auto& inst : ops) {
            switch (inst.op) {
                case Op::LoadGuest: case Op::StoreGuest: guest_ = true; break;
                case Op::LoadRam: case Op::StoreRam: ram_ = true; break;
                case Op::LoadContext: case Op::StoreContext: case Op::Barrier: ctx_ = true; break;
                case Op::NoteRamWrite: case Op::Call: ctx_ = calls_ = true; break;
                default: break;
            }
        }
    }

    static x86::Mem slot(std::uint32_t index) {
        return x86::qword_ptr(x86::rsp, kShadowSpace + 8 * static_cast<std::int32_t>(index));
    }

    void flush() {
        for (const auto& move : moves_) {
            using Kind = Location::Kind;
            if (move.from.kind == Kind::Register && move.to.kind == Kind::Slot) {
                a_.mov(slot(move.to.index), host(move.from.index));
            } else if (move.from.kind == Kind::Slot) {
                a_.mov(host(move.to.index), slot(move.from.index));
            } else {
                a_.mov(host(move.to.index), host(move.from.index));
            }
        }
        moves_.clear();
    }

    // A register holding the operand (64-bit view).
    x86::Gp use(std::uint32_t value, std::uint32_t position) {
        const unsigned r = alloc_.get(value, position, moves_);
        flush();
        return host(r);
    }

    // The result's register, at the result type's width. Distinct from every operand fetched
    // with use() for the same op: those stay locked until release_dead().
    x86::Gp def(const Inst& inst, std::uint32_t position) {
        const unsigned r = alloc_.define(inst.result, position, moves_);
        flush();
        return sized(host(r), inst.type);
    }

    // A call argument or a copy, from wherever the value is.
    bool load(const x86::Gp& to, std::uint32_t value) {
        const auto at = alloc_.where(value);
        if (at.kind == Location::Kind::Register) a_.mov(to, host(at.index));
        else if (at.kind == Location::Kind::Slot) a_.mov(to, slot(at.index));
        else return false;
        return true;
    }

    void jump_site(const Label& label, ExitId exit) { sites_.push_back({label, exit, alloc_.snapshot()}); }

    // An exit's ops from the allocator's current state, then its id in eax.
    bool emit_exit(ExitId exit) {
        const auto begin = uses_.exit_begin(exit);
        alloc_.set_region_end(uses_.exit_end(exit));
        alloc_.release_dead(begin);
        const auto& ops = block_.exit_ops(exit);
        for (std::uint32_t j = 0; j < ops.size(); ++j) {
            if (!op(ops[j], begin + j)) return false;
            alloc_.release_dead(begin + j + 1);
        }
        a_.mov(x86::eax, Imm(exit));
        return true;
    }

    x86::Mem guest(const Inst& inst, unsigned bytes) const {
        const auto offset = static_cast<std::int32_t>(inst.imm);
        switch (bytes) {
            case 1: return x86::byte_ptr(kGuest, offset);
            case 2: return x86::word_ptr(kGuest, offset);
            case 4: return x86::dword_ptr(kGuest, offset);
            default: return x86::qword_ptr(kGuest, offset);
        }
    }

    bool op(const Inst& inst, std::uint32_t p) {
        switch (inst.op) {
            case Op::Const: {
                const auto d = def(inst, p);
                if (inst.imm == 0) a_.xor_(d.r32(), d.r32());
                else if (inst.imm <= 0xFFFF'FFFFULL) a_.mov(d.r32(), Imm(inst.imm));  // zero-extends
                else a_.mov(d.r64(), Imm(inst.imm));
                return true;
            }
            case Op::LoadGuest: {
                const auto d = def(inst, p);
                a_.mov(d, guest(inst, inst.type == Type::I64 ? 8 : 4));
                return true;
            }
            case Op::StoreGuest: {
                const auto v = use(inst.a, p);
                switch (inst.aux) {
                    case 1: a_.mov(guest(inst, 1), v.r8()); break;
                    case 2: a_.mov(guest(inst, 2), v.r16()); break;
                    case 4: a_.mov(guest(inst, 4), v.r32()); break;
                    default: a_.mov(guest(inst, inst.type == Type::I64 ? 8 : 4), sized(v, inst.type)); break;
                }
                return true;
            }
            case Op::LoadContext: {
                const auto d = def(inst, p);
                a_.mov(d, x86::qword_ptr(kCtx, field(context_offset(static_cast<ContextField>(inst.aux)))));
                return true;
            }
            case Op::StoreContext: {
                const auto v = use(inst.a, p);
                a_.mov(x86::qword_ptr(kCtx, field(context_offset(static_cast<ContextField>(inst.aux)))), v);
                return true;
            }
            case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: case Op::Nor:
            case Op::Shl: case Op::Shr: case Op::Sar: case Op::Mul:
                binary(inst, p);
                return true;
            case Op::MulHiS: case Op::MulHiU: mul_high(inst, p); return true;
            case Op::Sext8: case Op::Sext16: case Op::Sext32: case Op::Zext32: case Op::Trunc:
                convert(inst, p);
                return true;
            case Op::Compare: {
                const auto type = block_.type_of(inst.a);
                const auto x = use(inst.a, p);
                const auto y = use(inst.b, p);
                const auto d = def(inst, p);
                a_.xor_(d, d);
                a_.cmp(sized(x, type), sized(y, type));
                a_.set(condition(static_cast<Cond>(inst.aux)), d.r8());
                return true;
            }
            case Op::Select: {
                const auto cond = use(inst.a, p);
                const auto if_true = use(inst.b, p);
                const auto if_false = use(inst.c, p);
                const auto d = def(inst, p);
                a_.mov(d, sized(if_false, inst.type));
                a_.test(cond.r32(), cond.r32());
                a_.cmovnz(d, sized(if_true, inst.type));
                return true;
            }
            case Op::OverflowAdd: case Op::OverflowSub: case Op::CarryAdd: flag(inst, p); return true;
            case Op::LoadRam: load_ram(inst, p); return true;
            case Op::StoreRam: store_ram(inst, p); return true;
            case Op::Barrier: barrier(inst, p); return true;
            case Op::NoteRamWrite: note_ram_write(inst, p); return true;
            case Op::Call: return call(inst, p);
            case Op::ExitIf: {
                const auto cond = use(inst.a, p);
                const auto taken = a_.new_label();
                a_.test(cond.r32(), cond.r32());
                a_.jnz(taken);
                jump_site(taken, static_cast<ExitId>(inst.imm));
                return true;
            }
            case Op::Exit: return emit_exit(static_cast<ExitId>(inst.imm));
        }
        return false;
    }

    void binary(const Inst& inst, std::uint32_t p) {
        const auto x = sized(use(inst.a, p), inst.type);
        const auto y = sized(use(inst.b, p), inst.type);
        const auto d = def(inst, p);
        if (inst.op == Op::Shl || inst.op == Op::Shr || inst.op == Op::Sar) {
            // x86 masks a CL count by 31 or 63: the IR's width-1 mask.
            a_.mov(x86::ecx, y.r32());
            a_.mov(d, x);
            if (inst.op == Op::Shl) a_.shl(d, x86::cl);
            else if (inst.op == Op::Shr) a_.shr(d, x86::cl);
            else a_.sar(d, x86::cl);
            return;
        }
        a_.mov(d, x);
        switch (inst.op) {
            case Op::Add: a_.add(d, y); break;
            case Op::Sub: a_.sub(d, y); break;
            case Op::And: a_.and_(d, y); break;
            case Op::Or: a_.or_(d, y); break;
            case Op::Xor: a_.xor_(d, y); break;
            case Op::Nor: a_.or_(d, y); a_.not_(d); break;
            case Op::Mul: a_.imul(d, y); break;
            default: break;
        }
    }

    void mul_high(const Inst& inst, std::uint32_t p) {
        const bool is_signed = inst.op == Op::MulHiS;
        const auto x = use(inst.a, p);
        const auto y = use(inst.b, p);
        const auto d = def(inst, p);
        if (inst.type == Type::I64) {
            a_.mov(x86::rax, x);
            if (is_signed) a_.imul(y);  // rdx:rax <- rax * y
            else a_.mul(y);
            a_.mov(d, x86::rdx);
            return;
        }
        // 32 bits: the full product fits a 64-bit register.
        if (is_signed) {
            a_.movsxd(x86::rax, x.r32());
            a_.movsxd(x86::rcx, y.r32());
        } else {
            a_.mov(x86::eax, x.r32());
            a_.mov(x86::ecx, y.r32());
        }
        a_.imul(x86::rax, x86::rcx);
        if (is_signed) a_.sar(x86::rax, 32);
        else a_.shr(x86::rax, 32);
        a_.mov(d, x86::eax);
    }

    void convert(const Inst& inst, std::uint32_t p) {
        const auto s = use(inst.a, p);
        const auto d = def(inst, p);
        switch (inst.op) {
            case Op::Sext8: a_.movsx(d, s.r8()); break;
            case Op::Sext16: a_.movsx(d, s.r16()); break;
            case Op::Sext32: a_.movsxd(d.r64(), s.r32()); break;
            case Op::Zext32: case Op::Trunc: a_.mov(d.r32(), s.r32()); break;  // a 32-bit write clears the upper half
            default: break;
        }
    }

    void flag(const Inst& inst, std::uint32_t p) {
        const auto type = block_.type_of(inst.a);
        const auto x = use(inst.a, p);
        const auto y = use(inst.b, p);
        const auto d = def(inst, p);
        const auto scratch = sized(x86::rax, type);
        a_.xor_(d, d);
        a_.mov(scratch, sized(x, type));
        if (inst.op == Op::OverflowSub) a_.sub(scratch, sized(y, type));
        else a_.add(scratch, sized(y, type));
        if (inst.op == Op::CarryAdd) a_.setc(d.r8());
        else a_.seto(d.r8());
    }

    void load_ram(const Inst& inst, std::uint32_t p) {
        const auto at = use(inst.a, p);
        const auto d = def(inst, p);
        a_.mov(x86::ecx, at.r32());  // zero-extended physical address
        switch (inst.aux) {
            case 1: a_.movzx(d.r32(), x86::byte_ptr(kRam, x86::rcx)); break;
            case 2:
                a_.movzx(d.r32(), x86::word_ptr(kRam, x86::rcx));
                a_.ror(d.r16(), 8);
                break;
            case 4:
                a_.mov(d.r32(), x86::dword_ptr(kRam, x86::rcx));
                a_.bswap(d.r32());
                break;
            default:
                a_.mov(d.r64(), x86::qword_ptr(kRam, x86::rcx));
                a_.bswap(d.r64());
                break;
        }
    }

    void store_ram(const Inst& inst, std::uint32_t p) {
        const auto at = use(inst.a, p);
        const auto v = use(inst.b, p);
        a_.mov(x86::ecx, at.r32());
        switch (inst.aux) {
            case 1: a_.mov(x86::byte_ptr(kRam, x86::rcx), v.r8()); break;
            case 2:
                a_.mov(x86::eax, v.r32());
                a_.ror(x86::ax, 8);
                a_.mov(x86::word_ptr(kRam, x86::rcx), x86::ax);
                break;
            case 4:
                a_.mov(x86::eax, v.r32());
                a_.bswap(x86::eax);
                a_.mov(x86::dword_ptr(kRam, x86::rcx), x86::eax);
                break;
            default:
                a_.mov(x86::rax, v.r64());
                a_.bswap(x86::rax);
                a_.mov(x86::qword_ptr(kRam, x86::rcx), x86::rax);
                break;
        }
    }

    // The renderer's memory barrier, with no call: the block leaves through the barrier's
    // exit when the page has a newer packet than completed_seen, or the renderer failed.
    // rax = view, rcx = page, rdx = sequences, r8 = table, r9 = slot / sequence.
    void barrier(const Inst& inst, std::uint32_t p) {
        const bool write = (inst.imm & 1) != 0;
        const auto at = use(inst.a, p);
        const auto slow = a_.new_label();
        const auto done = a_.new_label();
        a_.mov(x86::rax, x86::qword_ptr(kCtx, field(offsetof(Context, barrier))));
        a_.test(x86::rax, x86::rax);
        a_.jz(done);
        a_.mov(x86::ecx, at.r32());
        a_.shr(x86::ecx, 12);
        a_.mov(x86::rdx, x86::qword_ptr(x86::rax, field(offsetof(PageBarrierView, slot_sequence))));
        a_.mov(x86::r8, x86::qword_ptr(x86::rax, field(offsetof(PageBarrierView, writers))));
        a_.movzx(x86::r9d, x86::word_ptr(x86::r8, x86::rcx, 1));
        a_.mov(x86::r9, x86::qword_ptr(x86::rdx, x86::r9, 3));
        if (write) {
            a_.mov(x86::r8, x86::qword_ptr(x86::rax, field(offsetof(PageBarrierView, readers))));
            a_.movzx(x86::r8d, x86::word_ptr(x86::r8, x86::rcx, 1));
            a_.mov(x86::r8, x86::qword_ptr(x86::rdx, x86::r8, 3));
            a_.cmp(x86::r8, x86::r9);
            a_.cmova(x86::r9, x86::r8);
        }
        a_.mov(x86::r8, x86::qword_ptr(x86::rax, field(offsetof(PageBarrierView, completed_seen))));
        a_.cmp(x86::r9, x86::qword_ptr(x86::r8));
        a_.ja(slow);
        a_.mov(x86::r8, x86::qword_ptr(x86::rax, field(offsetof(PageBarrierView, failed))));
        a_.cmp(x86::byte_ptr(x86::r8), 0);  // an acquire load is a plain load on x86-64
        a_.jne(slow);
        a_.bind(done);
        jump_site(slow, static_cast<ExitId>(inst.imm >> Inst::kBarrierExitShift));
    }

    // The call is conditional, so the volatile registers are made safe on both paths.
    void note_ram_write(const Inst& inst, std::uint32_t p) {
        const auto at = use(inst.a, p);
        a_.mov(x86::edx, at.r32());
        alloc_.before_call(p, moves_);
        flush();
        const auto done = a_.new_label();
        a_.mov(x86::ecx, x86::edx);
        a_.shr(x86::ecx, 12);
        a_.mov(x86::rax, x86::qword_ptr(kCtx, field(offsetof(Context, code_pages))));
        a_.movzx(x86::eax, x86::byte_ptr(x86::rax, x86::rcx));
        a_.test(x86::eax, x86::eax);
        a_.jz(done);  // eax = 0: compiled code is unchanged
        a_.mov(x86::rcx, kCtx);
        a_.mov(x86::r8d, inst.aux);
        a_.call(x86::qword_ptr(kCtx, field(offsetof(Context, code_write))));
        a_.bind(done);
        const auto d = def(inst, p);
        a_.mov(d.r32(), x86::eax);
    }

    bool call(const Inst& inst, std::uint32_t p) {
        if (!load(x86::rdx, inst.a) || !load(x86::r8, inst.b) || !load(x86::r9, inst.c)) return false;
        alloc_.before_call(p, moves_);
        flush();
        a_.mov(x86::rcx, kCtx);
        a_.mov(x86::rax, Imm(inst.imm));
        a_.call(x86::rax);
        const auto d = def(inst, p);
        a_.mov(d.r64(), x86::rax);
        return true;
    }

    CodeHolder& code_;
    x86::Assembler& a_;
    const IrBlock& block_;
    UsePositions uses_;
    RegAlloc alloc_;
    std::vector<Move> moves_;
    std::vector<Site> sites_;
    Label epilogue_;
    bool ctx_ = false, guest_ = false, ram_ = false, calls_ = false;
};

}  // namespace
#endif

Compiled compile(const IrBlock& block, CodeBuffer& buffer) {
#if EZY_JIT_AVAILABLE
    if (!buffer.available()) return {nullptr, CompileError::Unsupported};
    auto& impl = *buffer.impl_;
    asmjit::CodeHolder code;
    Errors errors;
    if (code.init(impl.runtime.environment(), impl.runtime.cpu_features()) != asmjit::Error::kOk) {
        return {nullptr, CompileError::Unsupported};
    }
    code.set_error_handler(&errors);
    asmjit::x86::Assembler assembler(&code);
    if (!Emitter(code, assembler, block).emit() || errors.failed) return {nullptr, CompileError::Unsupported};
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
    return {nullptr, CompileError::Unsupported};
#endif
}

}  // namespace ezy::jit
