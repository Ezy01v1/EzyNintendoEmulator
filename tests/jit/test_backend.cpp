#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "jit/code_buffer.hpp"
#include "jit/context.hpp"
#include "jit/ir.hpp"
#include "jit/x64_backend.hpp"

using namespace ezy::jit;

namespace {

constexpr bool kNative = EZY_JIT_AVAILABLE == 1;

constexpr std::uint32_t slot(unsigned index) { return index * 8; }

// A Context over local arrays; one CodeBuffer per machine.
struct Machine {
    std::array<std::uint64_t, 8> guest{};
    std::array<std::uint8_t, 8192> ram{};
    std::array<std::uint8_t, 2> code_pages{};
    Context ctx{};
    CodeBuffer buffer{1U << 20};

    Machine() {
        ctx.guest = reinterpret_cast<std::uint8_t*>(guest.data());
        ctx.ram = ram.data();
        ctx.ram_size = static_cast<std::uint32_t>(ram.size());
        ctx.code_pages = code_pages.data();
        ctx.user = this;
    }

    BlockFn build(IrBuilder& builder) {
        const auto compiled = compile(builder.finish(), buffer);
        REQUIRE(compiled.error == CompileError::None);
        REQUIRE(compiled.fn != nullptr);
        return compiled.fn;
    }

    std::uint32_t run(IrBuilder& builder) { return build(builder)(&ctx); }
};

// guest[2] = op(guest[0], guest[1]) at the given width.
BlockFn binary_block(Machine& m, Op op, Type type) {
    IrBuilder b;
    const auto a = b.load_guest(type, slot(0));
    const auto c = b.load_guest(type, slot(1));
    b.store_guest(slot(2), b.binary(op, a, c));
    b.exit(b.add_exit());
    return m.build(b);
}

enum class Flag { OverflowAdd, OverflowSub, Carry };

std::uint64_t flag(Flag kind, Type type, std::uint64_t x, std::uint64_t y) {
    Machine m;
    IrBuilder b;
    const auto a = b.load_guest(type, slot(0));
    const auto c = b.load_guest(type, slot(1));
    Value r;
    switch (kind) {
        case Flag::OverflowAdd: r = b.overflow(Op::Add, a, c); break;
        case Flag::OverflowSub: r = b.overflow(Op::Sub, a, c); break;
        case Flag::Carry: r = b.carry_add(a, c); break;
    }
    b.store_guest(slot(2), b.convert(Op::Zext32, Type::I64, r));
    b.exit(b.add_exit());
    m.guest[0] = x;
    m.guest[1] = y;
    m.guest[2] = 0xDEAD;
    m.run(b);
    return m.guest[2];
}

std::uint64_t compare(Cond cond, Type type, std::uint64_t x, std::uint64_t y) {
    Machine m;
    IrBuilder b;
    const auto a = b.load_guest(type, slot(0));
    const auto c = b.load_guest(type, slot(1));
    b.store_guest(slot(2), b.convert(Op::Zext32, Type::I64, b.compare(cond, a, c)));
    b.exit(b.add_exit());
    m.guest[0] = x;
    m.guest[1] = y;
    m.guest[2] = 0xDEAD;
    m.run(b);
    return m.guest[2];
}

std::uint64_t mul_hi_s(std::uint64_t a, std::uint64_t b) {
#if defined(_MSC_VER)
    std::int64_t hi = 0;
    (void)_mul128(static_cast<std::int64_t>(a), static_cast<std::int64_t>(b), &hi);
    return static_cast<std::uint64_t>(hi);
#else
    return static_cast<std::uint64_t>((static_cast<__int128>(static_cast<std::int64_t>(a)) *
                                       static_cast<std::int64_t>(b)) >> 64);
#endif
}

std::uint64_t mul_hi_u(std::uint64_t a, std::uint64_t b) {
#if defined(_MSC_VER)
    std::uint64_t hi = 0;
    (void)_umul128(a, b, &hi);
    return hi;
#else
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(a) * b) >> 64);
#endif
}

}  // namespace

