#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

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
    std::array<std::uint64_t, 64> guest{};
    std::array<std::uint8_t, 8192> ram{};
    std::array<std::uint8_t, 2> code_pages{};
    Context ctx{};
    CodeBuffer buffer;

    explicit Machine(std::size_t code_bytes = 1U << 20) : buffer(code_bytes) {
        ctx.guest = reinterpret_cast<std::uint8_t*>(guest.data());
        ctx.ram = ram.data();
        ctx.ram_size = static_cast<std::uint32_t>(ram.size());
        ctx.code_pages = code_pages.data();
        ctx.user = this;
    }

    BlockFn build(const IrBlock& block) {
        const auto compiled = compile(block, buffer);
        REQUIRE(compiled.error == CompileError::None);
        REQUIRE(compiled.fn != nullptr);
        return compiled.fn;
    }

    BlockFn build(IrBuilder& builder) { return build(builder.finish()); }

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

namespace {

// Pairs of operands that keep every value live until the last pairs: value i meets value i+stride.
template <typename T>
std::uint64_t combine(unsigned which, T x, T y) {
    switch (which % 5) {
        case 0: return static_cast<std::uint64_t>(static_cast<T>(x + y));
        case 1: return static_cast<std::uint64_t>(static_cast<T>(x - y));
        case 2: return static_cast<std::uint64_t>(static_cast<T>(x ^ y));
        case 3: return static_cast<std::uint64_t>(static_cast<T>(x * y));
        default: return static_cast<std::uint64_t>(static_cast<T>(x << (y & (sizeof(T) * 8 - 1))));
    }
}

constexpr std::array<Op, 5> kCombineOps{Op::Add, Op::Sub, Op::Xor, Op::Mul, Op::Shl};

std::uint64_t sum_helper(Context*, std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    return a + 3 * b + 5 * c;
}

}  // namespace

TEST_CASE("spills under pressure" * doctest::skip(!kNative)) {
    constexpr unsigned kWide = 24, kNarrow = 8;
    Machine m;
    for (unsigned i = 0; i < 32; ++i) m.guest[i] = 0x9E37'79B9'7F4A'7C15ULL * (i + 1) + (i << 3);
    IrBuilder b;
    std::array<Value, kWide> wide{};
    std::array<Value, kNarrow> narrow{};
    for (unsigned i = 0; i < kWide; ++i) wide[i] = b.load_guest(Type::I64, slot(i));
    for (unsigned i = 0; i < kNarrow; ++i) narrow[i] = b.load_guest(Type::I32, slot(kWide + i));
    // Every value is read by two pairs, the second 7 (or 3) pairs later, so all 32 stay
    // live until the late pairs.
    for (unsigned i = 0; i < kWide; ++i) {
        b.store_guest(slot(32 + i), b.binary(kCombineOps[i % 5], wide[i], wide[(i + 7) % kWide]));
    }
    for (unsigned i = 0; i < kNarrow; ++i) {
        const auto r = b.binary(kCombineOps[i % 5], narrow[i], narrow[(i + 3) % kNarrow]);
        b.store_guest(slot(56 + i), b.convert(Op::Sext32, Type::I64, r));
    }
    b.exit(b.add_exit());
    const auto input = m.guest;
    m.run(b);
    for (unsigned i = 0; i < kWide; ++i) {
        CAPTURE(i);
        CHECK(m.guest[32 + i] == combine<std::uint64_t>(i, input[i], input[(i + 7) % kWide]));
    }
    for (unsigned i = 0; i < kNarrow; ++i) {
        CAPTURE(i);
        const auto x = static_cast<std::uint32_t>(input[kWide + i]);
        const auto y = static_cast<std::uint32_t>(input[kWide + (i + 3) % kNarrow]);
        const auto r = static_cast<std::uint32_t>(combine<std::uint32_t>(i, x, y));
        CHECK(m.guest[56 + i] == static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(r))));
    }
    for (unsigned i = 0; i < 32; ++i) CHECK(m.guest[i] == input[i]);  // inputs untouched
}

