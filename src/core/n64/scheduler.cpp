#include "n64/scheduler.hpp"

#include <algorithm>

namespace ezy::n64 {

std::optional<std::uint64_t> Scheduler::next_event_time() const {
    std::optional<std::uint64_t> earliest;
    for (const Slot& slot : slots_) {
        if (slot.active && (!earliest || slot.when < *earliest)) earliest = slot.when;
    }
    return earliest;
}

std::optional<Scheduler::Due> Scheduler::pop_due_slow() {
    // One pass finds the earliest event (ties keep the lower type) and the earliest of the
    // rest, which becomes the exact bound once the first is popped.
    const auto due_time = [this](std::size_t i) { return slots_[i].when; };
    std::optional<std::size_t> best;
    std::uint64_t second = ~std::uint64_t{0};
    std::uint64_t others = ~std::uint64_t{0};
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].active) continue;
        if (i != index(EventType::RspStep)) others = std::min(others, due_time(i));
        if (!best || due_time(i) < due_time(*best)) {
            if (best) second = std::min(second, due_time(*best));
            best = i;
        } else {
            second = std::min(second, due_time(i));
        }
    }
    if (!best || due_time(*best) > now_) {
        next_ = best ? due_time(*best) : ~std::uint64_t{0};
        others_next_ = others;
        return std::nullopt;
    }
    slots_[*best].active = false;
    next_ = second;
    // Without the popped event, the others' earliest is the second-earliest overall unless
    // the RSP is that one; the scan's value is then still a valid (early) bound.
    others_next_ = *best == index(EventType::RspStep) ? others : std::max(others, second);
    return Due{static_cast<EventType>(*best), slots_[*best].when};
}

}  // namespace ezy::n64
