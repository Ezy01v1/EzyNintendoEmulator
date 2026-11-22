#include "gc/gamecube.hpp"

#include "gc/disc.hpp"
#include "gc/hle_boot.hpp"

namespace ezy::gc {

std::unique_ptr<GameCube> GameCube::create(const std::filesystem::path& path) {
    auto disc = Disc::open(path);
    if (!disc) return nullptr;
    return std::unique_ptr<GameCube>(new GameCube(std::move(disc)));
}

GameCube::GameCube(std::unique_ptr<Disc> disc)
    : disc_(std::move(disc)), cpu_(memory_), vi_(scheduler_, pi_) {
    // Register blocks. Tasks 5-6 register DI, SI, EXI, AI, the DSP interface, CP and PE here
    // (memory_.map_mmio(MmioBlock::Di, &di_), ...); a block nobody maps throws when touched.
    memory_.map_mmio(MmioBlock::Pi, &pi_);
    memory_.map_mmio(MmioBlock::Mi, &mi_);
    memory_.map_mmio(MmioBlock::Vi, &vi_);

    // Interrupt wiring: peripherals -> PI cause bits -> the Gekko's external interrupt line.
    pi_.set_line_sink([this](bool level) { cpu_.set_external_interrupt(level); });
    mi_.set_irq_sink([this](bool level) { pi_.set_cause(Pi::kMi, level); });

    hle_boot(cpu_, memory_, *disc_);
}

GameCube::~GameCube() = default;

void GameCube::run_field() {
    const std::uint64_t fields_before = vi_.fields();
    while (vi_.fields() == fields_before) {
        const int cycles = cpu_.step();
        ++stats_.instructions;
        scheduler_.advance(static_cast<std::uint64_t>(cycles));
        // The time base ticks once per 12 CPU cycles; the decrementer follows it.
        cycle_remainder_ += static_cast<std::uint64_t>(cycles);
        if (cycle_remainder_ >= 12) {
            cpu_.advance_timebase(cycle_remainder_ / 12);
            cycle_remainder_ %= 12;
        }
        while (const auto due = scheduler_.pop_due()) {
            switch (due->type) {
                case EventType::ViLine:
                    if (vi_.on_event(due->when)) stats_.fifo_bytes_this_field = 0;
                    break;
                case EventType::DiTransfer:  // Task 5
                case EventType::Count:
                    break;
            }
        }
    }
    stats_.fields = vi_.fields();
}

Image GameCube::xfb() const { return vi_.xfb_image(const_cast<Memory&>(memory_).mem1()); }

Stats GameCube::stats() const {
    Stats s = stats_;
    for (unsigned bit = 0; bit < Pi::kCauseCount; ++bit) s.interrupts[bit] = pi_.interrupts(bit);
    s.fields = vi_.fields();
    return s;
}

}  // namespace ezy::gc
