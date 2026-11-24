#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "gc/ai.hpp"
#include "gc/di.hpp"
#include "gc/dsp.hpp"
#include "gc/exi.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/mi.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "gc/si.hpp"
#include "gc/vi.hpp"

namespace ezy::gc {

class Disc;

// What the runner prints. stats() fills the interrupt counts, the DI reads and the DSP mails
// from the peripherals; the FIFO bytes are a plain field the graphics FIFO (Task 6) bumps
// through GameCube::stats_mut().
struct Stats {
    std::uint64_t instructions = 0;
    std::array<std::uint64_t, Pi::kCauseCount> interrupts{};  // rising edges per PI cause bit
    std::uint64_t di_reads = 0;               // DI Read commands (Di::reads())
    std::uint64_t fifo_bytes_this_field = 0;  // reset at each field start
    std::uint64_t dsp_mails = 0;              // mails CPU->DSP (Dsp::mails_received())
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
    // Controller 1 (PadState is in gc/si.hpp; the SI's channel 0 reports it).
    void set_pad(const PadState& pad) { si_.set_pad(pad); }
    [[nodiscard]] const PadState& pad() const { return si_.pad(); }

    // Gekko cycles since the boot.
    [[nodiscard]] std::uint64_t cycles() const { return scheduler_.now(); }

    // Parts, for tests, the runner and the later tasks' peripherals.
    Memory& memory() { return memory_; }
    Gekko& cpu() { return cpu_; }
    Scheduler& scheduler() { return scheduler_; }
    Pi& pi() { return pi_; }
    Vi& vi() { return vi_; }
    Di& di() { return di_; }
    Si& si() { return si_; }
    Exi& exi() { return exi_; }
    Ai& ai() { return ai_; }
    Dsp& dsp() { return dsp_; }
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
    Di di_;
    Si si_;
    Exi exi_;
    Ai ai_;
    Dsp dsp_;  // after ai_: its AI DMA plays at the AI's rate
    Stats stats_;
    std::uint64_t cycle_remainder_ = 0;  // CPU cycles not yet a whole time base tick
};

}  // namespace ezy::gc
