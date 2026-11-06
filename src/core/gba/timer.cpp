#include "gba/timer.hpp"

namespace ezy::gba {

namespace {

constexpr std::uint32_t kTimerBase = 0x100;
constexpr std::uint32_t kIf = 0x202;
constexpr std::array<int, 4> kPrescalerShift = {0, 6, 8, 10};
constexpr std::array<EventType, 4> kEvents = {EventType::Timer0, EventType::Timer1, EventType::Timer2,
                                              EventType::Timer3};

constexpr bool overlaps(std::size_t offset, std::size_t size, std::size_t first, std::size_t last) {
    return offset <= last && offset + size > first;
}

}  // namespace

int Timers::Timer::shift() const { return kPrescalerShift[control & 3]; }

bool Timers::runs_on_clock(int index) const {
    const Timer& t = timers_[static_cast<std::size_t>(index)];
    // Timer 0 has no previous timer, so its count-up bit is ignored.
    return t.enabled() && (index == 0 || !t.cascade());
}

std::uint32_t Timers::live_counter(int index, std::uint64_t now) const {
    const Timer& t = timers_[static_cast<std::size_t>(index)];
    if (!runs_on_clock(index) || scheduler_ == nullptr) return t.counter;
    const std::uint64_t value = t.counter + ((now - t.start) >> t.shift());
    if (value <= 0xFFFF) return static_cast<std::uint32_t>(value);
    // The overflow event is due but not dispatched yet: wrap the way the hardware would.
    const std::uint64_t period = 0x10000 - t.reload;
    return t.reload + static_cast<std::uint32_t>((value - 0x10000) % period);
}

std::uint16_t Timers::counter(int index) const {
    const std::uint64_t now = scheduler_ != nullptr ? scheduler_->now() : 0;
    return static_cast<std::uint16_t>(live_counter(index, now));
}

void Timers::rebase(int index) {
    Timer& t = timers_[static_cast<std::size_t>(index)];
    if (scheduler_ == nullptr) return;
    t.counter = live_counter(index, scheduler_->now());
    t.start = scheduler_->now();
}

void Timers::schedule_overflow(int index) {
    if (scheduler_ == nullptr) return;
    const EventType event = kEvents[static_cast<std::size_t>(index)];
    if (!runs_on_clock(index)) {
        scheduler_->cancel(event);
        return;
    }
    const Timer& t = timers_[static_cast<std::size_t>(index)];
    // Ticks left until 0xFFFF -> 0x10000, converted to cycles from the current base.
    const std::uint64_t ticks_left = 0x10000 - t.counter;
    scheduler_->schedule(event, t.start + (ticks_left << t.shift()));
}

void Timers::after_io_write(std::size_t offset, std::size_t size) {
    for (int i = 0; i < 4; ++i) {
        const std::size_t base = kTimerBase + static_cast<std::size_t>(i) * 4;
        Timer& t = timers_[static_cast<std::size_t>(i)];
        if (overlaps(offset, size, base, base + 1)) {
            // Writing TMxCNT_L sets the reload value; the counter itself is unaffected.
            t.reload = static_cast<std::uint16_t>(io_[base] | (io_[base + 1] << 8));
        }
        if (!overlaps(offset, size, base + 2, base + 3)) continue;

        const auto control = static_cast<std::uint16_t>(io_[base + 2] | (io_[base + 3] << 8));
        if (control == t.control) continue;
        rebase(i);  // keep the value counted so far under the old settings
        const bool was_enabled = t.enabled();
        t.control = control;
        if (!was_enabled && t.enabled()) {
            t.counter = t.reload;  // enabling (0 -> 1) reloads the counter
            if (scheduler_ != nullptr) t.start = scheduler_->now();
        }
        schedule_overflow(i);
    }
}

void Timers::on_overflow(int index, std::uint64_t when) { overflow(index, when); }

void Timers::overflow(int index, std::uint64_t when) {
    Timer& t = timers_[static_cast<std::size_t>(index)];
    t.counter = t.reload;
    t.start = when;  // from the cycle it was due, so lateness doesn't accumulate
    if ((t.control & 0x40) != 0) request_irq(static_cast<std::uint16_t>(1U << (3 + index)));
    if (runs_on_clock(index)) schedule_overflow(index);
    if (hook_ != nullptr) hook_->on_timer_overflow(index);

    // Count-up: the next timer advances by one per overflow of this one.
    if (index < 3) {
        Timer& next = timers_[static_cast<std::size_t>(index) + 1];
        if (next.enabled() && next.cascade() && ++next.counter > 0xFFFF) overflow(index + 1, when);
    }
}

void Timers::request_irq(std::uint16_t bits) {
    io_[kIf] = static_cast<std::uint8_t>(io_[kIf] | bits);
    io_[kIf + 1] = static_cast<std::uint8_t>(io_[kIf + 1] | (bits >> 8));
}

}  // namespace ezy::gba