TEST_CASE("IR adds two guest registers and stores the sign-extended 32-bit sum" * doctest::skip(!kNative)) {
    Machine m;
    IrBuilder b;
    std::array<ExitId, 8> exits{};
    for (auto& e : exits) e = b.add_exit();
    for (unsigned i = 0; i < exits.size(); ++i) CHECK(exits[i] == i);
    const auto g1 = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(1)));
    const auto g2 = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(2)));
    b.store_guest(slot(0), b.convert(Op::Sext32, Type::I64, b.binary(Op::Add, g1, g2)));
    b.exit(exits[7]);
    m.guest[1] = 0x7FFFFFFF;
    m.guest[2] = 1;
    const auto tag = m.run(b);
    CHECK(m.guest[0] == 0xFFFFFFFF80000000ULL);
    CHECK(tag == 7);
}

TEST_CASE("IR exit_if takes the side exit and runs only its own stores" * doctest::skip(!kNative)) {
    for (const bool taken : {true, false}) {
        Machine m;
        IrBuilder b;
        const auto e0 = b.add_exit();
        const auto e1 = b.add_exit();
        b.begin_exit(e1);
        b.store_guest(slot(3), b.constant(Type::I64, 11));
        b.end_exit();
        b.exit_if(b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(0))), e1);
        b.store_guest(slot(4), b.constant(Type::I64, 22));
        b.exit(e0);
        m.guest[0] = taken ? 0x100000001ULL : 0x100000000ULL;
        const auto tag = m.run(b);
        if (taken) {
            CHECK(tag == 1);
            CHECK(m.guest[3] == 11);
            CHECK(m.guest[4] == 0);
        } else {
            CHECK(tag == 0);
            CHECK(m.guest[3] == 0);
            CHECK(m.guest[4] == 22);
        }
    }
}

TEST_CASE("IR 64-bit ops match C++ for edge values" * doctest::skip(!kNative)) {
    constexpr std::array<std::uint64_t, 6> values{
        0, 1, ~0ULL, 1ULL << 63, 0x8000'0000ULL, 0x7FFF'FFFF'FFFF'FFFFULL};
    const auto expect = [](Op op, std::uint64_t a, std::uint64_t b) -> std::uint64_t {
        switch (op) {
            case Op::Add: return a + b;
            case Op::Sub: return a - b;
            case Op::And: return a & b;
            case Op::Or: return a | b;
            case Op::Xor: return a ^ b;
            case Op::Nor: return ~(a | b);
            case Op::Shl: return a << (b & 63);
            case Op::Shr: return a >> (b & 63);
            case Op::Sar: return static_cast<std::uint64_t>(static_cast<std::int64_t>(a) >> (b & 63));
            case Op::Mul: return a * b;
            case Op::MulHiS: return mul_hi_s(a, b);
            case Op::MulHiU: return mul_hi_u(a, b);
            default: return 0;
        }
    };
    Machine m;
    for (const auto op : {Op::Add, Op::Sub, Op::And, Op::Or, Op::Xor, Op::Nor, Op::Shl, Op::Shr,
                          Op::Sar, Op::Mul, Op::MulHiS, Op::MulHiU}) {
        const auto fn = binary_block(m, op, Type::I64);
        for (const auto a : values) {
            for (const auto b : values) {
                m.guest[0] = a;
                m.guest[1] = b;
                fn(&m.ctx);
                const auto out = m.guest[2];
                const auto expected = expect(op, a, b);
                CAPTURE(static_cast<int>(op));
                CAPTURE(a);
                CAPTURE(b);
                CHECK(out == expected);
            }
        }
    }
}

