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
    SpDma,   // an SP DMA row finishes
    RspStep, // the RSP executes one instruction
    RdpWord, // the RDP consumes a word from its command stream
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
    // Due::when (not now()) so lateness never accumulates.
    // Called after every CPU instruction, so the common "nothing due" case is a single compare.
    [[nodiscard]] std::optional<Due> pop_due() {
        if (now_ < next_) return std::nullopt;
        return pop_due_slow();
    }

    // Save-state contents (state.hpp). The ROM, BIOS and wiring are not part of it.
    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        ar(now_);
        // Version 1 predates SP execution; versions 2 and 3 predate DP execution.
        const std::size_t count = version == 1 ? index(EventType::SpDma)
                                  : version < 4 ? index(EventType::RdpWord) : slots_.size();
        for (std::size_t i = 0; i < count; ++i) ar(slots_[i]);
        for (std::size_t i = count; i < slots_.size(); ++i) slots_[i] = {};
        next_ = 0;  // recomputed by the next pop_due
    }

private:
    struct Slot {
        bool active = false;
        std::uint64_t when = 0;
        template <class Ar>
        void serialize(Ar& ar) { ar(active); ar(when); }
    };

    static constexpr std::size_t index(EventType type) { return static_cast<std::size_t>(type); }

    [[nodiscard]] std::optional<Due> pop_due_slow();

    std::uint64_t now_ = 0;
    std::array<Slot, kEventTypeCount> slots_{};
    // A lower bound on the earliest active event (never later than it). schedule() lowers it;
    // cancelling or rescheduling later may leave it early, which only costs one slow scan.
    std::uint64_t next_ = 0;
};

}  // namespace ezy::n64
