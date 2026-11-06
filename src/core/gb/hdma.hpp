#pragma once

#include <cstdint>

namespace ezy::gb {

// CGB VRAM DMA registers HDMA1-HDMA5 (0xFF51-0xFF55). This class only holds the state;
// the bus does the copying, because it owns both ends and the clock.
//
// Two kinds of transfer, chosen by bit 7 of the HDMA5 write:
//   0 = general purpose (GDMA): everything at once, the CPU stops until it is done.
//   1 = HBlank (HDMA): one 16-byte block at the start of each HBlank (lines 0-143).
// Writing HDMA5 with bit 7 = 0 while an HDMA runs cancels it instead.
class Hdma {
public:
    // A completed 16-byte block. Copy from `source`, to `dest` in the current VRAM bank.
    struct Block {
        std::uint16_t source;
        std::uint16_t dest;
    };

    // HDMA1-4; they are write-only.
    void write_address(std::uint16_t addr, std::uint8_t value);
    // HDMA5 write. Returns the number of blocks to copy right now (a GDMA), or 0.
    [[nodiscard]] int write_control(std::uint8_t value);
    [[nodiscard]] std::uint8_t read_control() const;

    [[nodiscard]] bool hblank_active() const { return active_; }
    // Takes the next block and advances the addresses. Ends an HDMA after its last block.
    Block next_block();

    template <class Ar>
    void serialize(Ar& ar) {
        ar(source_);
        ar(dest_);
        ar(remaining_);
        ar(active_);
    }

private:
    std::uint16_t source_ = 0;  // bits 4-15
    std::uint16_t dest_ = 0;    // bits 4-12 (offset in VRAM)
    std::uint8_t remaining_ = 0;  // blocks left (an HDMA), 0 when idle
    bool active_ = false;         // an HDMA is in progress
};

}  // namespace ezy::gb
