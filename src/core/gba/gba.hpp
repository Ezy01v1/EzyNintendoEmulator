#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "gba/bus.hpp"
#include "gba/cartridge.hpp"
#include "gba/cpu/arm7tdmi.hpp"
#include "gba/ppu.hpp"
#include "gba/scheduler.hpp"

namespace ezy::gba {

inline constexpr std::uint32_t kCpuHz = 16'777'216;
inline constexpr std::uint32_t kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;  // 280,896 (~59.73 Hz)

// The whole console: what the frontend talks to (the GBA counterpart of gb::GameBoy).
//
// Main loop: the CPU executes one instruction at a time, the bus charges its cycles to the
// scheduler, and between instructions every due event is dispatched. Components never
// "tick"; they schedule their next event instead.
class Gba final : private SwiHook {
public:
    // Returns nullptr if the ROM can't be parsed. Heap-allocated because the CPU keeps a
    // reference to the bus (and the bus a pointer to the scheduler), so it must not move.
    // Without a BIOS, maps our own minimal BIOS (hle_bios.hpp), emulates BIOS calls in
    // C++ and starts at the cartridge entry point in the state the real BIOS leaves.
    [[nodiscard]] static std::unique_ptr<Gba> create(std::span<const std::uint8_t> rom,
                                                     std::span<const std::uint8_t> bios = {});

    Gba(const Gba&) = delete;
    Gba& operator=(const Gba&) = delete;

    // One instruction (or, while halted, a jump to the next event), then due events.
    // Pending interrupts are taken before the instruction.
    void step();
    void run_until(std::uint64_t cycle);
    void run_frame();  // until the next VBlank starts (the frame is complete)

    [[nodiscard]] Arm7tdmi& cpu() { return cpu_; }
    [[nodiscard]] const Arm7tdmi& cpu() const { return cpu_; }
    [[nodiscard]] Bus& bus() { return bus_; }
    [[nodiscard]] const Bus& bus() const { return bus_; }
    [[nodiscard]] Scheduler& scheduler() { return scheduler_; }
    [[nodiscard]] const Scheduler& scheduler() const { return scheduler_; }
    [[nodiscard]] const CartridgeHeader& header() const { return header_; }
    [[nodiscard]] std::uint64_t frame_count() const { return frames_; }

private:
    Gba(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios, CartridgeHeader header);

    void boot_without_bios();
    void dispatch_due_events();

    // BIOS calls (hle_swi.cpp).
    bool on_swi(std::uint8_t number) override;
    bool intr_wait(bool discard, std::uint16_t flags);
    void swi_div(std::uint32_t numerator, std::uint32_t denominator);
    void swi_cpu_set(bool fast);

    // Declaration order is construction order: the bus needs the scheduler, the CPU the bus.
    Scheduler scheduler_;
    Bus bus_;
    Arm7tdmi cpu_;
    CartridgeHeader header_;
    std::uint64_t frames_ = 0;
    bool frame_done_ = false;
    bool intr_waiting_ = false;  // inside an IntrWait that is being re-executed
};

}  // namespace ezy::gba
