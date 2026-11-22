#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ezy::gc {

// Everything that can be scheduled, one pending event per type at most (the N64 scheduler's
// design, src/core/n64/scheduler.hpp: a small fixed array, rescheduling and cancelling are a
// single store). Each peripheral task adds its own types; the first two are the ones the
// design already names (a VI half-line, a DI transfer finishing after its fixed delay) and
// let the scheduler be tested now. Tasks 4 and 5 wire them (and may rename them).
enum class EventType : std::uint8_t {
    ViLine,      // VI reaches the next line (Task 4)
    DiTransfer,  // a DI command (a disc read and its DMA) finishes (Task 5)
    Count,
};

inline constexpr std::size_t kEventTypeCount = static_cast<std::size_t>(EventType::Count);

// Time is an absolute count of Gekko cycles (486 MHz) since the boot. 64 bits never overflow
// in practice (over 1000 years), so there is no wrap-around handling anywhere.
class Scheduler {
public:
    struct Due {
        EventType type;
        std::uint64_t when;  // the time it was scheduled for (may be before now())
    };

    [[nodiscard]] std::uint64_t now() const { return now_; }
    void advance(std::uint64_t cycles) { now_ += cycles; }
    void advance_to(std::uint64_t cycle) {
        if (cycle > now_) now_ = cycle;
    }

    // Replaces any pending event of the same type.
    void schedule(EventType type, std::uint64_t when) {
        slots_[index(type)] = {true, when};
        if (when < next_) next_ = when;
    }
    void schedule_in(EventType type, std::uint64_t cycles) { schedule(type, now_ + cycles); }
    void cancel(EventType type) { slots_[index(type)].active = false; }
    [[nodiscard]] bool pending(EventType type) const { return slots_[index(type)].active; }

    // Earliest pending event time, or nullopt if nothing is scheduled.
    [[nodiscard]] std::optional<std::uint64_t> next_event_time() const;

    // Removes and returns the earliest event with when <= now(). Ties go to the lower
    // EventType, so the order is deterministic. Handlers should reschedule relative to
    // Due::when (not now()) so lateness never accumulates. Called after every CPU instruction,
    // so the common "nothing due" case is a single compare.
    [[nodiscard]] std::optional<Due> pop_due() {
        if (now_ < next_) return std::nullopt;
        return pop_due_slow();
    }

private:
    struct Slot {
        bool active = false;
        std::uint64_t when = 0;
    };

    static constexpr std::size_t index(EventType type) { return static_cast<std::size_t>(type); }

    [[nodiscard]] std::optional<Due> pop_due_slow();

    std::uint64_t now_ = 0;
    std::array<Slot, kEventTypeCount> slots_{};
    // A lower bound on the earliest active event (never later than it). schedule() lowers it;
    // cancelling or rescheduling later may leave it early, which only costs one slow scan.
    std::uint64_t next_ = ~std::uint64_t{0};
};

}  // namespace ezy::gc
