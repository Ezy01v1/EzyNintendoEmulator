#include "gba/gba.hpp"

#include <vector>

namespace ezy::gba {

namespace {

constexpr std::uint32_t kRomEntry = 0x08000000;

// Stack pointers the BIOS sets up before jumping to the cartridge (GBATEK "BIOS RAM Usage").
constexpr std::uint32_t kStackSvc = 0x03007FE0;
constexpr std::uint32_t kStackIrq = 0x03007FA0;
constexpr std::uint32_t kStackUser = 0x03007F00;

}  // namespace

std::unique_ptr<Gba> Gba::create(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios) {
    auto header = parse_header(rom);
    if (!header) return nullptr;
    return std::unique_ptr<Gba>(new Gba(rom, bios, std::move(*header)));
}

Gba::Gba(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> bios, CartridgeHeader header)
    : bus_(std::vector<std::uint8_t>(rom.begin(), rom.end()), bios, &scheduler_),
      cpu_(bus_),
      header_(std::move(header)) {
    if (bios.empty()) {
        boot_without_bios();
    } else {
        cpu_.reset();  // the BIOS runs from 0x00000000 and jumps to the cartridge itself
    }
    bus_.ppu().start();
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
            case EventType::PpuHDrawEnd: bus_.ppu().on_hdraw_end(event->when); break;
            case EventType::PpuHBlankEnd:
                bus_.ppu().on_hblank_end(event->when);
                if (bus_.ppu().take_frame_done()) {
                    ++frames_;
                    frame_done_ = true;
                }
                break;
            case EventType::Count: break;
        }
    }
}

void Gba::run_until(std::uint64_t cycle) {
    while (scheduler_.now() < cycle) step();
}

void Gba::run_frame() {
    frame_done_ = false;
    while (!frame_done_) step();
}

}  // namespace ezy::gba
