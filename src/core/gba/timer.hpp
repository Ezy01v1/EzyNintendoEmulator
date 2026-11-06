#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gba/scheduler.hpp"

namespace ezy::gba {

// The four 16-bit timers (0x04000100-0x0400010F, GBATEK "Timers").
//
// A running timer is never ticked. It remembers when it started counting and from which
// value, and the counter is computed when read; an overflow is a scheduler event at the
// exact cycle it happens. Count-up (cascade) timers have no event: they advance by one each
// time the previous timer overflows.
//
// Like the PPU, the timers work on a view of the bus's I/O storage: TMxCNT_L there holds
// the last value written (the reload), and the bus asks counter() when the CPU reads it.
//
// Simplifications: the counter starts on the cycle the enable bit is written (hardware:
// 2 cycles later), and prescaled timers count from their own start, not from the global
// prescaler phase.
// Told about every overflow (the sound FIFOs are clocked by timers 0 and 1).
class TimerOverflowHook {
public:
    virtual void on_timer_overflow(int index) = 0;

protected:
    ~TimerOverflowHook() = default;
};

class Timers {
public:
    Timers(std::span<std::uint8_t> io, Scheduler* scheduler) : io_(io), scheduler_(scheduler) {}

    void set_overflow_hook(TimerOverflowHook* hook) { hook_ = hook; }

    // The bus calls this after any I/O write touching 0x100-0x10F.
    void after_io_write(std::size_t offset, std::size_t size);
    void on_overflow(int index, std::uint64_t when);

    // The counter as the CPU reads it from TMxCNT_L.
    [[nodiscard]] std::uint16_t counter(int index) const;

    // Save-state contents (state.hpp). The ROM, BIOS and wiring are not part of it.
    template <class Ar>
    void serialize(Ar& ar) {
        ar(timers_);
    }

private:
    struct Timer {
        std::uint16_t reload = 0;
        std::uint16_t control = 0;
        std::uint32_t counter = 0;     // value at `start` (running) or current (stopped/cascade)
        std::uint64_t start = 0;       // cycle the counter was last (re)based
        [[nodiscard]] bool enabled() const { return (control & 0x80) != 0; }
        [[nodiscard]] bool cascade() const { return (control & 0x04) != 0; }
        [[nodiscard]] int shift() const;  // prescaler: 1, 64, 256, 1024 cycles per tick
    };

    [[nodiscard]] bool runs_on_clock(int index) const;  // enabled and counting cycles
    [[nodiscard]] std::uint32_t live_counter(int index, std::uint64_t now) const;
    void rebase(int index);           // freeze the live value into counter/start
    void schedule_overflow(int index);
    void overflow(int index, std::uint64_t when);
    void request_irq(std::uint16_t bits);

    std::span<std::uint8_t> io_;
    Scheduler* scheduler_;
    std::array<Timer, 4> timers_{};
    TimerOverflowHook* hook_ = nullptr;
};

}  // namespace ezy::gba
