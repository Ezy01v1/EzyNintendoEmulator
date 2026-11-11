#include "n64/n64.hpp"

#include <algorithm>
#include <array>

#include "common/state.hpp"
#include "n64/hle_boot.hpp"

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kStateMagic = 0x364E5A45;  // "EZN6"
constexpr std::uint32_t kStateVersion = 11;          // bump whenever a serialize() changes
constexpr std::uint32_t kFirstStateVersion = 1;
constexpr std::size_t kPifRomSize = 1984;
// A field is at most this many instructions even if the VI is misprogrammed.
constexpr std::uint64_t kMaxFrameCycles = 93'750'000 / 10;

}  // namespace

N64::N64(Cartridge cart, RdpMode mode) : cart_(std::move(cart)), bus_(scheduler_, cart_, mode), cpu_(bus_) {}

RdpMode N64::default_rdp_mode() { return ezy::n64::default_rdp_mode(); }
void N64::set_default_rdp_mode(RdpMode mode) { ezy::n64::set_default_rdp_mode(mode); }

std::unique_ptr<N64> N64::create(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> pif_rom, RdpMode mode) {
    auto cart = Cartridge::load(rom);
    if (!cart) return nullptr;
    std::unique_ptr<N64> n64(new N64(std::move(*cart), mode));
    if (pif_rom.size() == kPifRomSize) {
        n64->bus_.set_pif_rom(pif_rom);
        n64->cpu_.set_pc(0xFFFFFFFFBFC00000ULL);  // Cop0 starts in its reset state (BEV, ERL)
    } else {
        hle_boot(n64->cpu_, n64->bus_, n64->cart_);
    }
    return n64;
}

void N64::step() {
    // Idle loop: jump over whole iterations up to just before the next event or Compare, so
    // everything happens at the same cycle as when stepping one instruction at a time.
    //
    // The RSP, RDP and SP DMA keep running meanwhile: their handlers only use the event
    // time, never now(), so they run in order up to that point; the first one that makes
    // the CPU take an interrupt stops the run, and the CPU continues from that cycle.
    if (cpu_.idle_loop_cycles()) {
        constexpr std::uint32_t kBatchable = Scheduler::bit(EventType::RspStep) | Scheduler::bit(EventType::RdpWord) |
                                             Scheduler::bit(EventType::SpDma);
        const std::uint64_t start = scheduler_.now();
        const std::uint64_t limit = std::min(start + cpu_.cycles_until_compare(), scheduler_.earliest_except(kBatchable));
        if (limit > start + 1) {
            std::uint64_t stop = limit - 1;  // the last cycle reached before the limit's step
            scheduler_.advance_to(stop);
            if (const auto interrupted = run_due_events(true)) {
                stop = std::max(*interrupted, start);
                scheduler_.rewind_to(stop);
                run_due_events(false);  // the rest due at that cycle still runs before the CPU's step
            }
            cpu_.skip_idle_cycles(stop - start);
        }
    }
    scheduler_.advance(static_cast<std::uint64_t>(cpu_.step()));
    run_due_events(false);
}

// Runs every due event in order. With watch_interrupt, stops right after the first one
// that makes the CPU take an interrupt and returns its time.
std::optional<std::uint64_t> N64::run_due_events(bool watch_interrupt) {
    for (;;) {
        std::uint64_t when = 0;
        if (scheduler_.take_rsp_step(when)) {
            // Only the MI line can change while the RSP runs, so the CPU would take the
            // interrupt as soon as it rises if IP2 is enabled.
            const auto interrupted = bus_.rsp_run(when, watch_interrupt && cpu_.takes_ip2());
            scheduler_.settle_rsp();
            if (interrupted) return interrupted;
        }
        const auto due = scheduler_.pop_due();
        if (!due) return std::nullopt;
        bus_.handle_event(due->type, due->when);
        if (watch_interrupt && cpu_.interrupt_pending()) return due->when;
    }
}

void N64::run_frame() {
    const std::uint64_t limit = scheduler_.now() + kMaxFrameCycles;
    while (!bus_.vi().take_vblank() && scheduler_.now() < limit) step();
}

std::vector<std::uint8_t> N64::save_state() {
    bus_.drain_rdp(); // Drain before serializing even the scheduler and RAM.
    StateWriter writer;
    std::uint32_t magic = kStateMagic, version = kStateVersion, crc = cart_.crc32();
    writer(magic), writer(version), writer(crc);
    serialize(writer, version);
    return writer.take();
}

bool N64::load_state(std::span<const std::uint8_t> state) {
    StateReader header(state);
    std::uint32_t magic = 0, version = 0, crc = 0;
    header(magic), header(version), header(crc);
    if (header.failed() || magic != kStateMagic || version < kFirstStateVersion || version > kStateVersion ||
        crc != cart_.crc32()) return false;

    std::vector<std::uint8_t> previous = save_state();
    StateReader reader(state);
    reader(magic), reader(version), reader(crc);
    serialize(reader, version);
    if (reader.failed() || !reader.at_end()) {
        StateReader undo(previous);
        undo(magic), undo(version), undo(crc);
        serialize(undo, version);
        return false;
    }
    return true;
}

}  // namespace ezy::n64
