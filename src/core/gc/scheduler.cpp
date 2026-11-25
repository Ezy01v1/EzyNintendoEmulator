#include "gc/scheduler.hpp"

namespace ezy::gc {

std::optional<std::uint64_t> Scheduler::next_event_time() const {
    std::optional<std::uint64_t> earliest;
    for (const Slot& slot : slots_) {
        if (slot.active && (!earliest || slot.when < *earliest)) earliest = slot.when;
    }
    return earliest;
}

std::optional<Scheduler::Due> Scheduler::pop_due_slow() {
    // One pass finds the earliest event (ties keep the lower type) and the second earliest,
    // which becomes the exact bound once the first is popped.
    std::optional<std::size_t> best;
    std::uint64_t second = ~std::uint64_t{0};
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].active) continue;
        if (!best || slots_[i].when < slots_[*best].when) {
            if (best && slots_[*best].when < second) second = slots_[*best].when;
            best = i;
        } else if (slots_[i].when < second) {
            second = slots_[i].when;
        }
    }
    if (!best || slots_[*best].when > now_) {
        next_ = best ? slots_[*best].when : ~std::uint64_t{0};
        return std::nullopt;
    }
    slots_[*best].active = false;
    next_ = second;
    return Due{static_cast<EventType>(*best), slots_[*best].when};
}

}  // namespace ezy::gc
