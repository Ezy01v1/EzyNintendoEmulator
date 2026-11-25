#pragma once

#include <array>
#include <cstdint>

#include "gc/mmio.hpp"

namespace ezy::gc {

class Pi;
class Scheduler;

// The controller 1 state the frontend sets (GameCube::set_pad); the SI's channel 0 reports
// it. Buttons use the GameCube bit layout of the SI poll's first word: Start 0x1000, Y 0x0800,
// X 0x0400, B 0x0200, A 0x0100, L 0x0040, R 0x0020, Z 0x0010, D-pad up 0x0008, down 0x0004,
// right 0x0002, left 0x0001. Sticks and triggers are the raw 8-bit values (centre 128,
// triggers 0..255).
struct PadState {
    std::uint16_t buttons = 0;
    std::uint8_t stick_x = 128, stick_y = 128;
    std::uint8_t c_x = 128, c_y = 128;
    std::uint8_t trigger_l = 0, trigger_r = 0;
};

// The serial interface at 0x0C006400 (design, "Periféricos": SI). Offsets and bit layouts from
// Dolphin's Core/HW/SI/SI.h/.cpp and SI_DeviceGCController.cpp (master, read on 2026-11-23),
// and YAGCD's "Serial Interface". The registers are 32 bits (a 16-bit access throws), except
// the communication buffer, which also takes 16-bit accesses (as Dolphin registers it):
//   0x00 + 0x0C n  SICnOUTBUF: CMD bits 16-23, OUTPUT0 8-15, OUTPUT1 0-7 (stored). With CMD
//                  0x40 (the poll command) OUTPUT0 is the analog mode, which must be 3 (the
//                  SDK's default; the others throw when written) and OUTPUT1 the motor.
//   0x04 + 0x0C n  SICnINBUFH: the last poll's first word (ERRSTAT 31, ERRLATCH 30, then the
//                  data); reading it clears that channel's RDST.
//   0x08 + 0x0C n  SICnINBUFL: the second word; reading it clears RDST too (as Dolphin).
//   0x30 SIPOLL    VBCPY3-0 bits 0-3, EN3-0 bits 4-7 (EN0 = bit 7), Y 8-15, X 16-25 (stored).
//   0x34 SICOMCSR  TSTART 0, CHANNEL 1-2, INLNGTH 8-14, OUTLNGTH 16-22 (0 means 128 bytes),
//                  RDSTINTMSK 27, RDSTINT 28, COMERR 29, TCINTMSK 30, TCINT 31. RDSTINT reads
//                  1 while any channel's RDST is set; TCINT clears when written 1.
//   0x38 SISR      per channel (channel 0 in bits 24-29, 1 in 16-21, 2 in 8-13, 3 in 0-5):
//                  UNRUN, OVRUN, COLL, NOREP (write 1 clears), WRST, RDST (read-only); WR
//                  bit 31: writing it "copies" the OUT buffers, done at once (WR and WRST
//                  read 0).
//   0x3C SIEXILK   the EXI 32 MHz clock lock: stored, no effect.
//   0x80-0xFF      the communication buffer (128 bytes).
// The SI cause to the PI: (RDSTINT & RDSTINTMSK) | (TCINT & TCINTMSK).
//
// Transfers (TSTART in SICOMCSR) run at once: OUTLNGTH bytes of the buffer go to the
// channel's device, its answer (up to INLNGTH bytes) comes back into the buffer from 0x80,
// then TSTART clears and TCINT rises. Channel 0 is a standard controller (Dolphin's
// CSIDevice_GCController): 0x00 (ID) and 0xFF (reset) answer 0x09 0x00 0x00 (SI_GC_CONTROLLER),
// 0x40 (direct status, mode in the next byte, 3 only) the 8 bytes of a poll, 0x41 (origin)
// and 0x42 (recalibrate) the 10-byte origin: buttons 0, sticks centred (0x80), triggers 0,
// two zero bytes. Any other command throws. Channels 1-3 have no device: the transfer sets
// COMERR and that channel's NOREP (Dolphin's null device answers -1).
//
// Polling: while any EN bit is set and X > 0, every X video lines (X * 30,888 CPU cycles, the
// VI's line, Vi::kCyclesPerHalfLine * 2) each enabled channel is polled. Channel 0 latches
// IN_HI = stick Y | stick X << 8 | (buttons | 0x0080 "use origin") << 16 and, in mode 3,
// IN_LO = trigger R | trigger L << 8 | C-stick Y << 16 | C-stick X << 24, and sets RDST0.
// A channel with no device gets ERRSTAT | ERRLATCH in IN_HI and NOREP, no RDST: Dolphin's
// CSIDevice_Null::GetData (SI_DeviceNull.cpp, master) returns ErrorNoResponse, which SI.cpp's
// UpdateDevices turns into SetNoResponse + ERRSTAT/ERRLATCH. (Older Dolphin versions set
// hi = 0x80000000 and reported data; master no longer does.) Simplification: the polls run free every X lines from the SIPOLL
// write instead of restarting at each field's vsync, and Y (polls per field) is not used to
// stop them (Dolphin also polls every X lines; YAGCD: "Polls per X lines, begins at vsync").
class Si final : public MmioHandler {
public:
    Si(Scheduler& scheduler, Pi& pi);

    void set_pad(const PadState& pad) { pad_ = pad; }
    [[nodiscard]] const PadState& pad() const { return pad_; }

    // Handles EventType::SiPoll that was due at `when`: polls and reschedules from `when`.
    void on_event(std::uint64_t when);

    std::uint16_t read16(std::uint32_t offset) override;
    std::uint32_t read32(std::uint32_t offset) override;
    void write16(std::uint32_t offset, std::uint16_t value) override;
    void write32(std::uint32_t offset, std::uint32_t value) override;

private:
    static constexpr std::uint32_t kBufferBase = 0x80, kBufferSize = 0x80;
    struct Channel {
        std::uint32_t out = 0, in_hi = 0, in_lo = 0;
    };

    void transfer();
    void poll();
    void schedule_poll();
    void update_irq();
    [[nodiscard]] std::uint32_t pad_hi() const;
    [[nodiscard]] std::uint32_t pad_lo() const;
    [[nodiscard]] std::uint32_t buffer32(std::uint32_t at) const;

    Scheduler& sched_;
    Pi& pi_;
    PadState pad_;
    std::array<Channel, 4> channels_{};
    std::uint32_t poll_ = 492u << 16;  // Dolphin's Init(): X = 492, nothing enabled
    std::uint32_t com_csr_ = 0, status_ = 0, exi_clock_lock_ = 0;
    std::uint64_t poll_period_ = 0;    // the period of the scheduled poll (0: none)
    std::array<std::uint8_t, kBufferSize> buffer_{};
};

}  // namespace ezy::gc
