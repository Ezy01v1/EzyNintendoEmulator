#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ezy::n64 {

// Everything that can be scheduled. One pending event per type at most, which keeps the
// scheduler a small fixed array (cheap, cache friendly, trivially serialisable for save
// states) and makes rescheduling/cancelling a single store.
enum class EventType : std::uint8_t {
    ViLine,  // VI reaches the next half-line
    PiDma,   // a PI DMA finishes
    SiDma,   // an SI DMA finishes
    AiDma,   // the playing AI buffer finishes
    Count,
};

inline constexpr std::size_t kEventTypeCount = static_cast<std::size_t>(EventType::Count);

// Time is an absolute count of CPU cycles (93.75 MHz) since power-on. 64 bits never
// overflow in practice, so there is no wrap-around handling anywhere.
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
    void schedule(EventType type, std::uint64_t when) { slots_[index(type)] = {true, when}; }
    void schedule_in(EventType type, std::uint64_t cycles) { schedule(type, now_ + cycles); }
    void cancel(EventType type) { slots_[index(type)].active = false; }
    [[nodiscard]] bool pending(EventType type) const { return slots_[index(type)].active; }

    // Earliest pending event time, or nullopt if nothing is scheduled.
    [[nodiscard]] std::optional<std::uint64_t> next_event_time() const;

    // Removes and returns the earliest event with when <= now(). Ties go to the lower
    // EventType, so the order is deterministic. Handlers should reschedule relative to
    // Due::when (not now()) so lateness never accumulates.
    [[nodiscard]] std::optional<Due> pop_due();

    // Save-state contents (state.hpp). The ROM, BIOS and wiring are not part of it.
    template <class Ar>
    void serialize(Ar& ar) {
        ar(now_);
        ar(slots_);
    }

private:
    struct Slot {
        bool active = false;
        std::uint64_t when = 0;
        template <class Ar>
        void serialize(Ar& ar) { ar(active); ar(when); }
    };

    static constexpr std::size_t index(EventType type) { return static_cast<std::size_t>(type); }

    std::uint64_t now_ = 0;
    std::array<Slot, kEventTypeCount> slots_{};
};

}  // namespace ezy::n64
