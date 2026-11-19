#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "jit/context.hpp"

// Public header of ezy_jit: a linear IR that knows no guest CPU and never includes AsmJit.
namespace ezy::jit {

// Floats exist for the PowerPC front-end; the builder rejects them until they are supported.
enum class Type : std::uint8_t { I32, I64, F32, F64, F32x2 };
enum class Cond : std::uint8_t { Eq, Ne, LtS, LtU, LeS, GtS, GeS, GeU };
enum class Op : std::uint8_t {
    Const, LoadGuest, StoreGuest, LoadContext, StoreContext,
    Add, Sub, And, Or, Xor, Nor, Shl, Shr, Sar, Mul, MulHiS, MulHiU,
    Sext8, Sext16, Sext32, Zext32, Trunc,           // to the result type
    Compare, Select, OverflowAdd, OverflowSub, CarryAdd,
    LoadRam, StoreRam, Barrier, NoteRamWrite, Call,
    ExitIf, Exit,
};

struct Value { std::uint32_t id = ~0U; };
using ExitId = std::uint32_t;
using Helper = std::uint64_t (*)(Context*, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;

// One IR instruction. Operands and the result are value ids.
struct Inst {
    static constexpr std::uint32_t kNone = ~0U;
    Op op = Op::Const;
    Type type = Type::I32;       // result type; for stores, the stored value's type
    std::uint8_t aux = 0;        // Cond, ContextField, or byte count of a RAM access or guest store (0: whole value)
    std::uint32_t result = kNone;
    std::uint32_t a = kNone, b = kNone, c = kNone;
    std::uint64_t imm = 0;       // constant, guest offset, Helper address, ExitId, Barrier flags
    // Barrier::imm: bit 0 = write; bits 32-63 = 1 + the exit taken when barrier_wait fails (0: none).
    static constexpr unsigned kBarrierExitShift = 32;
};

// A verified block: the main body (ending in Exit) and one op list per exit. Exit `n`
// runs its ops and makes the block return `n`.
class IrBlock {
public:
    [[nodiscard]] const std::vector<Inst>& body() const { return body_; }
    [[nodiscard]] std::size_t exit_count() const { return exits_.size(); }
    [[nodiscard]] const std::vector<Inst>& exit_ops(ExitId exit) const { return exits_[exit]; }
    [[nodiscard]] std::size_t value_count() const { return types_.size(); }
    [[nodiscard]] Type type_of(std::uint32_t value) const { return types_[value]; }

private:
    friend class IrBuilder;
    IrBlock() = default;
    std::vector<Inst> body_;
    std::vector<std::vector<Inst>> exits_;
    std::vector<Type> types_;
};

// Builds an IrBlock. Every misuse throws std::logic_error: type errors when the op is
// added, structural errors at finish().
class IrBuilder {
public:
    Value constant(Type type, std::uint64_t value);
    Value load_guest(Type type, std::uint32_t offset);
    // bytes: 0 stores the whole value; 1, 2 or 4 store its low bytes (within its width).
    void store_guest(std::uint32_t offset, Value value, unsigned bytes = 0);
    Value load_context(ContextField field);                 // I64
    void store_context(ContextField field, Value value);
    Value binary(Op op, Value a, Value b);                  // same type; shifts mask the amount by width-1
    Value convert(Op op, Type to, Value v);                 // Sext8/16 (to any), Sext32/Zext32 (to I64), Trunc (to I32)
    Value compare(Cond cond, Value a, Value b);             // I32 0/1
    Value select(Value cond, Value if_true, Value if_false);  // cond is I32, nonzero = true
    Value overflow(Op add_or_sub, Value a, Value b);        // I32 0/1, signed, at the operands' width
    Value carry_add(Value a, Value b);                      // I32 0/1, unsigned carry out
    Value load_ram(Type type, Value paddr, unsigned bytes); // big-endian, zero-extended; paddr is I32, checked by the front-end
    void store_ram(Value paddr, Value value, unsigned bytes);  // the low `bytes` of value, big-endian
    // paddr must be below Context::ram_size and the access must not cross a 4 KiB page;
    // the front-end checks this (like load_ram).
    void barrier(Value paddr, unsigned bytes, bool write);  // inline fast check, ctx->barrier_wait on the slow path
    // The same, leaving through `on_failure` when barrier_wait returns nonzero. The fast path
    // is unchanged: only the slow path tests the answer. A jump, like exit_if().
    void barrier(Value paddr, unsigned bytes, bool write, ExitId on_failure);
    // Same precondition as barrier(): paddr below Context::ram_size, no 4 KiB page crossing.
    Value note_ram_write(Value paddr, unsigned bytes);      // I32: 1 if compiled code changed
    Value call(Helper helper, Value a, Value b, Value c);   // I64; use constant(I64, 0) for unused arguments
    ExitId add_exit();                                      // ids 0..n-1 in order; the block returns the id
    void begin_exit(ExitId exit);                           // ops until end_exit() run only on that exit
    void end_exit();
    void exit_if(Value cond, ExitId exit);                  // cond is I32, nonzero = take the exit
    void exit(ExitId exit);                                 // last op of the main body
    [[nodiscard]] IrBlock finish();                         // verifies and resets the builder

private:
    static constexpr std::uint32_t kBody = ~0U;
    struct ValueInfo {
        Type type;
        std::uint32_t region;    // kBody or the exit that defined it
        std::uint32_t position;  // index in its region's op list
    };
    struct ExitInfo {
        bool opened = false;
        std::uint32_t needs = 0;               // jumps must come at or after this body index
        std::uint32_t first_jump = Inst::kNone;
    };

    Type use(Value value);
    Value define(Inst inst);
    void push(Inst inst);
    std::vector<Inst>& ops();
    void check_exit(ExitId exit) const;

    std::vector<Inst> body_;
    std::vector<std::vector<Inst>> exit_ops_;
    std::vector<ExitInfo> exits_;
    std::vector<ValueInfo> values_;
    std::uint32_t region_ = kBody;
    bool terminated_ = false;
};

}  // namespace ezy::jit
