#include "gba/gba.hpp"

#include <array>
#include <vector>

#include "gba/hle_bios.hpp"
#include "gba/state.hpp"

namespace ezy::gba {

namespace {

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
void Gba::boot_without_bios(std::uint32_t entry) {
    cpu_.set_banked(kBankSvc, 5, kStackSvc);
    cpu_.set_banked(kBankIrq, 5, kStackIrq);
    cpu_.set_cpsr_raw(static_cast<std::uint32_t>(Mode::System));
    cpu_.set_reg(13, kStackUser);
    cpu_.jump(entry);
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

namespace {

constexpr std::uint32_t kStateMagic = 0x41424745;  // "EGBA"
constexpr std::uint32_t kStateVersion = 1;          // bump whenever a serialize() changes

struct StateHeader {
    std::uint32_t magic = kStateMagic;
    std::uint32_t version = kStateVersion;
    std::array<char, 4> game_code{};
    std::uint8_t header_checksum = 0;
};

StateHeader header_for(const CartridgeHeader& cart) {
    StateHeader h;
    for (std::size_t i = 0; i < h.game_code.size() && i < cart.game_code.size(); ++i) h.game_code[i] = cart.game_code[i];
    h.header_checksum = cart.header_checksum;
    return h;
}

}  // namespace

std::vector<std::uint8_t> Gba::save_state() {
    StateWriter writer;
    StateHeader h = header_for(header_);
    writer(h.magic);
    writer(h.version);
    writer(h.game_code);
    writer(h.header_checksum);
    serialize(writer);
    return writer.take();
}

bool Gba::load_state(std::span<const std::uint8_t> state) {
    StateReader reader(state);
    StateHeader h;
    reader(h.magic);
    reader(h.version);
    reader(h.game_code);
    reader(h.header_checksum);
    const StateHeader want = header_for(header_);
    if (reader.failed() || h.magic != want.magic || h.version != want.version || h.game_code != want.game_code ||
        h.header_checksum != want.header_checksum) {
        return false;
    }
    std::vector<std::uint8_t> previous = save_state();
    serialize(reader);
    if (reader.failed() || !reader.at_end()) {
        StateReader undo(previous);
        undo(h.magic);
        undo(h.version);
        undo(h.game_code);
        undo(h.header_checksum);
        serialize(undo);
        return false;
    }
    return true;
}

}  // namespace ezy::gba
