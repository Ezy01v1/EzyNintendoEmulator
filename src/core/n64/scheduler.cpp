#include "n64/scheduler.hpp"

namespace ezy::n64 {

std::optional<std::uint64_t> Scheduler::next_event_time() const {
    std::optional<std::uint64_t> earliest;
    for (const Slot& slot : slots_) {
        if (slot.active && (!earliest || slot.when < *earliest)) earliest = slot.when;
    }
    return earliest;
}

std::optional<Scheduler::Due> Scheduler::pop_due() {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (!slot.active || slot.when > now_) continue;
        if (!best || slot.when < slots_[*best].when) best = i;  // strict <: ties keep the lower type
    }
    if (!best) return std::nullopt;
    slots_[*best].active = false;
    return Due{static_cast<EventType>(*best), slots_[*best].when};
}

}  // namespace ezy::n64
