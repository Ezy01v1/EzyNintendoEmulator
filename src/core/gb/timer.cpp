#include "gb/timer.hpp"

namespace ezy::gb {

namespace {

constexpr std::uint16_t kDiv = 0xFF04;
constexpr std::uint16_t kTima = 0xFF05;
constexpr std::uint16_t kTma = 0xFF06;
constexpr std::uint16_t kTac = 0xFF07;

// TAC clock select -> counter bit: 4096 Hz, 262144 Hz, 65536 Hz, 16384 Hz.
constexpr int kTacBit[4] = {9, 3, 5, 7};

}  // namespace

bool Timer::input() const {
    return (tac_ & 0x04) != 0 && ((counter_ >> kTacBit[tac_ & 0x03]) & 1) != 0;
}

void Timer::apply_edge(bool before, int reload_delay) {
    if (before && !input()) increment_tima(reload_delay);
}

void Timer::increment_tima(int reload_delay) {
    if (++tima_ == 0) reload_delay_ = reload_delay;  // reads 0 until the reload
}

void Timer::tick() {
    reloading_ = false;
    if (reload_delay_ > 0 && --reload_delay_ == 0) {
        tima_ = tma_;
        interrupt_ = true;
        reloading_ = true;
    }
    const bool before = input();
    counter_ = static_cast<std::uint16_t>(counter_ + 4);
    apply_edge(before, 1);
}

std::uint8_t Timer::read(std::uint16_t addr) const {
    switch (addr) {
        case kDiv: return static_cast<std::uint8_t>(counter_ >> 8);
        case kTima: return tima_;
        case kTma: return tma_;
        default: return static_cast<std::uint8_t>(tac_ | 0xF8);  // unused bits read as 1
    }
}

void Timer::write(std::uint16_t addr, std::uint8_t value) {
    const bool before = input();
    switch (addr) {
        case kDiv: counter_ = 0; break;  // any write resets the whole counter
        case kTima:
            // During the delay cycle a write cancels the reload; on the reload cycle itself
            // the write loses against TMA.
            if (!reloading_) {
                tima_ = value;
                reload_delay_ = 0;
            }
            break;
        case kTma:
            tma_ = value;
            if (reloading_) tima_ = value;  // the reload is still latching TMA
            break;
        default: tac_ = value & 0x07; break;
    }
    apply_edge(before, 1);
}

bool Timer::take_interrupt() {
    const bool pending = interrupt_;
    interrupt_ = false;
    return pending;
}

}  // namespace ezy::gb
