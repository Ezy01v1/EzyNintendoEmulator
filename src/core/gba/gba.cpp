#include "gba/gba.hpp"

#include <vector>

#include "gba/hle_bios.hpp"

namespace ezy::gba {

namespace {

constexpr std::uint32_t kRomEntry = 0x08000000;

// Stack pointers the BIOS sets up before jumping to the cartridge (GBATEK "BIOS RAM Usage").
constexpr std::uint32_t kStackSvc = 0x03007FE0;
constexpr std::uint32_t kStackIrq = 0x03007FA0;
constexpr std::uint32_t kStackUser = 0x03007F00;

constexpr auto kHleBios = hle_bios_image();

}  // namespace

std::unique_ptr<Gba> Gba::create(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios) {
    auto header = parse_header(rom);
    if (!header) return nullptr;
    return std::unique_ptr<Gba>(new Gba(rom, bios, std::move(*header)));
}

Gba::Gba(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios, CartridgeHeader header)
    : bus_(std::vector<std::uint8_t>(rom.begin(), rom.end()), bios.empty() ? std::span(kHleBios) : bios,
           &scheduler_),
      cpu_(bus_),
      header_(std::move(header)) {
    if (bios.empty()) {
        cpu_.set_swi_hook(this);
        boot_without_bios();
    } else {
        cpu_.reset();  // the BIOS runs from 0x00000000 and jumps to the cartridge itself
    }
    bus_.ppu().start();
    bus_.apu().start();
}

// The state the BIOS leaves when it hands over to the cartridge: System mode, ARM state,
// interrupts enabled in the CPSR, stacks set up for SVC/IRQ/System.
void Gba::boot_without_bios() {
    cpu_.set_banked(kBankSvc, 5, kStackSvc);
    cpu_.set_banked(kBankIrq, 5, kStackIrq);
    cpu_.set_cpsr_raw(static_cast<std::uint32_t>(Mode::System));
    cpu_.set_reg(13, kStackUser);
    cpu_.jump(kRomEntry);
}

void Gba::dispatch_due_events() {
    while (const auto event = scheduler_.pop_due()) {
        switch (event->type) {
            case EventType::PpuHDrawEnd:
                bus_.ppu().on_hdraw_end(event->when);
                if (bus_.ppu().vcount() < kScreenHeight) bus_.on_hblank();  // HBlank DMA: visible lines only
                break;
            case EventType::PpuHBlankEnd:
                bus_.ppu().on_hblank_end(event->when);
                if (bus_.ppu().take_frame_done()) {
                    ++frames_;
                    frame_done_ = true;
                    bus_.on_vblank();
                }
                break;
            case EventType::Timer0: bus_.timers().on_overflow(0, event->when); break;
            case EventType::Timer1: bus_.timers().on_overflow(1, event->when); break;
            case EventType::Timer2: bus_.timers().on_overflow(2, event->when); break;
            case EventType::Timer3: bus_.timers().on_overflow(3, event->when); break;
            case EventType::ApuSample: bus_.apu().on_sample(event->when); break;
            case EventType::Count: break;
        }
    }
}

void Gba::step() {
    if (bus_.halted()) {
        if (!bus_.interrupt_pending()) {
            // Nothing can happen before the next event: skip straight to it.
            if (const auto next = scheduler_.next_event_time()) scheduler_.advance_to(*next);
            dispatch_due_events();
            return;
        }
        bus_.set_halted(false);  // IE & IF wakes the CPU even with IME off
    }
    if (bus_.interrupt_master_enable() && bus_.interrupt_pending()) (void)cpu_.irq();
    cpu_.step();
    dispatch_due_events();
}

void Gba::run_until(std::uint64_t cycle) {
    while (scheduler_.now() < cycle) step();
}

void Gba::run_frame() {
    frame_done_ = false;
    while (!frame_done_) step();
}

}  // namespace ezy::gba
