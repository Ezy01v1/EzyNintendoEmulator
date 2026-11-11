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

// The whole console: what the frontend and the runner talk to.
//
// Main loop: the CPU runs one instruction, the scheduler advances by the cycles it took,
// and every event that is due (VI lines, DMA completions, RSP and RDP work) is dispatched to the bus.
class N64 {
public:
    // nullptr if the file isn't an N64 ROM. Without a PIF ROM the console boots through
    // hle_boot (which runs the cartridge's real IPL3); with a 1984-byte PIF ROM it starts
    // at the reset vector 0xBFC00000 like the hardware. Heap-allocated because the bus
    // and the CPU keep references to each other's neighbours.
    [[nodiscard]] static std::unique_ptr<N64> create(std::span<const std::uint8_t> rom,
                                                     std::span<const std::uint8_t> pif_rom = {},
                                                     RdpMode mode = default_rdp_mode());
    static RdpMode default_rdp_mode();
    static void set_default_rdp_mode(RdpMode mode);

    N64(const N64&) = delete;
    N64& operator=(const N64&) = delete;

    void step();

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

    // Save states: the whole machine as bytes. load_state() refuses a state of another
    // game or format version and, if the state turns out to be truncated, leaves the
    // machine as it was.
    [[nodiscard]] std::vector<std::uint8_t> save_state();
    [[nodiscard]] bool load_state(std::span<const std::uint8_t> state);

private:
    explicit N64(Cartridge cart, RdpMode mode);

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
};

}  // namespace ezy::n64
