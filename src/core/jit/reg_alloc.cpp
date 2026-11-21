#include "jit/reg_alloc.hpp"

#include <algorithm>
#include <stdexcept>

namespace ezy::jit {

namespace {

// The exit a jump op leaves through, or kNever.
std::uint32_t jump_target(const Inst& inst) {
    switch (inst.op) {
        case Op::ExitIf: case Op::Exit: return static_cast<std::uint32_t>(inst.imm);
        case Op::Barrier: return static_cast<std::uint32_t>(inst.imm >> Inst::kBarrierExitShift);
        default: return kNever;
    }
}

template <typename Visit>
void operands(const Inst& inst, Visit&& visit) {
    if (inst.a != Inst::kNone) visit(inst.a);
    if (inst.b != Inst::kNone) visit(inst.b);
    if (inst.c != Inst::kNone) visit(inst.c);
}

}  // namespace

UsePositions::UsePositions(const IrBlock& block) {
    const auto values = block.value_count();
    const auto& body = block.body();
    const auto exits = block.exit_count();
    exit_begin_.resize(exits + 1);
    auto position = static_cast<std::uint32_t>(body.size());
    for (ExitId exit = 0; exit < exits; ++exit) {
        exit_begin_[exit] = position;
        position += static_cast<std::uint32_t>(block.exit_ops(exit).size());
    }
    exit_begin_[exits] = position;

    // The body values each exit reads, once each: a value an exit defines is marked with
    // that exit before any read of it.
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> reads_begin(exits + 1);
    std::vector<std::uint32_t> mark(values, kNever);
    for (ExitId exit = 0; exit < exits; ++exit) {
        reads_begin[exit] = static_cast<std::uint32_t>(reads.size());
        for (const auto& inst : block.exit_ops(exit)) {
            operands(inst, [&](std::uint32_t value) {
                if (mark[value] == exit) return;
                mark[value] = exit;
                reads.push_back(value);
            });
            if (inst.result != Inst::kNone) mark[inst.result] = exit;
        }
    }
    reads_begin[exits] = static_cast<std::uint32_t>(reads.size());

    // Every read in position order, twice: to count them, then to fill the table.
    const auto each_read = [&](auto&& visit) {
        for (std::uint32_t p = 0; p < body.size(); ++p) {
            operands(body[p], [&](std::uint32_t value) { visit(value, p); });
            const auto exit = jump_target(body[p]);
            if (exit == kNever) continue;
            for (auto r = reads_begin[exit]; r < reads_begin[exit + 1]; ++r) visit(reads[r], p);
        }
        for (ExitId exit = 0; exit < exits; ++exit) {
            auto p = exit_begin_[exit];
            for (const auto& inst : block.exit_ops(exit)) {
                operands(inst, [&](std::uint32_t value) { visit(value, p); });
                ++p;
            }
        }
    };
    first_.assign(values + 1, 0);
    each_read([&](std::uint32_t value, std::uint32_t) { ++first_[value + 1]; });
    for (std::size_t v = 0; v < values; ++v) first_[v + 1] += first_[v];
    uses_.resize(first_[values]);
    std::vector<std::uint32_t> cursor(first_.begin(), first_.end() - 1);
    each_read([&](std::uint32_t value, std::uint32_t p) { uses_[cursor[value]++] = p; });
}

std::uint32_t UsePositions::next(std::uint32_t value, std::uint32_t position) const {
    const auto begin = uses_.begin() + first_[value];
    const auto end = uses_.begin() + first_[value + 1];
    const auto it = std::lower_bound(begin, end, position);
    return it == end ? kNever : *it;
}

RegAlloc::RegAlloc(unsigned registers, std::uint32_t volatile_mask, const UsePositions& uses)
    : uses_(uses), count_(registers), volatile_(volatile_mask), region_end_(uses.body_size()) {
    if (registers == 0 || registers > kMaxRegisters) throw std::logic_error("jit: bad register count");
    register_value_.fill(kFree);
    value_register_.assign(uses.value_count(), kNoRegister);
    value_slot_.assign(uses.value_count(), kFree);
}

unsigned RegAlloc::get(std::uint32_t value, std::uint32_t position, std::vector<Move>& moves) {
    if (value_register_[value] != kNoRegister) {
        const unsigned r = value_register_[value];
        locked_ |= 1U << r;
        return r;
    }
    const auto slot = value_slot_[value];
    if (slot == kFree) throw std::logic_error("jit: a value read where it does not exist");
    const unsigned r = take_register(position, moves);
    register_value_[r] = value;
    value_register_[value] = static_cast<std::uint8_t>(r);
    locked_ |= 1U << r;
    moves.push_back({value, {Location::Kind::Slot, slot}, {Location::Kind::Register, r}});
    return r;
}

unsigned RegAlloc::define(std::uint32_t value, std::uint32_t position, std::vector<Move>& moves) {
    const unsigned r = take_register(position, moves);
    register_value_[r] = value;
    value_register_[value] = static_cast<std::uint8_t>(r);
    locked_ |= 1U << r;
    return r;
}

Location RegAlloc::where(std::uint32_t value) const {
    if (value_register_[value] != kNoRegister) return {Location::Kind::Register, value_register_[value]};
    if (value_slot_[value] != kFree) return {Location::Kind::Slot, value_slot_[value]};
    return {};
}

unsigned RegAlloc::take_register(std::uint32_t position, std::vector<Move>& moves) {
    for (unsigned r = 0; r < count_; ++r) {
        if (register_value_[r] == kFree && (locked_ & (1U << r)) == 0) {
            used_ |= 1U << r;
            return r;
        }
    }
    unsigned victim = kMaxRegisters;
    std::uint32_t farthest = 0;
    for (unsigned r = 0; r < count_; ++r) {
        if ((locked_ & (1U << r)) != 0) continue;
        const auto next = uses_.next(register_value_[r], position);
        if (victim == kMaxRegisters || next > farthest) {
            victim = r;
            farthest = next;
        }
    }
    if (victim == kMaxRegisters) throw std::logic_error("jit: every register is locked");
    const auto value = register_value_[victim];
    if (farthest >= region_end_) {
        free_slot(value);  // dead already: nothing to keep
    } else if (value_slot_[value] == kFree) {
        const auto slot = take_slot(value);
        moves.push_back({value, {Location::Kind::Register, victim}, {Location::Kind::Slot, slot}});
    }
    register_value_[victim] = kFree;
    value_register_[value] = kNoRegister;
    return victim;
}

std::uint32_t RegAlloc::take_slot(std::uint32_t value) {
    auto slot = static_cast<std::uint32_t>(
        std::find(slot_value_.begin(), slot_value_.end(), kFree) - slot_value_.begin());
    if (slot == slot_value_.size()) slot_value_.push_back(kFree);
    slot_value_[slot] = value;
    value_slot_[value] = slot;
    slot_count_ = std::max(slot_count_, static_cast<std::uint32_t>(slot_value_.size()));
    return slot;
}

void RegAlloc::free_slot(std::uint32_t value) {
    const auto slot = value_slot_[value];
    if (slot == kFree) return;
    slot_value_[slot] = kFree;
    value_slot_[value] = kFree;
}

void RegAlloc::release_dead(std::uint32_t position) {
    for (unsigned r = 0; r < count_; ++r) {
        const auto value = register_value_[r];
        if (value == kFree || live(value, position)) continue;
        register_value_[r] = kFree;
        value_register_[value] = kNoRegister;
        free_slot(value);
    }
    for (const auto value : slot_value_) {
        if (value != kFree && !live(value, position)) free_slot(value);
    }
    locked_ = 0;
}

void RegAlloc::before_call(std::uint32_t position, std::vector<Move>& moves) {
    for (unsigned r = 0; r < count_; ++r) {
        const auto value = register_value_[r];
        if ((volatile_ & (1U << r)) == 0 || value == kFree) continue;
        register_value_[r] = kFree;
        value_register_[value] = kNoRegister;
        locked_ &= ~(1U << r);
        if (!live(value, position + 1)) continue;  // only the call reads it, from where it was
        unsigned safe = kMaxRegisters;
        for (unsigned c = 0; c < count_ && safe == kMaxRegisters; ++c) {
            const auto bit = 1U << c;
            if ((volatile_ & bit) == 0 && (locked_ & bit) == 0 && register_value_[c] == kFree) safe = c;
        }
        if (safe != kMaxRegisters) {
            register_value_[safe] = value;
            value_register_[value] = static_cast<std::uint8_t>(safe);
            used_ |= 1U << safe;
            moves.push_back({value, {Location::Kind::Register, r}, {Location::Kind::Register, safe}});
        } else if (value_slot_[value] == kFree) {
            const auto slot = take_slot(value);
            moves.push_back({value, {Location::Kind::Register, r}, {Location::Kind::Slot, slot}});
        }
    }
}

RegAlloc::Snapshot RegAlloc::snapshot() const { return {register_value_, slot_value_}; }

void RegAlloc::restore(const Snapshot& snapshot) {
    for (unsigned r = 0; r < count_; ++r) {
        if (register_value_[r] != kFree) value_register_[register_value_[r]] = kNoRegister;
    }
    for (const auto value : slot_value_) {
        if (value != kFree) value_slot_[value] = kFree;
    }
    register_value_ = snapshot.registers;
    slot_value_ = snapshot.slots;
    for (unsigned r = 0; r < count_; ++r) {
        if (register_value_[r] != kFree) value_register_[register_value_[r]] = static_cast<std::uint8_t>(r);
    }
    for (std::uint32_t s = 0; s < slot_value_.size(); ++s) {
        if (slot_value_[s] != kFree) value_slot_[slot_value_[s]] = s;
    }
    locked_ = 0;
}

}  // namespace ezy::jit
