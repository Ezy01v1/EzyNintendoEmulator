#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "jit/ir.hpp"
#include "jit/reg_alloc.hpp"

using namespace ezy::jit;

namespace {

using Kind = Location::Kind;

constexpr Location reg(std::uint32_t index) { return {Kind::Register, index}; }
constexpr Location slot(std::uint32_t index) { return {Kind::Slot, index}; }

std::uint64_t ignore(Context*, std::uint64_t, std::uint64_t, std::uint64_t) noexcept { return 0; }

}  // namespace

TEST_CASE("use positions count jumps as reads of their exit's body values") {
    IrBuilder b;
    const auto exit = b.add_exit();
    const auto taken = b.add_exit();
    const auto v = b.load_guest(Type::I64, 0);       // 0
    const auto cond = b.constant(Type::I32, 1);      // 1
    b.begin_exit(taken);
    b.store_guest(8, v);                             // exit 1, op 0
    b.end_exit();
    b.exit_if(cond, taken);                          // 2
    b.store_guest(16, b.constant(Type::I64, 3));     // 3, 4
    b.exit(exit);                                    // 5
    const auto block = b.finish();
    const UsePositions uses(block);

    CHECK(uses.body_size() == 6);
    CHECK(uses.exit_begin(exit) == 6);
    CHECK(uses.exit_end(exit) == 6);
    CHECK(uses.exit_begin(taken) == 6);
    CHECK(uses.exit_end(taken) == 7);
    CHECK(uses.next(v.id, 0) == 2);       // the exit_if needs it for its exit
    CHECK(uses.next(v.id, 3) == 6);       // then only the exit's store reads it
    CHECK(uses.next(v.id, 7) == kNever);
    CHECK(uses.next(cond.id, 0) == 2);
    CHECK(uses.next(cond.id, 3) == kNever);
}

TEST_CASE("eviction spills the value used farthest away") {
    IrBuilder b;
    const auto v0 = b.load_guest(Type::I64, 0);  // 0
    const auto v1 = b.load_guest(Type::I64, 8);  // 1
    const auto v2 = b.load_guest(Type::I64, 16); // 2
    b.store_guest(24, v1);                       // 3
    b.store_guest(32, v0);                       // 4
    b.store_guest(40, v2);                       // 5
    b.exit(b.add_exit());                        // 6
    const auto block = b.finish();
    const UsePositions uses(block);
    RegAlloc alloc(2, 0, uses);
    std::vector<Move> moves;

    CHECK(alloc.define(v0.id, 0, moves) == 0);
    alloc.release_dead(1);
    CHECK(alloc.define(v1.id, 1, moves) == 1);
    alloc.release_dead(2);
    CHECK(moves.empty());

    // v0 is next read at 4, v1 at 3: v0 leaves.
    CHECK(alloc.define(v2.id, 2, moves) == 0);
    alloc.release_dead(3);
    REQUIRE(moves.size() == 1);
    CHECK(moves[0] == Move{v0.id, reg(0), slot(0)});
    CHECK(alloc.where(v0.id) == slot(0));
    CHECK(alloc.where(v2.id) == reg(0));
    moves.clear();

    CHECK(alloc.get(v1.id, 3, moves) == 1);
    alloc.release_dead(4);  // v1 is dead: its register is free
    CHECK(moves.empty());
    CHECK(alloc.get(v0.id, 4, moves) == 1);
    REQUIRE(moves.size() == 1);
    CHECK(moves[0] == Move{v0.id, slot(0), reg(1)});
    CHECK(alloc.slot_count() == 1);
    CHECK(alloc.used_registers() == 0b11U);
}

TEST_CASE("an evicted value with a slot copy leaves without a store") {
    IrBuilder b;
    const auto v0 = b.load_guest(Type::I64, 0);  // 0
    const auto v1 = b.load_guest(Type::I64, 8);  // 1
    b.store_guest(16, v0);                       // 2: reloads v0
    const auto v2 = b.load_guest(Type::I64, 24); // 3: evicts v0 again
    b.store_guest(32, v1);                       // 4
    b.store_guest(40, v2);                       // 5
    b.store_guest(48, v0);                       // 6
    b.exit(b.add_exit());
    const auto block = b.finish();
    const UsePositions uses(block);
    RegAlloc alloc(1, 0, uses);
    std::vector<Move> moves;
    (void)alloc.define(v0.id, 0, moves);
    alloc.release_dead(1);
    (void)alloc.define(v1.id, 1, moves);  // v0 (next 2) vs nothing else: v0 spills
    alloc.release_dead(2);
    (void)alloc.get(v0.id, 2, moves);      // v1 spills, v0 reloads
    alloc.release_dead(3);
    moves.clear();
    (void)alloc.define(v2.id, 3, moves);   // v0 already has its slot: no store
    REQUIRE(moves.empty());
    CHECK(alloc.where(v0.id).kind == Kind::Slot);
}

