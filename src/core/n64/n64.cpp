#include "n64/n64.hpp"

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

N64::N64(Cartridge cart) : cart_(std::move(cart)), bus_(scheduler_, cart_), cpu_(bus_) {}

std::unique_ptr<N64> N64::create(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> pif_rom) {
    auto cart = Cartridge::load(rom);
    if (!cart) return nullptr;
    std::unique_ptr<N64> n64(new N64(std::move(*cart)));
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
    if (const int per = cpu_.idle_loop_cycles()) {
        const std::uint64_t now = scheduler_.now();
        std::uint64_t target = now + cpu_.cycles_until_compare();
        if (const auto next = scheduler_.next_event_time(); next && *next < target) target = *next;
        if (target > now + 1) {
            const std::uint64_t iterations = (target - now - 1) / static_cast<std::uint64_t>(per);
            if (iterations > 0) {
                cpu_.skip_idle_loop(iterations);
                scheduler_.advance(iterations * static_cast<std::uint64_t>(per));
            }
        }
    }
    scheduler_.advance(static_cast<std::uint64_t>(cpu_.step()));
    for (;;) {
        std::uint64_t when = 0;
        if (scheduler_.take_rsp_step(when)) {
            bus_.rsp_step(when);
            while (scheduler_.take_rsp_step(when)) bus_.rsp_step(when);
            scheduler_.settle_rsp();
        }
        const auto due = scheduler_.pop_due();
        if (!due) break;
        bus_.handle_event(due->type, due->when);
    }
}

void N64::run_frame() {
    const std::uint64_t limit = scheduler_.now() + kMaxFrameCycles;
    while (!bus_.vi().take_vblank() && scheduler_.now() < limit) step();
}

std::vector<std::uint8_t> N64::save_state() {
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