TEST_CASE("a frame larger than a page" * doctest::skip(!kNative)) {
    // 600 live values need more than 4 KiB of spill slots.
    constexpr unsigned kValues = 600;
    Machine m;
    for (unsigned i = 0; i < 16; ++i) m.guest[i] = i * 0x1111ULL + 1;
    IrBuilder b;
    std::vector<Value> values;
    for (unsigned i = 0; i < kValues; ++i) {
        values.push_back(b.binary(Op::Add, b.load_guest(Type::I64, slot(i % 16)), b.constant(Type::I64, i)));
    }
    auto sum = b.constant(Type::I64, 0);
    for (unsigned i = kValues; i-- > 0;) {
        sum = b.binary(Op::Xor, b.binary(Op::Mul, sum, b.constant(Type::I64, 3)), values[i]);
    }
    b.store_guest(slot(20), sum);
    b.exit(b.add_exit());
    m.run(b);
    std::uint64_t expected = 0;
    for (unsigned i = kValues; i-- > 0;) expected = (expected * 3) ^ (m.guest[i % 16] + i);
    CHECK(m.guest[20] == expected);
}

TEST_CASE("values survive a helper call" * doctest::skip(!kNative)) {
    constexpr unsigned kLive = 12;  // more than the callee-saved registers
    Machine m;
    for (unsigned i = 0; i < kLive; ++i) m.guest[i] = 0x0101'0101'0101'0101ULL * (i + 1);
    IrBuilder b;
    std::array<Value, kLive> live{};
    for (unsigned i = 0; i < kLive; ++i) live[i] = b.load_guest(Type::I64, slot(i));
    const auto narrow = b.convert(Op::Trunc, Type::I32, live[0]);
    const auto r = b.call(&sum_helper, live[1], live[2], live[3]);
    for (unsigned i = 0; i < kLive; ++i) b.store_guest(slot(16 + i), b.binary(Op::Add, live[i], r));
    b.store_guest(slot(30), b.convert(Op::Zext32, Type::I64, narrow));
    b.store_guest(slot(31), b.call(&sum_helper, live[kLive - 1], r, live[0]));
    for (unsigned i = 0; i < kLive; ++i) b.store_guest(slot(32 + i), live[i]);
    b.exit(b.add_exit());
    const auto input = m.guest;
    m.run(b);
    const auto expected_r = input[1] + 3 * input[2] + 5 * input[3];
    for (unsigned i = 0; i < kLive; ++i) {
        CAPTURE(i);
        CHECK(m.guest[16 + i] == input[i] + expected_r);
        CHECK(m.guest[32 + i] == input[i]);
    }
    CHECK(m.guest[30] == (input[0] & 0xFFFF'FFFFULL));
    CHECK(m.guest[31] == input[kLive - 1] + 3 * expected_r + 5 * input[0]);
}

TEST_CASE("an exit taken from two sites" * doctest::skip(!kNative)) {
    constexpr unsigned kCrowd = 14;
    Machine m;
    IrBuilder b;
    const auto done = b.add_exit();
    const auto side = b.add_exit();
    const auto v = b.load_guest(Type::I64, slot(0));
    const auto first = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(1)));
    b.begin_exit(side);
    b.store_guest(slot(2), v);
    b.store_guest(slot(3), b.binary(Op::Add, v, b.constant(Type::I64, 1)));
    b.end_exit();
    b.exit_if(first, side);  // v sits in a register here: few values are live
    // The crowd is read (stored) before the second site, so every crowd value is nearer than
    // v's next read, the second site itself: eviction takes v first and it sits in a spill
    // slot there, where the exit's copy has to read it from.
    std::array<Value, kCrowd> crowd{};
    for (unsigned i = 0; i < kCrowd; ++i) {
        crowd[i] = b.binary(Op::Add, b.load_guest(Type::I64, slot(8 + i)), b.constant(Type::I64, i));
    }
    for (unsigned i = 0; i < kCrowd; ++i) b.store_guest(slot(32 + i), crowd[i]);
    const auto second = b.convert(Op::Trunc, Type::I32, b.load_guest(Type::I64, slot(4)));
    b.exit_if(second, side);
    b.exit(done);
    const auto fn = m.build(b);

    for (unsigned path = 0; path < 3; ++path) {
        CAPTURE(path);
        m.guest.fill(0);
        m.guest[0] = 0x1234'5678'9ABC'DEF0ULL;
        m.guest[1] = path == 0 ? 1 : 0;
        m.guest[4] = path == 1 ? 1 : 0;
        for (unsigned i = 0; i < kCrowd; ++i) m.guest[8 + i] = 100 * i;
        const auto tag = fn(&m.ctx);
        CHECK(tag == (path < 2 ? side : done));
        if (path < 2) {
            CHECK(m.guest[2] == 0x1234'5678'9ABC'DEF0ULL);
            CHECK(m.guest[3] == 0x1234'5678'9ABC'DEF1ULL);
        } else {
            CHECK(m.guest[2] == 0);
        }
        // The first site leaves before the crowd's stores; the other two run them.
        for (unsigned i = 0; i < kCrowd; ++i) CHECK(m.guest[32 + i] == (path == 0 ? 0 : 100 * i + i));
    }
}

namespace {

// A small deterministic generator (Knuth's MMIX LCG).
struct Lcg {
    std::uint64_t state;
    std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    }
    std::uint32_t below(std::size_t n) { return static_cast<std::uint32_t>((next() >> 33) % n); }
    bool chance(unsigned percent) { return below(100) < percent; }
};

std::uint64_t mix_helper(Context* ctx, std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
    ctx->extra_cycles += 1;
    return (a * 0x9E37'79B9'7F4A'7C15ULL) ^ (b + 0x632B'E59B'D9B4'E019ULL) ^ (c << 7) ^ (c >> 3);
}

std::uint32_t parity_write(Context*, std::uint32_t paddr, std::uint32_t bytes) noexcept {
    return (paddr ^ bytes) & 1U;
}

// Guest slots 0-15 are inputs, 16-47 outputs; RAM is 8 KiB (two pages).
class RandomBlock {
public:
    explicit RandomBlock(Lcg& rng) : rng_(rng) {}

    IrBlock build() {
        const ExitId done = b_.add_exit();
        for (unsigned k = 0; k < 3; ++k) {
            i64_.push_back(b_.load_guest(Type::I64, slot(rng_.below(16))));
            i32_.push_back(b_.load_guest(Type::I32, slot(rng_.below(16)) + 4 * rng_.below(2)));
        }
        const unsigned ops = 20 + rng_.below(80);
        for (unsigned n = 0; n < ops; ++n) step();
        region(done);
        b_.exit(done);
        return b_.finish();
    }

private:
    Value pick(Type type) {
        const auto& pool = type == Type::I64 ? i64_ : i32_;
        return pool[rng_.below(pool.size())];
    }
    Type any_type() { return rng_.chance(50) ? Type::I64 : Type::I32; }
    void keep(Value v, Type type) { (type == Type::I64 ? i64_ : i32_).push_back(v); }

    Value constant(Type type) {
        switch (rng_.below(4)) {
            case 0: return b_.constant(type, 0);
            case 1: return b_.constant(type, rng_.below(64));
            case 2: return b_.constant(type, ~std::uint64_t{0} - rng_.below(4));
            default: return b_.constant(type, rng_.next());
        }
    }

    unsigned bytes(Type type) {
        constexpr std::array<unsigned, 4> sizes{1, 2, 4, 8};
        return sizes[rng_.below(type == Type::I64 ? 4 : 3)];
    }

    // An 8-byte aligned physical address within RAM: no access crosses a page.
    Value paddr() { return b_.binary(Op::And, pick(Type::I32), b_.constant(Type::I32, 0x1FF8)); }

    void store() {
        constexpr std::array<unsigned, 4> sizes{0, 1, 2, 4};
        b_.store_guest(slot(16 + rng_.below(32)), pick(any_type()), sizes[rng_.below(4)]);
    }

    // An exit's ops: stores of body values, some computed in the region.
    void region(ExitId exit) {
        b_.begin_exit(exit);
        const unsigned stores = 1 + rng_.below(4);
        for (unsigned k = 0; k < stores; ++k) store();
        if (rng_.chance(50)) {
            const auto type = any_type();
            b_.store_guest(slot(16 + rng_.below(32)), b_.binary(Op::Xor, pick(type), constant(type)));
        }
        if (rng_.chance(20)) {
            b_.store_guest(slot(16 + rng_.below(32)),
                           b_.call(&mix_helper, pick(Type::I64), pick(Type::I64), pick(Type::I64)));
        }
        b_.end_exit();
    }

    ExitId side_exit() {
        if (!side_.empty() && rng_.chance(40)) return side_[rng_.below(side_.size())];
        const ExitId exit = b_.add_exit();
        region(exit);
        side_.push_back(exit);
        return exit;
    }

    void convert() {
        const auto from = pick(any_type());
        switch (rng_.below(4)) {
            case 0: {
                const auto to = any_type();
                keep(b_.convert(rng_.chance(50) ? Op::Sext8 : Op::Sext16, to, from), to);
                break;
            }
            case 1: keep(b_.convert(Op::Sext32, Type::I64, from), Type::I64); break;
            case 2: keep(b_.convert(Op::Zext32, Type::I64, from), Type::I64); break;
            default: keep(b_.convert(Op::Trunc, Type::I32, from), Type::I32); break;
        }
    }

    void step() {
        const auto type = any_type();
        switch (rng_.below(17)) {
            case 0: keep(constant(type), type); break;
            case 1: {
                const auto offset = slot(rng_.below(16)) + (type == Type::I32 ? 4 * rng_.below(2) : 0);
                keep(b_.load_guest(type, offset), type);
                break;
            }
            case 2: case 3: case 4: {
                const auto op = static_cast<Op>(static_cast<unsigned>(Op::Add) + rng_.below(12));
                keep(b_.binary(op, pick(type), pick(type)), type);
                break;
            }
            case 5: convert(); break;
            case 6: keep(b_.compare(static_cast<Cond>(rng_.below(8)), pick(type), pick(type)), Type::I32); break;
            case 7: keep(b_.select(pick(Type::I32), pick(type), pick(type)), type); break;
            case 8: {
                const auto which = rng_.below(3);
                const auto v = which == 2 ? b_.carry_add(pick(type), pick(type))
                                          : b_.overflow(which == 0 ? Op::Add : Op::Sub, pick(type), pick(type));
                keep(v, Type::I32);
                break;
            }
            case 9: case 10: store(); break;
            case 11:
                keep(b_.call(&mix_helper, pick(Type::I64), pick(Type::I64), pick(Type::I64)), Type::I64);
                break;
            case 12: {
                // About one in eight is taken.
                const auto low = b_.binary(Op::And, pick(Type::I32), b_.constant(Type::I32, 7));
                const auto cond = b_.compare(Cond::Eq, low, b_.constant(Type::I32, rng_.below(8)));
                b_.exit_if(cond, side_exit());
                break;
            }
            case 13: {
                const auto at = paddr();
                if (rng_.chance(50)) keep(b_.load_ram(type, at, bytes(type)), type);
                else b_.store_ram(at, pick(type), bytes(type));
                break;
            }
            case 14: {
                const auto at = paddr();
                b_.barrier(at, bytes(Type::I64), rng_.chance(50), side_exit());
                break;
            }
            case 15: keep(b_.note_ram_write(paddr(), bytes(Type::I64)), Type::I32); break;
            default:
                if (rng_.chance(50)) keep(b_.load_context(ContextField::ExtraCycles), Type::I64);
                else b_.store_context(ContextField::Budget, pick(Type::I64));
                break;
        }
    }

    Lcg& rng_;
    IrBuilder b_;
    std::vector<Value> i32_, i64_;
    std::vector<ExitId> side_;
};

}  // namespace

namespace {

// A reference for the random blocks: runs the IR directly, op by op, on a Context. It is the
// oracle for the native code, so it is written for clarity.
class IrInterpreter {
public:
    IrInterpreter(const IrBlock& block, Context& ctx) : block_(block), ctx_(ctx), values_(block.value_count()) {}

    // The id of the exit the block leaves through (a body always ends in one).
    std::uint32_t run() {
        std::uint32_t exit = 0;
        execute(block_.body(), exit);
        return exit;
    }

private:
    static std::uint64_t mask(Type type) { return type == Type::I64 ? ~0ULL : 0xFFFF'FFFFULL; }
    static unsigned width(Type type) { return type == Type::I64 ? 64 : 32; }

    static std::uint64_t sign_extend(std::uint64_t value, unsigned bits) {
        const auto shift = 64 - bits;
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(value << shift) >> shift);
    }

    static std::uint64_t mul_high_unsigned(std::uint64_t x, std::uint64_t y) {
        const std::uint64_t xl = x & 0xFFFF'FFFFULL, xh = x >> 32, yl = y & 0xFFFF'FFFFULL, yh = y >> 32;
        const std::uint64_t ll = xl * yl, lh = xl * yh, hl = xh * yl, hh = xh * yh;
        const std::uint64_t mid = (ll >> 32) + (lh & 0xFFFF'FFFFULL) + (hl & 0xFFFF'FFFFULL);
        return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    }

    std::uint64_t& v(std::uint32_t id) { return values_[id]; }

    static std::uint64_t load_bytes(const std::uint8_t* at, unsigned bytes) {
        std::uint64_t value = 0;
        std::memcpy(&value, at, bytes);
        return value;
    }

    static std::uint64_t big_endian(const std::uint8_t* at, unsigned bytes) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | at[i];
        return value;
    }

    std::uint8_t* field(ContextField which) { return reinterpret_cast<std::uint8_t*>(&ctx_) + context_offset(which); }

    std::uint64_t binary(const Inst& inst) {
        const auto type = inst.type;
        const auto a = v(inst.a) & mask(type), b = v(inst.b) & mask(type);
        const auto count = static_cast<unsigned>(v(inst.b)) & (width(type) - 1);
        switch (inst.op) {
            case Op::Add: return a + b;
            case Op::Sub: return a - b;
            case Op::And: return a & b;
            case Op::Or: return a | b;
            case Op::Xor: return a ^ b;
            case Op::Nor: return ~(a | b);
            case Op::Shl: return a << count;
            case Op::Shr: return a >> count;
            case Op::Sar:
                return static_cast<std::uint64_t>(static_cast<std::int64_t>(sign_extend(a, width(type))) >> count);
            case Op::Mul: return a * b;
            case Op::MulHiU: return type == Type::I64 ? mul_high_unsigned(a, b) : (a * b) >> 32;
            default: {  // MulHiS
                if (type == Type::I32) {
                    const auto product = static_cast<std::int64_t>(sign_extend(a, 32)) *
                                         static_cast<std::int64_t>(sign_extend(b, 32));
                    return static_cast<std::uint64_t>(product >> 32);
                }
                auto high = mul_high_unsigned(a, b);
                if (static_cast<std::int64_t>(a) < 0) high -= b;
                if (static_cast<std::int64_t>(b) < 0) high -= a;
                return high;
            }
        }
    }

    static bool compare(Cond cond, std::uint64_t a, std::uint64_t b, Type type) {
        const auto x = a & mask(type), y = b & mask(type);
        const auto sx = static_cast<std::int64_t>(sign_extend(x, width(type)));
        const auto sy = static_cast<std::int64_t>(sign_extend(y, width(type)));
        switch (cond) {
            case Cond::Eq: return x == y;
            case Cond::Ne: return x != y;
            case Cond::LtS: return sx < sy;
            case Cond::LtU: return x < y;
            case Cond::LeS: return sx <= sy;
            case Cond::GtS: return sx > sy;
            case Cond::GeS: return sx >= sy;
            case Cond::GeU: return x >= y;
        }
        return false;
    }

    static bool flag(const Inst& inst, std::uint64_t x, std::uint64_t y, Type type) {
        const auto a = x & mask(type), b = y & mask(type);
        const auto sum = (a + b) & mask(type), diff = (a - b) & mask(type);
        const auto sign = 1ULL << (width(type) - 1);
        if (inst.op == Op::CarryAdd) return sum < a;
        if (inst.op == Op::OverflowAdd) return ((a ^ sum) & (b ^ sum) & sign) != 0;
        return ((a ^ b) & (a ^ diff) & sign) != 0;
    }

    // The renderer's barrier: the access must wait for a newer packet, or the renderer failed.
    bool barrier_waits(const Inst& inst) {
        const auto* view = ctx_.barrier;
        if (view == nullptr) return false;
        const auto page = static_cast<std::uint32_t>(v(inst.a)) >> 12;
        auto sequence = view->slot_sequence[view->writers[page]];
        if ((inst.imm & 1) != 0) sequence = std::max(sequence, view->slot_sequence[view->readers[page]]);
        return sequence > *view->completed_seen || view->failed->load();
    }

    // Runs an exit's ops and records which exit the block returns.
    bool leave(ExitId target, std::uint32_t& exit) {
        execute(block_.exit_ops(target), exit);
        exit = target;
        return true;
    }

    // true: the block left through an exit.
    bool execute(const std::vector<Inst>& ops, std::uint32_t& exit) {
        for (const auto& inst : ops) {
            const auto type = inst.type;
            switch (inst.op) {
                case Op::Const: v(inst.result) = inst.imm & mask(type); break;
                case Op::LoadGuest: v(inst.result) = load_bytes(ctx_.guest + inst.imm, type == Type::I64 ? 8 : 4); break;
                case Op::StoreGuest: {
                    const auto bytes = inst.aux != 0 ? inst.aux : (block_.type_of(inst.a) == Type::I64 ? 8U : 4U);
                    std::memcpy(ctx_.guest + inst.imm, &v(inst.a), bytes);
                    break;
                }
                case Op::LoadContext:
                    v(inst.result) = load_bytes(field(static_cast<ContextField>(inst.aux)), 8) & mask(type);
                    break;
                case Op::StoreContext: std::memcpy(field(static_cast<ContextField>(inst.aux)), &v(inst.a), 8); break;
                case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: case Op::Nor:
                case Op::Shl: case Op::Shr: case Op::Sar: case Op::Mul: case Op::MulHiS: case Op::MulHiU:
                    v(inst.result) = binary(inst) & mask(type);
                    break;
                case Op::Sext8: v(inst.result) = sign_extend(v(inst.a), 8) & mask(type); break;
                case Op::Sext16: v(inst.result) = sign_extend(v(inst.a), 16) & mask(type); break;
                case Op::Sext32: v(inst.result) = sign_extend(v(inst.a), 32) & mask(type); break;
                case Op::Zext32: case Op::Trunc: v(inst.result) = v(inst.a) & 0xFFFF'FFFFULL; break;
                case Op::Compare:
                    v(inst.result) = compare(static_cast<Cond>(inst.aux), v(inst.a), v(inst.b), block_.type_of(inst.a));
                    break;
                case Op::Select:
                    v(inst.result) = ((v(inst.a) & 0xFFFF'FFFFULL) != 0 ? v(inst.b) : v(inst.c)) & mask(type);
                    break;
                case Op::OverflowAdd: case Op::OverflowSub: case Op::CarryAdd:
                    v(inst.result) = flag(inst, v(inst.a), v(inst.b), block_.type_of(inst.a));
                    break;
                case Op::LoadRam:
                    v(inst.result) = big_endian(ctx_.ram + static_cast<std::uint32_t>(v(inst.a)), inst.aux) & mask(type);
                    break;
                case Op::StoreRam: {
                    const auto at = static_cast<std::uint32_t>(v(inst.a));
                    for (unsigned i = 0; i < inst.aux; ++i) {
                        ctx_.ram[at + i] = static_cast<std::uint8_t>(v(inst.b) >> (8 * (inst.aux - 1 - i)));
                    }
                    break;
                }
                case Op::Barrier:
                    if (barrier_waits(inst)) return leave(static_cast<ExitId>(inst.imm >> Inst::kBarrierExitShift), exit);
                    break;
                case Op::NoteRamWrite: {
                    const auto at = static_cast<std::uint32_t>(v(inst.a));
                    v(inst.result) = ctx_.code_pages[at >> 12] != 0 ? ctx_.code_write(&ctx_, at, inst.aux) : 0;
                    break;
                }
                case Op::Call:
                    v(inst.result) = reinterpret_cast<Helper>(inst.imm)(&ctx_, v(inst.a), v(inst.b), v(inst.c));
                    break;
                case Op::ExitIf:
                    if ((v(inst.a) & mask(block_.type_of(inst.a))) != 0) {
                        return leave(static_cast<ExitId>(inst.imm), exit);
                    }
                    break;
                case Op::Exit: return leave(static_cast<ExitId>(inst.imm), exit);
            }
        }
        return false;
    }

    const IrBlock& block_;
    Context& ctx_;
    std::vector<std::uint64_t> values_;
};

}  // namespace

TEST_CASE("random IR blocks match the reference interpreter" * doctest::skip(!kNative)) {
    Lcg rng{0x5EED'2026'1121ULL};
    Machine fast{32U << 20};
    Machine reference;
    // Page 1 holds compiled code and has a renderer packet newer than completed_seen.
    BarrierProbe probe;
    probe.writers[1] = 1;
    probe.slot_sequence[1] = 5;
    probe.completed_seen = 4;
    for (auto* m : {&fast, &reference}) {
        m->code_pages[1] = 1;
        m->ctx.code_write = &parity_write;
        m->ctx.barrier = &probe.view;
    }
    unsigned at_end = 0;
    for (unsigned n = 0; n < 500; ++n) {
        CAPTURE(n);
        const auto block = RandomBlock(rng).build();
        std::array<std::uint64_t, 64> guest{};
        for (auto& g : guest) g = rng.next();
        std::array<std::uint8_t, 8192> ram{};
        for (auto& r : ram) r = static_cast<std::uint8_t>(rng.next() >> 56);
        for (auto* m : {&fast, &reference}) {
            m->guest = guest;
            m->ram = ram;
            m->ctx.budget = 1000;
            m->ctx.extra_cycles = 0;
        }
        const auto tag = fast.build(block)(&fast.ctx);
        const auto expected = IrInterpreter(block, reference.ctx).run();
        CHECK(tag == expected);
        CHECK(fast.guest == reference.guest);
        CHECK(fast.ram == reference.ram);
        CHECK(fast.ctx.budget == reference.ctx.budget);
        CHECK(fast.ctx.extra_cycles == reference.ctx.extra_cycles);
        if (tag == 0) ++at_end;
    }
    // The blocks leave through the body's end and through side exits.
    CHECK(at_end > 0);
    CHECK(at_end < 500);
}
