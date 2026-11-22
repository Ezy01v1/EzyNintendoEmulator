#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/mi.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "gc/vi.hpp"

namespace ezy::gc {

class Disc;

// The controller 1 state the frontend sets (the SI poll, Task 5, reads it back). Buttons use
// the GameCube bit layout of the SI poll's first word: Start 0x1000, Y 0x0800, X 0x0400,
// B 0x0200, A 0x0100, L 0x0040, R 0x0020, Z 0x0010, D-pad up 0x0008, down 0x0004, right
// 0x0002, left 0x0001. Sticks and triggers are the raw 8-bit values (centre 128, triggers 0..255).
struct PadState {
    std::uint16_t buttons = 0;
    std::uint8_t stick_x = 128, stick_y = 128;
    std::uint8_t c_x = 128, c_y = 128;
    std::uint8_t trigger_l = 0, trigger_r = 0;
};

// What the runner prints. The counters the later peripherals feed (DI reads, FIFO bytes, DSP
// mails) are plain fields they bump through GameCube::stats_mut().
struct Stats {
    std::uint64_t instructions = 0;
    std::array<std::uint64_t, Pi::kCauseCount> interrupts{};  // rising edges per PI cause bit
    std::uint64_t di_reads = 0;
    std::uint64_t fifo_bytes_this_field = 0;  // reset at each field start
    std::uint64_t dsp_mails = 0;
    std::uint64_t fields = 0;
};

// The machine (design, "Runner y ezyemu"): the Gekko, MEM1, the scheduler and the
// peripherals, wired as the hardware is.
class GameCube {
public:
    // Opens the disc, builds the machine and HLE-boots it. nullptr (reason on stderr) when
    // the disc cannot be opened; the HLE boot's std::runtime_error propagates.
    static std::unique_ptr<GameCube> create(const std::filesystem::path& disc);
    ~GameCube();
    GameCube(const GameCube&) = delete;
    GameCube& operator=(const GameCube&) = delete;

    // Runs until the VI starts the next field. Throws GekkoError/BusError as the CPU does;
    // the machine stays inspectable (cpu().pc() is the instruction that failed).
    void run_field();

    // The picture the VI is showing: ARGB, 640 wide, the frame's height (see Vi::xfb_image).
    [[nodiscard]] Image xfb() const;
    [[nodiscard]] Stats stats() const;
    Stats& stats_mut() { return stats_; }
    void set_pad(const PadState& pad) { pad_ = pad; }
    [[nodiscard]] const PadState& pad() const { return pad_; }

    // Gekko cycles since the boot.
    [[nodiscard]] std::uint64_t cycles() const { return scheduler_.now(); }

    // Parts, for tests, the runner and the later tasks' peripherals.
    Memory& memory() { return memory_; }
    Gekko& cpu() { return cpu_; }
    Scheduler& scheduler() { return scheduler_; }
    Pi& pi() { return pi_; }
    Vi& vi() { return vi_; }
    Disc& disc() { return *disc_; }

private:
    explicit GameCube(std::unique_ptr<Disc> disc);

    std::unique_ptr<Disc> disc_;
    Memory memory_;
    Gekko cpu_;
    Scheduler scheduler_;
    Pi pi_;
    Mi mi_;
    Vi vi_;
    PadState pad_;
    Stats stats_;
    std::uint64_t cycle_remainder_ = 0;  // CPU cycles not yet a whole time base tick
};

}  // namespace ezy::gc
