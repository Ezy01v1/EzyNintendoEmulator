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
// Simplification for now: on overflow TIMA is reloaded immediately (hardware waits 1 M-cycle).
class Timer {
public:
    void tick();  // one M-cycle

    [[nodiscard]] std::uint8_t read(std::uint16_t addr) const;
    void write(std::uint16_t addr, std::uint8_t value);

    // True once per overflow; the bus turns it into an interrupt request.
    bool take_interrupt();

    template <class Ar>
    void serialize(Ar& ar) {
        ar(counter_), ar(tima_), ar(tma_), ar(tac_), ar(interrupt_);
    }

    [[nodiscard]] std::uint16_t counter() const { return counter_; }

private:
    [[nodiscard]] bool input() const;  // selected counter bit AND timer enable
    void apply_edge(bool before);
    void increment_tima();

    std::uint16_t counter_ = 0xABCC;  // post-boot DIV = 0xAB
    std::uint8_t tima_ = 0;
    std::uint8_t tma_ = 0;
    std::uint8_t tac_ = 0;
    bool interrupt_ = false;
};

}  // namespace ezy::gb