TEST_CASE("IR 32-bit ops work on 32 bits and mask shifts by 31" * doctest::skip(!kNative)) {
    constexpr std::array<std::uint32_t, 6> values{0, 1, ~0U, 1U << 31, 0x7FFF'FFFFU, 33};
    const auto expect = [](Op op, std::uint32_t a, std::uint32_t b) -> std::uint32_t {
        const auto sa = static_cast<std::int64_t>(static_cast<std::int32_t>(a));
        const auto sb = static_cast<std::int64_t>(static_cast<std::int32_t>(b));
        switch (op) {
            case Op::Add: return a + b;
            case Op::Nor: return ~(a | b);
            case Op::Shl: return a << (b & 31);
            case Op::Shr: return a >> (b & 31);
            case Op::Sar: return static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> (b & 31));
            case Op::Mul: return a * b;
            case Op::MulHiS: return static_cast<std::uint32_t>(static_cast<std::uint64_t>(sa * sb) >> 32);
            case Op::MulHiU: return static_cast<std::uint32_t>((std::uint64_t{a} * b) >> 32);
            default: return 0;
        }
    };
    Machine m;
    for (const auto op : {Op::Add, Op::Nor, Op::Shl, Op::Shr, Op::Sar, Op::Mul, Op::MulHiS, Op::MulHiU}) {
        const auto fn = binary_block(m, op, Type::I32);
        for (const auto a : values) {
            for (const auto b : values) {
                // The upper halves must not leak into a 32-bit op.
                m.guest[0] = 0xA5A5'A5A5'0000'0000ULL | a;
                m.guest[1] = 0x5A5A'5A5A'0000'0000ULL | b;
                m.guest[2] = 0xFFFF'FFFF'FFFF'FFFFULL;
                fn(&m.ctx);
                CAPTURE(static_cast<int>(op));
                CAPTURE(a);
                CAPTURE(b);
                CHECK(static_cast<std::uint32_t>(m.guest[2]) == expect(op, a, b));
                CHECK((m.guest[2] >> 32) == 0xFFFF'FFFFULL);  // a 32-bit store writes 4 bytes
            }
        }
    }
}

TEST_CASE("IR conversions extend and truncate" * doctest::skip(!kNative)) {
    Machine m;
    IrBuilder b;
    const auto v = b.load_guest(Type::I64, slot(0));
    b.store_guest(slot(1), b.convert(Op::Sext8, Type::I64, v));
    b.store_guest(slot(2), b.convert(Op::Sext16, Type::I64, v));
    b.store_guest(slot(3), b.convert(Op::Sext32, Type::I64, v));
    b.store_guest(slot(4), b.convert(Op::Zext32, Type::I64, v));
    b.store_guest(slot(5), b.convert(Op::Zext32, Type::I64, b.convert(Op::Sext8, Type::I32, v)));
    b.exit(b.add_exit());
    m.guest[0] = 0x1234'5678'8765'C3F0ULL;
    m.run(b);
    CHECK(m.guest[1] == 0xFFFF'FFFF'FFFF'FFF0ULL);
    CHECK(m.guest[2] == 0xFFFF'FFFF'FFFF'C3F0ULL);
    CHECK(m.guest[3] == 0xFFFF'FFFF'8765'C3F0ULL);
    CHECK(m.guest[4] == 0x0000'0000'8765'C3F0ULL);
    CHECK(m.guest[5] == 0x0000'0000'FFFF'FFF0ULL);
}

TEST_CASE("IR stores the low 1, 2 or 4 bytes of a value to the guest" * doctest::skip(!kNative)) {
    Machine m;
    IrBuilder b;
    const auto v = b.load_guest(Type::I64, slot(0));
    b.store_guest(slot(1) + 1, v, 1);
    b.store_guest(slot(2) + 2, v, 2);
    b.store_guest(slot(3) + 4, b.convert(Op::Trunc, Type::I32, v), 4);
    b.store_guest(slot(4), b.convert(Op::Trunc, Type::I32, v), 1);
    b.exit(b.add_exit());
    m.guest[0] = 0x1122'3344'5566'7788ULL;
    for (unsigned k = 1; k <= 4; ++k) m.guest[k] = ~0ULL;
    m.run(b);
    CHECK(m.guest[1] == 0xFFFF'FFFF'FFFF'88FFULL);  // little-endian host: byte 1
    CHECK(m.guest[2] == 0xFFFF'FFFF'7788'FFFFULL);
    CHECK(m.guest[3] == 0x5566'7788'FFFF'FFFFULL);
    CHECK(m.guest[4] == 0xFFFF'FFFF'FFFF'FF88ULL);
}

TEST_CASE("IR rejects a guest store wider than its value") {
    IrBuilder b;
    const auto v = b.load_guest(Type::I32, slot(0));
    CHECK_THROWS_AS(b.store_guest(slot(1), v, 8), std::logic_error);
    CHECK_THROWS_AS(b.store_guest(slot(1), v, 3), std::logic_error);
}

TEST_CASE("IR compare, select, overflow and carry give 0 or 1" * doctest::skip(!kNative)) {
    CHECK(flag(Flag::OverflowAdd, Type::I32, 0x7FFFFFFF, 1) == 1);
    CHECK(flag(Flag::OverflowAdd, Type::I32, 0x7FFFFFFE, 1) == 0);
    CHECK(flag(Flag::OverflowAdd, Type::I64, 0x7FFFFFFF, 1) == 0);
    CHECK(flag(Flag::OverflowSub, Type::I64, 1ULL << 63, 1) == 1);
    CHECK(flag(Flag::OverflowSub, Type::I32, 1ULL << 63, 1) == 0);
    CHECK(flag(Flag::Carry, Type::I32, 0xFFFFFFFF, 1) == 1);
    CHECK(flag(Flag::Carry, Type::I64, 0xFFFFFFFF, 1) == 0);
    CHECK(flag(Flag::Carry, Type::I64, ~0ULL, 1) == 1);
    CHECK(compare(Cond::LtU, Type::I64, 1, ~0ULL) == 1);
    CHECK(compare(Cond::LtS, Type::I64, 1, ~0ULL) == 0);
    CHECK(compare(Cond::Eq, Type::I32, 0x1'0000'0005ULL, 5) == 1);
    CHECK(compare(Cond::Ne, Type::I64, 0x1'0000'0005ULL, 5) == 1);
    CHECK(compare(Cond::LeS, Type::I32, 0xFFFFFFFF, 0) == 1);
    CHECK(compare(Cond::GtS, Type::I32, 0xFFFFFFFF, 0) == 0);
    CHECK(compare(Cond::GeS, Type::I64, 5, 5) == 1);
    CHECK(compare(Cond::GeU, Type::I32, 0xFFFFFFFF, 0) == 1);

    Machine m;
    IrBuilder b;
    const auto cond = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(0)));
    b.store_guest(slot(1), b.select(cond, b.constant(Type::I64, 0x1111'2222'3333'4444ULL),
                                    b.constant(Type::I64, 0x5555'6666'7777'8888ULL)));
    b.exit(b.add_exit());
    const auto fn = m.build(b);
    m.guest[0] = 1;
    fn(&m.ctx);
    CHECK(m.guest[1] == 0x1111'2222'3333'4444ULL);
    m.guest[0] = 0x1'0000'0000ULL;  // only the I32 condition counts
    fn(&m.ctx);
    CHECK(m.guest[1] == 0x5555'6666'7777'8888ULL);
}

TEST_CASE("IR RAM loads and stores are big-endian" * doctest::skip(!kNative)) {
    Machine m;
    const std::array<std::uint8_t, 8> bytes{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::copy(bytes.begin(), bytes.end(), m.ram.begin());
    const auto load_ram = [&](Type type, std::uint32_t paddr, unsigned size) {
        IrBuilder b;
        auto v = b.load_ram(type, b.constant(Type::I32, paddr), size);
        if (type == Type::I32) v = b.convert(Op::Zext32, Type::I64, v);
        b.store_guest(slot(0), v);
        b.exit(b.add_exit());
        m.guest[0] = 0xDEAD'BEEF'DEAD'BEEFULL;
        m.run(b);
        return m.guest[0];
    };
    CHECK(load_ram(Type::I32, 0, 4) == 0x11223344);
    CHECK(load_ram(Type::I64, 0, 8) == 0x1122334455667788ULL);
    CHECK(load_ram(Type::I64, 4, 4) == 0x55667788);
    CHECK(load_ram(Type::I64, 1, 1) == 0x22);
    CHECK(load_ram(Type::I32, 6, 2) == 0x7788);

    IrBuilder b;
    b.store_ram(b.constant(Type::I32, 0), b.constant(Type::I32, 0xAABB), 2);
    b.store_ram(b.constant(Type::I32, 2), b.constant(Type::I64, 0xCC), 1);
    b.store_ram(b.constant(Type::I32, 16), b.constant(Type::I32, 0x01020304), 4);
    b.store_ram(b.constant(Type::I32, 24), b.constant(Type::I64, 0x0102030405060708ULL), 8);
    b.exit(b.add_exit());
    m.run(b);
    CHECK(m.ram[0] == 0xAA);
    CHECK(m.ram[1] == 0xBB);
    CHECK(m.ram[2] == 0xCC);
    CHECK(m.ram[3] == 0x44);
    for (unsigned i = 0; i < 4; ++i) CHECK(m.ram[16 + i] == i + 1);
    for (unsigned i = 0; i < 8; ++i) CHECK(m.ram[24 + i] == i + 1);
}

namespace {

std::uint64_t pack_helper(Context* ctx, std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    auto& m = *static_cast<Machine*>(ctx->user);
    if (&m.ctx != ctx) return 0;
    return (a << 16) | (b << 8) | c;
}

}  // namespace

TEST_CASE("IR calls a helper with the context and three arguments" * doctest::skip(!kNative)) {
    Machine m;
    IrBuilder b;
    const auto r = b.call(&pack_helper, b.constant(Type::I64, 1), b.load_guest(Type::I64, slot(1)),
                          b.constant(Type::I64, 3));
    b.store_guest(slot(0), r);
    b.exit(b.add_exit());
    m.guest[1] = 2;
    m.run(b);
    const auto result = m.guest[0];
    CHECK(result == 0x10203);
}

namespace {

struct BarrierProbe {
    std::array<std::uint16_t, 2> writers{};
    std::array<std::uint16_t, 2> readers{};
    std::array<std::uint64_t, 4> slot_sequence{};
    std::uint64_t completed_seen = 0;
    std::atomic<bool> failed{false};
    PageBarrierView view{writers.data(), readers.data(), slot_sequence.data(), &completed_seen, &failed};
};

}  // namespace

TEST_CASE("IR barrier leaves through its exit only when the page's packet is newer than completed_seen" *
          doctest::skip(!kNative)) {
    // A 4-byte read barrier, then an 8-byte write barrier at the same address, each with its
    // own exit; slot 1 = 1 before them, 2 after them, which only the fall-through reaches.
    Machine m;
    BarrierProbe p;
    IrBuilder b;
    const auto paddr = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(0)));
    b.store_guest(slot(1), b.constant(Type::I64, 1));
    const ExitId read_slow = b.add_exit(), write_slow = b.add_exit(), done = b.add_exit();
    b.barrier(paddr, 4, false, read_slow);
    b.barrier(b.binary(Op::Add, paddr, b.constant(Type::I32, 0)), 8, true, write_slow);
    b.store_guest(slot(1), b.constant(Type::I64, 2));
    b.exit(done);
    const auto fn = m.build(b);
    const auto run = [&] {
        m.guest[1] = 0;
        return fn(&m.ctx);
    };

    m.ctx.barrier = nullptr;  // no barrier: nothing happens
    p.writers[0] = 1;
    p.slot_sequence[1] = 5;
    CHECK(run() == done);
    CHECK(m.guest[1] == 2);

    m.ctx.barrier = &p.view;
    m.guest[0] = 0x123;  // page 0
    p.completed_seen = 5;
    CHECK(run() == done);
    CHECK(m.guest[1] == 2);

    p.completed_seen = 4;  // the writer's packet may still run: before the access
    CHECK(run() == read_slow);
    CHECK(m.guest[1] == 1);

    p.completed_seen = 5;
    p.failed = true;  // every access goes on to the slow path
    CHECK(run() == read_slow);
    p.failed = false;

    // Only a write looks at the readers.
    p.readers[1] = 2;
    p.slot_sequence[2] = 9;
    m.guest[0] = 0x1004;  // page 1
    CHECK(run() == write_slow);
    CHECK(m.guest[1] == 1);
    p.completed_seen = 9;
    CHECK(run() == done);
}

namespace {

struct WriteProbe {
    unsigned calls = 0;
    std::uint32_t paddr = 0, bytes = 0;
};

WriteProbe* write_probe = nullptr;

std::uint32_t count_write(Context*, std::uint32_t paddr, std::uint32_t bytes) noexcept {
    ++write_probe->calls;
    write_probe->paddr = paddr;
    write_probe->bytes = bytes;
    return 1;
}

}  // namespace

TEST_CASE("IR note_ram_write calls code_write only for marked pages and returns its answer" *
          doctest::skip(!kNative)) {
    Machine m;
    WriteProbe p;
    write_probe = &p;
    m.ctx.code_write = &count_write;
    IrBuilder b;
    const auto paddr = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(0)));
    b.store_guest(slot(1), b.convert(Op::Zext32, Type::I64, b.note_ram_write(paddr, 4)));
    b.exit(b.add_exit());
    const auto fn = m.build(b);

    m.guest[0] = 0x1010;
    m.guest[1] = 0xDEAD;
    fn(&m.ctx);
    const auto unmarked = m.guest[1];
    CHECK(p.calls == 0);
    CHECK(unmarked == 0);

    m.code_pages[1] = 1;
    fn(&m.ctx);
    const auto changed = m.guest[1];
    CHECK(p.calls == 1);
    CHECK(changed == 1);
    CHECK(p.paddr == 0x1010);
    CHECK(p.bytes == 4);
    write_probe = nullptr;
}

TEST_CASE("IR reads and writes the context's budget fields" * doctest::skip(!kNative)) {
    Machine m;
    IrBuilder b;
    const auto budget = b.load_context(ContextField::Budget);
    b.store_context(ContextField::ExtraCycles, b.binary(Op::Add, budget, b.load_guest(Type::I64, slot(0))));
    b.store_context(ContextField::Budget, b.constant(Type::I64, 0));
    b.exit(b.add_exit());
    m.ctx.budget = 40;
    m.guest[0] = 2;
    m.run(b);
    CHECK(m.ctx.extra_cycles == 42);
    CHECK(m.ctx.budget == 0);
}

TEST_CASE("IR rejects float types and misuse") {
    {
        IrBuilder builder;
        CHECK_THROWS_AS(builder.constant(Type::F32, 0), std::logic_error);
        CHECK_THROWS_AS(builder.load_guest(Type::F64, 0), std::logic_error);
        CHECK_THROWS_AS(builder.convert(Op::Sext32, Type::F32x2, builder.constant(Type::I64, 1)),
                        std::logic_error);
        CHECK_THROWS_AS(builder.load_ram(Type::F64, builder.constant(Type::I32, 0), 8), std::logic_error);
    }
    {
        IrBuilder b;  // mismatched operand types and bad operands
        const auto a = b.constant(Type::I32, 1);
        const auto w = b.constant(Type::I64, 1);
        CHECK_THROWS_AS(b.binary(Op::Add, a, w), std::logic_error);
        CHECK_THROWS_AS(b.binary(Op::Compare, a, a), std::logic_error);
        CHECK_THROWS_AS(b.binary(Op::Add, a, Value{}), std::logic_error);
        CHECK_THROWS_AS(b.select(w, a, a), std::logic_error);
        CHECK_THROWS_AS(b.load_ram(Type::I32, a, 8), std::logic_error);
        CHECK_THROWS_AS(b.load_ram(Type::I64, w, 4), std::logic_error);
        CHECK_THROWS_AS(b.overflow(Op::Mul, a, a), std::logic_error);
        CHECK_THROWS_AS(b.store_context(ContextField::Budget, a), std::logic_error);
        CHECK_THROWS_AS(b.call(&pack_helper, w, w, a), std::logic_error);
        CHECK_THROWS_AS(b.exit_if(a, 0), std::logic_error);  // no such exit
    }
    {
        IrBuilder b;  // the body must end in exit()
        b.constant(Type::I32, 1);
        CHECK_THROWS_AS((void)b.finish(), std::logic_error);
    }
    {
        IrBuilder b;  // nothing after the terminating exit
        b.exit(b.add_exit());
        CHECK_THROWS_AS(b.constant(Type::I32, 1), std::logic_error);
    }
    {
        IrBuilder b;  // an exit region must be closed, once, and cannot exit or nest
        const auto e0 = b.add_exit();
        const auto e1 = b.add_exit();
        b.begin_exit(e1);
        CHECK_THROWS_AS(b.begin_exit(e0), std::logic_error);
        CHECK_THROWS_AS(b.exit(e0), std::logic_error);
        const auto local = b.constant(Type::I64, 2);
        b.end_exit();
        CHECK_THROWS_AS(b.begin_exit(e1), std::logic_error);
        CHECK_THROWS_AS(b.end_exit(), std::logic_error);
        CHECK_THROWS_AS(b.store_guest(0, local), std::logic_error);  // region values stay in the region
        b.exit(e0);
        b.begin_exit(e0);  // left open
        CHECK_THROWS_AS((void)b.finish(), std::logic_error);
    }
    {
        IrBuilder b;  // a region cannot use a value defined after a jump to it
        const auto e0 = b.add_exit();
        const auto e1 = b.add_exit();
        b.exit_if(b.constant(Type::I32, 1), e1);
        const auto late = b.constant(Type::I64, 3);
        b.begin_exit(e1);
        b.store_guest(0, late);
        b.end_exit();
        b.exit(e0);
        CHECK_THROWS_AS((void)b.finish(), std::logic_error);
    }
    {
        IrBuilder b;  // a barrier's slow-path exit is a jump like exit_if's
        const auto e0 = b.add_exit();
        const auto e1 = b.add_exit();
        const auto paddr = b.constant(Type::I32, 0);
        CHECK_THROWS_AS(b.barrier(paddr, 4, false, 7), std::logic_error);  // no such exit
        b.begin_exit(e0);
        CHECK_THROWS_AS(b.barrier(paddr, 4, false, e1), std::logic_error);  // not from a region
        b.end_exit();
        b.barrier(paddr, 4, false, e1);
        const auto late = b.constant(Type::I64, 3);
        b.begin_exit(e1);
        b.store_guest(0, late);
        b.end_exit();
        b.exit(e0);
        CHECK_THROWS_AS((void)b.finish(), std::logic_error);
    }
}

TEST_CASE("compile reports OutOfSpace on a tiny buffer" * doctest::skip(!kNative)) {
    IrBuilder b;
    auto sum = b.load_guest(Type::I64, 0);
    for (unsigned i = 0; i < 64; ++i) {
        sum = b.binary(Op::Add, sum, b.load_guest(Type::I64, slot(i % 8)));
        b.store_guest(slot(i % 8), sum);
    }
    b.exit(b.add_exit());
    const auto big_block = b.finish();
    CodeBuffer tiny(64);
    CHECK(tiny.available());
    CHECK(compile(big_block, tiny).error == CompileError::OutOfSpace);
    CHECK(tiny.used() == 0);

    CodeBuffer roomy(1U << 16);
    const auto compiled = compile(big_block, roomy);
    CHECK(compiled.error == CompileError::None);
    CHECK(roomy.used() > 64);
    roomy.reset();
    CHECK(roomy.used() == 0);
}

TEST_CASE("compile reports Unsupported without native code" * doctest::skip(kNative)) {
    IrBuilder b;
    b.exit(b.add_exit());
    CodeBuffer buffer(4096);
    CHECK_FALSE(buffer.available());
    const auto compiled = compile(b.finish(), buffer);
    CHECK(compiled.error == CompileError::Unsupported);
    CHECK(compiled.fn == nullptr);
}
