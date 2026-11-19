#include "jit/ir.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ezy::jit {

namespace {

[[noreturn]] void fail(const char* message) { throw std::logic_error(message); }

Type integer(Type type) {
    if (type != Type::I32 && type != Type::I64) fail("jit IR: float types are not supported yet");
    return type;
}

void check_bytes(Type type, unsigned bytes) {
    const unsigned width = type == Type::I64 ? 8U : 4U;
    if ((bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) || bytes > width) {
        fail("jit IR: RAM access size must be 1, 2, 4 or 8 bytes and fit the value type");
    }
}

}  // namespace

std::vector<Inst>& IrBuilder::ops() { return region_ == kBody ? body_ : exit_ops_[region_]; }

void IrBuilder::push(Inst inst) {
    if (region_ == kBody && terminated_) fail("jit IR: no op may follow the body's exit");
    ops().push_back(inst);
}

Value IrBuilder::define(Inst inst) {
    inst.result = static_cast<std::uint32_t>(values_.size());
    push(inst);
    values_.push_back({inst.type, region_, static_cast<std::uint32_t>(ops().size() - 1)});
    return Value{inst.result};
}

Type IrBuilder::use(Value value) {
    if (value.id >= values_.size()) fail("jit IR: unknown value");
    const auto& info = values_[value.id];
    if (info.region != kBody && info.region != region_) fail("jit IR: an exit's values are local to it");
    if (info.region == kBody && region_ != kBody) {
        auto& exit = exits_[region_];
        exit.needs = std::max(exit.needs, info.position + 1);
    }
    return info.type;
}

void IrBuilder::check_exit(ExitId exit) const {
    if (exit >= exits_.size()) fail("jit IR: unknown exit");
}

Value IrBuilder::constant(Type type, std::uint64_t value) {
    if (integer(type) == Type::I32) value &= 0xFFFF'FFFFULL;
    return define({.op = Op::Const, .type = type, .imm = value});
}

