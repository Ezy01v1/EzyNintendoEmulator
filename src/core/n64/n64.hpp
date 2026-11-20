#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <vector>

#include "n64/bus.hpp"
#include "n64/cartridge.hpp"
#include "n64/cpu/vr4300.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

class Vr4300Jit;

// How N64::step() runs the CPU: one instruction at a time (Vr4300::step), or blocks of
// kseg0 code from RDRAM through the JIT (Vr4300Jit), with the same result. The default is
// what N64::create applies.
enum class CpuMode { Interpreter, Jit };
CpuMode default_cpu_mode();
void set_default_cpu_mode(CpuMode mode);

// The whole console: what the frontend and the runner talk to.
//
// Main loop: each step() the CPU runs one instruction (or, with the JIT, blocks of them up
// to the next event), the scheduler advances by the cycles it took, and every event that is
// due (VI lines, DMA completions, RSP and RDP work) is dispatched to the bus.
class N64 {
public:
    // nullptr if the file isn't an N64 ROM. Without a PIF ROM the console boots through
    // hle_boot (which runs the cartridge's real IPL3); with a 1984-byte PIF ROM it starts
    // at the reset vector 0xBFC00000 like the hardware. Heap-allocated because the bus
    // and the CPU keep references to each other's neighbours.
    [[nodiscard]] static std::unique_ptr<N64> create(std::span<const std::uint8_t> rom,
                                                     std::span<const std::uint8_t> pif_rom = {},
                                                     RdpMode mode = default_rdp_mode(),
                                                     RspMode rsp_mode = ezy::n64::default_rsp_mode());
    static RdpMode default_rdp_mode();
    static void set_default_rdp_mode(RdpMode mode);

    N64(const N64&) = delete;
    N64& operator=(const N64&) = delete;
    ~N64();

    // Jit: false (and the CPU stays in the interpreter) when this build or host has no JIT.
    // Interpreter drops the JIT and its compiled code.
    bool set_cpu_mode(CpuMode mode);
    [[nodiscard]] CpuMode cpu_mode() const { return cpu_jit_ ? CpuMode::Jit : CpuMode::Interpreter; }
    [[nodiscard]] const Vr4300Jit* cpu_jit() const { return cpu_jit_.get(); }  // nullptr in interpreter mode
    [[nodiscard]] Vr4300Jit* cpu_jit() { return cpu_jit_.get(); }

    void step();
    // step() without the JIT: exactly one instruction (after an idle-loop skip), for the
    // debugger's single step.
    void step_interpreted();

    // Runs the due events; see n64.cpp.
    std::optional<std::uint64_t> run_due_events(bool watch_interrupt);
    void run_frame();  // until the VI finishes a field

    [[nodiscard]] Vr4300& cpu() { return cpu_; }
    [[nodiscard]] const Vr4300& cpu() const { return cpu_; }
    [[nodiscard]] Bus& bus() { return bus_; }
    [[nodiscard]] const Bus& bus() const { return bus_; }
    [[nodiscard]] const Cartridge& cartridge() const { return cart_; }
    [[nodiscard]] std::uint64_t cycles() const { return scheduler_.now(); }
    [[nodiscard]] Vi::Image image() const { return bus_.image(); }
    [[nodiscard]] Vi::Image output() const { return bus_.output(); }

    // Save states: the whole machine as bytes. load_state() refuses a state of another
    // game or format version and, if the state turns out to be truncated, leaves the
    // machine as it was.
    [[nodiscard]] std::vector<std::uint8_t> save_state();
    [[nodiscard]] bool load_state(std::span<const std::uint8_t> state);

private:
    explicit N64(Cartridge cart, RdpMode mode, RspMode rsp_mode);

    void skip_idle_loop();
    void step_cpu();  // one interpreted instruction, then the due events

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        scheduler_.serialize(ar, version);
        bus_.serialize(ar, version);
        cpu_.serialize(ar);
    }

    Cartridge cart_;
    Scheduler scheduler_;
    Bus bus_;
    Vr4300 cpu_;
    // Declared after bus_ and cpu_, so destroyed before them: it is the bus's code watch.
    std::unique_ptr<Vr4300Jit> cpu_jit_;
};

}  // namespace ezy::n64