TEST_CASE("before_call keeps only live volatile values safe") {
    IrBuilder b;
    const auto v0 = b.load_guest(Type::I64, 0);   // 0
    const auto v1 = b.load_guest(Type::I64, 8);   // 1
    const auto v2 = b.load_guest(Type::I64, 16);  // 2
    const auto r = b.call(&ignore, v2, v2, v2);   // 3
    b.store_guest(24, v1);                        // 4
    b.store_guest(32, r);                         // 5
    b.store_guest(40, v0);                        // 6
    b.exit(b.add_exit());
    const auto block = b.finish();
    const UsePositions uses(block);

    SUBCASE("spilled when no callee-saved register is free") {
        RegAlloc alloc(3, 0b110, uses);  // registers 1 and 2 are volatile
        std::vector<Move> moves;
        CHECK(alloc.define(v0.id, 0, moves) == 0);
        alloc.release_dead(1);
        CHECK(alloc.define(v1.id, 1, moves) == 1);
        alloc.release_dead(2);
        CHECK(alloc.define(v2.id, 2, moves) == 2);
        alloc.release_dead(3);
        CHECK(alloc.where(v2.id) == reg(2));  // the call reads it
        alloc.before_call(3, moves);
        REQUIRE(moves.size() == 1);  // v2 is dead after the call: no move
        CHECK(moves[0] == Move{v1.id, reg(1), slot(0)});
        CHECK(alloc.where(v1.id) == slot(0));
        CHECK(alloc.where(v0.id) == reg(0));
    }
    SUBCASE("moved to a free callee-saved register first") {
        RegAlloc alloc(4, 0b0011, uses);  // registers 0 and 1 are volatile, 3 stays free
        std::vector<Move> moves;
        CHECK(alloc.define(v0.id, 0, moves) == 0);
        alloc.release_dead(1);
        CHECK(alloc.define(v1.id, 1, moves) == 1);
        alloc.release_dead(2);
        CHECK(alloc.define(v2.id, 2, moves) == 2);
        alloc.release_dead(3);
        alloc.before_call(3, moves);
        REQUIRE(moves.size() == 2);
        CHECK(moves[0] == Move{v0.id, reg(0), reg(3)});
        CHECK(moves[1] == Move{v1.id, reg(1), slot(0)});
        CHECK(alloc.where(v0.id) == reg(3));
        CHECK(alloc.where(v2.id) == reg(2));  // callee-saved: untouched
        CHECK(alloc.used_registers() == 0b1111U);
    }
}

TEST_CASE("snapshot and restore give the state of a jump site") {
    IrBuilder b;
    const auto done = b.add_exit();
    const auto side = b.add_exit();
    const auto v0 = b.load_guest(Type::I64, 0);    // 0
    const auto cond = b.constant(Type::I32, 1);    // 1
    b.begin_exit(side);
    b.store_guest(8, v0);
    b.end_exit();
    b.exit_if(cond, side);                         // 2
    const auto v1 = b.load_guest(Type::I64, 16);   // 3
    const auto v2 = b.load_guest(Type::I64, 24);   // 4
    b.store_guest(32, v1);                         // 5
    b.store_guest(40, v2);                         // 6
    b.exit(done);                                  // 7
    const auto block = b.finish();
    const UsePositions uses(block);
    RegAlloc alloc(2, 0, uses);
    std::vector<Move> moves;

    (void)alloc.define(v0.id, 0, moves);
    alloc.release_dead(1);
    (void)alloc.define(cond.id, 1, moves);
    alloc.release_dead(2);
    CHECK(alloc.get(cond.id, 2, moves) == 1);
    const auto site = alloc.snapshot();
    alloc.release_dead(3);  // cond and v0 are dead in the body after the jump
    CHECK(alloc.define(v1.id, 3, moves) == 0);
    alloc.release_dead(4);
    CHECK(alloc.define(v2.id, 4, moves) == 1);
    CHECK(moves.empty());
    alloc.release_dead(5);

    // The exit's copy starts from the site: v0 is back in register 0, with nothing to load.
    alloc.restore(site);
    alloc.set_region_end(uses.exit_end(side));
    alloc.release_dead(uses.exit_begin(side));
    CHECK(alloc.where(v0.id) == reg(0));
    CHECK(alloc.get(v0.id, uses.exit_begin(side), moves) == 0);
    CHECK(moves.empty());
    alloc.release_dead(uses.exit_begin(side));
    CHECK(alloc.where(cond.id).kind == Kind::None);  // the exit never reads it
}
