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
        if (type != EventType::RspStep && when < others_next_) others_next_ = when;
    }

    // Fast path for the RSP, which reschedules itself every 1-2 cycles: if its step is due
    // and strictly before every other event (so the order is the same as pop_due's), takes
    // it and returns true with its time. Ties go through pop_due.
    bool take_rsp_step(std::uint64_t& when) {
        Slot& rsp = slots_[index(EventType::RspStep)];
        if (!rsp.active || rsp.when > now_ || rsp.when >= others_next_) return false;
        rsp.active = false;
        when = rsp.when;
        return true;
    }
    // After a run of take_rsp_step: the exact bound again, so pop_due stays a single compare.
    void settle_rsp() {
        const Slot& rsp = slots_[index(EventType::RspStep)];
        next_ = rsp.active && rsp.when < others_next_ ? rsp.when : others_next_;
    }
    void schedule_in(EventType type, std::uint64_t cycles) { schedule(type, now_ + cycles); }
    void cancel(EventType type) { slots_[index(type)].active = false; }
    [[nodiscard]] bool pending(EventType type) const { return slots_[index(type)].active; }

    // Earliest time among the pending events whose type is not in `mask` (bit per type).
    [[nodiscard]] std::uint64_t earliest_except(std::uint32_t mask) const {
        std::uint64_t earliest = ~std::uint64_t{0};
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].active && !(mask & (1U << i)) && slots_[i].when < earliest) earliest = slots_[i].when;
        }
        return earliest;
    }
    // Moves time back, for the idle loop when an event it ran early raised an interrupt.
    // Only the handlers that ignore now() may have run past the new time.
    void rewind_to(std::uint64_t cycle) { now_ = cycle; }
    [[nodiscard]] static constexpr std::uint32_t bit(EventType type) { return 1U << index(type); }

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
        others_next_ = 0;
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
    // The same kind of lower bound, for every event except RspStep.
    std::uint64_t others_next_ = 0;
};

}  // namespace ezy::n64