Value IrBuilder::load_guest(Type type, std::uint32_t offset) {
    if (offset > 0x7FFF'FFFFU) fail("jit IR: guest offset out of range");
    return define({.op = Op::LoadGuest, .type = integer(type), .imm = offset});
}

void IrBuilder::store_guest(std::uint32_t offset, Value value, unsigned bytes) {
    if (offset > 0x7FFF'FFFFU) fail("jit IR: guest offset out of range");
    const auto type = use(value);
    if (bytes != 0) check_bytes(type, bytes);
    push({.op = Op::StoreGuest, .type = type, .aux = static_cast<std::uint8_t>(bytes), .a = value.id, .imm = offset});
}

Value IrBuilder::load_context(ContextField field) {
    return define({.op = Op::LoadContext, .type = Type::I64, .aux = static_cast<std::uint8_t>(field)});
}

void IrBuilder::store_context(ContextField field, Value value) {
    if (use(value) != Type::I64) fail("jit IR: context fields are I64");
    push({.op = Op::StoreContext, .type = Type::I64, .aux = static_cast<std::uint8_t>(field), .a = value.id});
}

Value IrBuilder::binary(Op op, Value a, Value b) {
    if (op < Op::Add || op > Op::MulHiU) fail("jit IR: not a binary op");
    const auto type = use(a);
    if (use(b) != type) fail("jit IR: binary operands must have the same type");
    return define({.op = op, .type = type, .a = a.id, .b = b.id});
}

Value IrBuilder::convert(Op op, Type to, Value v) {
    integer(to);
    use(v);
    switch (op) {
        case Op::Sext8: case Op::Sext16: break;
        case Op::Sext32: case Op::Zext32:
            if (to != Type::I64) fail("jit IR: Sext32/Zext32 produce I64");
            break;
        case Op::Trunc:
            if (to != Type::I32) fail("jit IR: Trunc produces I32");
            break;
        default: fail("jit IR: not a conversion");
    }
    return define({.op = op, .type = to, .a = v.id});
}

Value IrBuilder::compare(Cond cond, Value a, Value b) {
    if (cond > Cond::GeU) fail("jit IR: unknown condition");
    if (use(a) != use(b)) fail("jit IR: compared values must have the same type");
    return define({.op = Op::Compare, .type = Type::I32, .aux = static_cast<std::uint8_t>(cond), .a = a.id, .b = b.id});
}

Value IrBuilder::select(Value cond, Value if_true, Value if_false) {
    if (use(cond) != Type::I32) fail("jit IR: a condition is I32");
    const auto type = use(if_true);
    if (use(if_false) != type) fail("jit IR: selected values must have the same type");
    return define({.op = Op::Select, .type = type, .a = cond.id, .b = if_true.id, .c = if_false.id});
}

Value IrBuilder::overflow(Op add_or_sub, Value a, Value b) {
    if (add_or_sub != Op::Add && add_or_sub != Op::Sub) fail("jit IR: overflow is for Add or Sub");
    if (use(a) != use(b)) fail("jit IR: overflow operands must have the same type");
    const auto op = add_or_sub == Op::Add ? Op::OverflowAdd : Op::OverflowSub;
    return define({.op = op, .type = Type::I32, .a = a.id, .b = b.id});
}

Value IrBuilder::carry_add(Value a, Value b) {
    if (use(a) != use(b)) fail("jit IR: carry operands must have the same type");
    return define({.op = Op::CarryAdd, .type = Type::I32, .a = a.id, .b = b.id});
}

Value IrBuilder::load_ram(Type type, Value paddr, unsigned bytes) {
    check_bytes(integer(type), bytes);
    if (use(paddr) != Type::I32) fail("jit IR: a physical address is I32");
    return define({.op = Op::LoadRam, .type = type, .aux = static_cast<std::uint8_t>(bytes), .a = paddr.id});
}

void IrBuilder::store_ram(Value paddr, Value value, unsigned bytes) {
    if (use(paddr) != Type::I32) fail("jit IR: a physical address is I32");
    const auto type = use(value);
    check_bytes(type, bytes);
    push({.op = Op::StoreRam, .type = type, .aux = static_cast<std::uint8_t>(bytes), .a = paddr.id, .b = value.id});
}

void IrBuilder::barrier(Value paddr, unsigned bytes, bool write) {
    if (use(paddr) != Type::I32) fail("jit IR: a physical address is I32");
    check_bytes(Type::I64, bytes);
    push({.op = Op::Barrier, .aux = static_cast<std::uint8_t>(bytes), .a = paddr.id, .imm = write ? 1U : 0U});
}

void IrBuilder::barrier(Value paddr, unsigned bytes, bool write, ExitId on_failure) {
    check_exit(on_failure);
    if (region_ != kBody) fail("jit IR: an exit region cannot jump");
    if (use(paddr) != Type::I32) fail("jit IR: a physical address is I32");
    check_bytes(Type::I64, bytes);
    auto& info = exits_[on_failure];
    info.first_jump = std::min(info.first_jump, static_cast<std::uint32_t>(body_.size()));
    const std::uint64_t flags = (write ? 1U : 0U) | ((std::uint64_t{on_failure} + 1) << Inst::kBarrierExitShift);
    push({.op = Op::Barrier, .aux = static_cast<std::uint8_t>(bytes), .a = paddr.id, .imm = flags});
}

Value IrBuilder::note_ram_write(Value paddr, unsigned bytes) {
    if (use(paddr) != Type::I32) fail("jit IR: a physical address is I32");
    check_bytes(Type::I64, bytes);
    return define({.op = Op::NoteRamWrite, .type = Type::I32, .aux = static_cast<std::uint8_t>(bytes), .a = paddr.id});
}

Value IrBuilder::call(Helper helper, Value a, Value b, Value c) {
    if (helper == nullptr) fail("jit IR: null helper");
    if (use(a) != Type::I64 || use(b) != Type::I64 || use(c) != Type::I64) fail("jit IR: helper arguments are I64");
    return define({.op = Op::Call, .type = Type::I64, .a = a.id, .b = b.id, .c = c.id,
                   .imm = reinterpret_cast<std::uint64_t>(helper)});
}

ExitId IrBuilder::add_exit() {
    exits_.emplace_back();
    exit_ops_.emplace_back();
    return static_cast<ExitId>(exits_.size() - 1);
}

void IrBuilder::begin_exit(ExitId exit) {
    check_exit(exit);
    if (region_ != kBody) fail("jit IR: exit regions do not nest");
    if (exits_[exit].opened) fail("jit IR: an exit region is defined once");
    exits_[exit].opened = true;
    region_ = exit;
}

void IrBuilder::end_exit() {
    if (region_ == kBody) fail("jit IR: end_exit without begin_exit");
    region_ = kBody;
}

void IrBuilder::exit_if(Value cond, ExitId exit) {
    check_exit(exit);
    if (region_ != kBody) fail("jit IR: an exit region cannot jump");
    if (use(cond) != Type::I32) fail("jit IR: a condition is I32");
    auto& info = exits_[exit];
    info.first_jump = std::min(info.first_jump, static_cast<std::uint32_t>(body_.size()));
    push({.op = Op::ExitIf, .a = cond.id, .imm = exit});
}

void IrBuilder::exit(ExitId exit) {
    check_exit(exit);
    if (region_ != kBody) fail("jit IR: an exit region cannot jump");
    auto& info = exits_[exit];
    info.first_jump = std::min(info.first_jump, static_cast<std::uint32_t>(body_.size()));
    push({.op = Op::Exit, .imm = exit});
    terminated_ = true;
}

IrBlock IrBuilder::finish() {
    if (region_ != kBody) fail("jit IR: an exit region was not closed");
    if (!terminated_) fail("jit IR: the body must end in exit()");
    for (const auto& exit : exits_) {
        // A region may read body values only if every jump to it comes after their definition.
        if (exit.first_jump != Inst::kNone && exit.first_jump < exit.needs) {
            fail("jit IR: an exit region uses a value defined after a jump to it");
        }
    }
    IrBlock block;
    block.body_ = std::move(body_);
    block.exits_ = std::move(exit_ops_);
    block.types_.reserve(values_.size());
    for (const auto& value : values_) block.types_.push_back(value.type);
    *this = IrBuilder{};
    return block;
}

}  // namespace ezy::jit
