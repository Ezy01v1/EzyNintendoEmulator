#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "gc/ai.hpp"
#include "gc/di.hpp"
#include "gc/dsp.hpp"
#include "gc/exi.hpp"
#include "gc/gekko/gekko.hpp"
#include "gc/gx_fifo.hpp"
#include "gc/memory.hpp"
#include "gc/mi.hpp"
#include "gc/pi.hpp"
#include "gc/scheduler.hpp"
#include "gc/si.hpp"
#include "gc/vi.hpp"

namespace ezy::gc {

class Disc;

// What the runner prints. stats() fills it from the peripherals. The graphics FIFO's counts
// are the last finished field's (run_field() returns as a field starts, so the field in
// progress has seen nothing yet; GxFifo::counters() has it); the rest count since the boot.
struct Stats {
    std::uint64_t instructions = 0;
    std::array<std::uint64_t, Pi::kCauseCount> interrupts{};  // rising edges per PI cause bit
    std::uint64_t di_reads = 0;               // DI Read commands (Di::reads())
    // The graphics FIFO in the last finished field (GxFifo::last_field()).
    std::uint64_t fifo_bytes_last_field = 0;  // written to the FIFO by the pipe
    std::array<std::uint64_t, kGxCommandCount> gx_commands_last_field{};  // by GxCommand
    std::uint64_t gx_primitives_last_field = 0;
    std::uint64_t gx_vertices_last_field = 0;
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
    // One instruction (or the interrupt the Gekko takes instead) and the events it makes due:
    // run_field()'s loop body, for the debugger's single step. Throws as run_field().
    void step();

    // The picture the VI is showing: ARGB, 640 wide, the frame's height (see Vi::xfb_image).
    [[nodiscard]] Image xfb() const;
    [[nodiscard]] Stats stats() const;
    // Controller 1 (PadState is in gc/si.hpp; the SI's channel 0 reports it).
    void set_pad(const PadState& pad) { si_.set_pad(pad); }
    [[nodiscard]] const PadState& pad() const { return si_.pad(); }

    // The PC trace the runner prints when emulation stops: run_field() records the pc of every
    // instruction it steps, keeping the last `depth` (0, the default, turns it off).
    void set_pc_trace(std::size_t depth);
    // The recorded PCs, oldest first; the newest is the instruction stepped last (when a step
    // threw, the one that failed).
    [[nodiscard]] std::vector<std::uint32_t> recent_pcs() const;

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
    GxFifo& gx() { return gx_; }
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
    GxFifo gx_;
    std::uint64_t instructions_ = 0;
    std::uint64_t cycle_remainder_ = 0;  // CPU cycles not yet a whole time base tick
    std::vector<std::uint32_t> pc_trace_;  // a ring of set_pc_trace()'s depth
    std::size_t pc_trace_next_ = 0;        // where the next pc goes
    std::size_t pc_trace_count_ = 0;       // how many entries are filled (up to the depth)
};

}  // namespace ezy::gc
