#pragma once

#include <cstdint>

namespace ezy::gb {

// DIV / TIMA / TMA / TAC (0xFF04-0xFF07).
//
// Internally there is one 16-bit counter that advances every T-cycle; DIV is its upper
// byte. TIMA increments on the *falling edge* of one counter bit (selected by TAC) ANDed
// with the enable bit. Modelling the edge (instead of "every N cycles") is what makes
// odd cases right, e.g. writing DIV can bump TIMA.
//
// On overflow TIMA reads 0 for one M-cycle; on the next one it is loaded from TMA and the
// interrupt is requested. Writes in those two cycles behave specially (see write()).
class Timer {
public:
    void tick();  // one M-cycle

    [[nodiscard]] std::uint8_t read(std::uint16_t addr) const;
    void write(std::uint16_t addr, std::uint8_t value);

    // True once per overflow; the bus turns it into an interrupt request.
    bool take_interrupt();

    template <class Ar>
    void serialize(Ar& ar) {
        ar(counter_), ar(tima_), ar(tma_), ar(tac_), ar(interrupt_), ar(reload_delay_), ar(reloading_);
    }

    [[nodiscard]] std::uint16_t counter() const { return counter_; }

private:
    [[nodiscard]] bool input() const;  // selected counter bit AND timer enable
    void apply_edge(bool before, int reload_delay);
    void increment_tima(int reload_delay);

    std::uint16_t counter_ = 0xABCC;  // post-boot DIV = 0xAB
    std::uint8_t tima_ = 0;
    std::uint8_t tma_ = 0;
    std::uint8_t tac_ = 0;
    bool interrupt_ = false;
    // M-cycles until TIMA is loaded from TMA after an overflow (0 = none pending). An overflow
    // in tick() reloads on the next tick; one caused by a register write (which the bus does
    // after this M-cycle's tick) waits one tick more, so the CPU still sees 0 for a cycle.
    int reload_delay_ = 0;
    bool reloading_ = false;   // this M-cycle is the one that loaded TMA
};

}  // namespace ezy::gb
