#pragma once

// Drives a Scheduler without a CPU: jumps from event to event up to `cycle`, handing every
// due event to `deliver` (the test routes it to the peripheral that owns its type), then
// leaves the time at `cycle`.

#include <cstdint>

#include "gc/scheduler.hpp"

template <typename Deliver>
void run_events_to(ezy::gc::Scheduler& sched, std::uint64_t cycle, Deliver&& deliver) {
    for (;;) {
        const auto next = sched.next_event_time();
        if (!next || *next > cycle) break;
        sched.advance_to(*next);
        while (const auto due = sched.pop_due()) deliver(*due);
    }
    sched.advance_to(cycle);
}
