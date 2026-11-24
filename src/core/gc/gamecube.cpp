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
    : disc_(std::move(disc)),
      cpu_(memory_),
      vi_(scheduler_, pi_),
      di_(scheduler_, pi_, memory_.mem1(), *disc_),
      si_(scheduler_, pi_),
      exi_(pi_, memory_.mem1()),
      ai_(scheduler_, pi_),
      dsp_(scheduler_, pi_, memory_.mem1(), ai_),
      gx_(pi_, memory_.mem1()) {
    // Register blocks: every block is mapped; an offset a device does not model throws.
    memory_.map_mmio(MmioBlock::Cp, &gx_.cp());
    memory_.map_mmio(MmioBlock::Pe, &gx_.pe());
    memory_.map_mmio(MmioBlock::Pi, &pi_);
    memory_.map_mmio(MmioBlock::Mi, &mi_);
    memory_.map_mmio(MmioBlock::Vi, &vi_);
    memory_.map_mmio(MmioBlock::Dsp, &dsp_);
    memory_.map_mmio(MmioBlock::Di, &di_);
    memory_.map_mmio(MmioBlock::Si, &si_);
    memory_.map_mmio(MmioBlock::Exi, &exi_);
    memory_.map_mmio(MmioBlock::Ai, &ai_);
    memory_.set_write_gather_pipe(&gx_);

    // Interrupt wiring: peripherals -> PI cause bits -> the Gekko's external interrupt line.
    pi_.set_line_sink([this](bool level) { cpu_.set_external_interrupt(level); });
    mi_.set_irq_sink([this](bool level) { pi_.set_cause(Pi::kMi, level); });
    pi_.set_fifo_reset_sink([this] { gx_.reset_pipe(); });

    hle_boot(cpu_, memory_, *disc_);
}

GameCube::~GameCube() = default;

void GameCube::run_field() {
    const std::uint64_t fields_before = vi_.fields();
    while (vi_.fields() == fields_before) {
        const int cycles = cpu_.step();
        ++instructions_;
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
                    if (vi_.on_event(due->when)) gx_.end_field();
                    break;
                case EventType::DiTransfer: di_.on_event(); break;
                case EventType::SiPoll: si_.on_event(due->when); break;
                case EventType::ArDma: dsp_.on_aram_dma_done(); break;
                case EventType::AiDmaStart: dsp_.on_ai_dma_start(); break;
                case EventType::AiDmaBlock: dsp_.on_ai_dma_block(due->when); break;
                case EventType::AiSample: ai_.on_event(); break;
                case EventType::Count: break;
            }
        }
    }
}

Image GameCube::xfb() const { return vi_.xfb_image(memory_.mem1()); }

Stats GameCube::stats() const {
    Stats s;
    s.instructions = instructions_;
    for (unsigned bit = 0; bit < Pi::kCauseCount; ++bit) s.interrupts[bit] = pi_.interrupts(bit);
    s.di_reads = di_.reads();
    s.dsp_mails = dsp_.mails_received();
    const GxCounters& gx = gx_.last_field();
    s.fifo_bytes_last_field = gx.fifo_bytes;
    s.gx_commands_last_field = gx.commands;
    s.gx_primitives_last_field = gx.primitives;
    s.gx_vertices_last_field = gx.vertices;
    s.fields = vi_.fields();
    return s;
}

}  // namespace ezy::gc
