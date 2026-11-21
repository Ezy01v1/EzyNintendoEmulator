#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "jit/ir.hpp"

// Private to ezy_jit (the Assembler backend and its tests): a linear register allocator over
// abstract register indices. It knows neither AsmJit nor the host's register names.
namespace ezy::jit {

inline constexpr std::uint32_t kNever = ~0U;

// Where each value is read, on one numbering of a block's ops: the body at 0..n-1, then each
// exit's ops, exit by exit. A jump (ExitIf, Barrier, Exit) reads, at its own position, every
// body value its exit's ops read, so those values stay live up to each jump.
class UsePositions {
public:
    explicit UsePositions(const IrBlock& block);

    [[nodiscard]] std::size_t value_count() const { return first_.size() - 1; }
    [[nodiscard]] std::uint32_t body_size() const { return exit_begin_.front(); }
    [[nodiscard]] std::uint32_t exit_begin(ExitId exit) const { return exit_begin_[exit]; }
    [[nodiscard]] std::uint32_t exit_end(ExitId exit) const { return exit_begin_[exit + 1]; }
    // The first read of `value` at or after `position`, or kNever.
    [[nodiscard]] std::uint32_t next(std::uint32_t value, std::uint32_t position) const;

private:
    std::vector<std::uint32_t> first_;       // per value: its first entry in uses_ (one extra at the end)
    std::vector<std::uint32_t> uses_;        // read positions, ascending per value
    std::vector<std::uint32_t> exit_begin_;  // per exit, then the end of the last one
};

struct Location {
    enum class Kind : std::uint8_t { None, Register, Slot };
    Kind kind = Kind::None;
    std::uint32_t index = 0;
    friend bool operator==(const Location&, const Location&) = default;
};

// A transfer the emitter performs: register -> slot (spill), slot -> register (reload) or
// register -> register.
struct Move {
    std::uint32_t value = 0;
    Location from, to;
    friend bool operator==(const Move&, const Move&) = default;
};

// Values are SSA: a spilled value keeps its slot until it dies, so evicting it again costs
// no store. A value is dead once it has no read before the region's end (the body's end, or
// the end of the exit being emitted). On eviction the value read farthest away leaves first.
class RegAlloc {
public:
    static constexpr unsigned kMaxRegisters = 16;
    static constexpr std::uint32_t kFree = ~0U;

    struct Snapshot {
        std::array<std::uint32_t, kMaxRegisters> registers{};  // value per register, or kFree
        std::vector<std::uint32_t> slots;                       // value per slot, or kFree
    };

    RegAlloc(unsigned registers, std::uint32_t volatile_mask, const UsePositions& uses);

    // The register holding `value`, reloading it if it sits in a slot. The register stays
    // locked (never evicted) until release_dead().
    [[nodiscard]] unsigned get(std::uint32_t value, std::uint32_t position, std::vector<Move>& moves);
    // A register for the result defined at `position`, locked like get()'s.
    [[nodiscard]] unsigned define(std::uint32_t value, std::uint32_t position, std::vector<Move>& moves);
    // Where `value` is now (its register if it has one), without moving it.
    [[nodiscard]] Location where(std::uint32_t value) const;
    // The end of the op at position - 1: frees the dead values and unlocks every register.
    void release_dead(std::uint32_t position);
    // Before a call at `position`: values in volatile registers that are read after the call
    // move to a free callee-saved register, or to a slot; the dead ones lose their register.
    void before_call(std::uint32_t position, std::vector<Move>& moves);
    void set_region_end(std::uint32_t end) { region_end_ = end; }

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot& snapshot);  // also unlocks every register

    [[nodiscard]] std::uint32_t used_registers() const { return used_; }  // every register ever assigned
    [[nodiscard]] std::uint32_t slot_count() const { return slot_count_; }  // slots ever needed

private:
    static constexpr std::uint8_t kNoRegister = 0xFF;

    [[nodiscard]] bool live(std::uint32_t value, std::uint32_t position) const {
        return uses_.next(value, position) < region_end_;
    }
    unsigned take_register(std::uint32_t position, std::vector<Move>& moves);
    std::uint32_t take_slot(std::uint32_t value);
    void free_slot(std::uint32_t value);

    const UsePositions& uses_;
    unsigned count_;
    std::uint32_t volatile_;
    std::uint32_t locked_ = 0;
    std::uint32_t used_ = 0;
    std::uint32_t slot_count_ = 0;
    std::uint32_t region_end_;
    std::array<std::uint32_t, kMaxRegisters> register_value_{};
    std::vector<std::uint32_t> slot_value_;
    std::vector<std::uint8_t> value_register_;
    std::vector<std::uint32_t> value_slot_;
};

}  // namespace ezy::jit
