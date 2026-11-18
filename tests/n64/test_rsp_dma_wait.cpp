#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "common/state.hpp"
#include "mips_asm.hpp"
#include "n64/rsp_regs.hpp"

namespace {

using namespace ezy::n64;

// One RSP with its own scheduler, MI, RDP and RDRAM, driven either one instruction per
// scheduler event (step, the reference) or by run(), which may skip DMA wait loops.
struct World {
    RspRegs rsp{RspMode::Interpreter};
    Scheduler scheduler;
    Mi mi;
    Rdp rdp;
    std::vector<std::uint8_t> rdram = std::vector<std::uint8_t>(0x10000);

    void load(const std::vector<std::uint32_t>& words) {
        for (std::size_t i = 0; i < rdram.size(); ++i) rdram[i] = static_cast<std::uint8_t>(i * 7 + 3);
        std::vector<std::uint8_t> bytes;
        for (const auto word : words)
            for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(word >> (24 - 8 * i)));
        rsp.write_memory(0x1000, bytes);
        rsp.write_sp(0x10, 0x101, mi, scheduler, 0);  // clear halt, interrupt on break
    }

    void advance(std::uint64_t bound, std::uint64_t vi_period, bool use_run) {
        scheduler.advance_to(bound);
        while (const auto due = scheduler.pop_due()) {
            switch (due->type) {
                case EventType::RspStep:
                    if (use_run) rsp.run(mi, rdp, scheduler, due->when, false);
                    else rsp.step(mi, rdp, scheduler, due->when);
                    break;
                case EventType::SpDma: rsp.finish_dma_row(scheduler, rdram, due->when); break;
                case EventType::ViLine: scheduler.schedule(EventType::ViLine, due->when + vi_period); break;
                default: break;
            }
        }
    }

    std::vector<std::uint8_t> state() {
        ezy::StateWriter writer;
        rsp.serialize(writer, 12, rdp);
        mi.serialize(writer);
        return writer.take();
    }
};

// Two chained SP DMA reads of two 256-byte rows each, a wait on SP_DMA_FULL and then one
// on SP_DMA_BUSY (with the BNE operands the other way round), then BREAK.
std::vector<std::uint32_t> program(unsigned prelude) {
    std::vector<std::uint32_t> words(prelude, mips::nop());
    const std::vector<std::uint32_t> body{
        mips::addiu(1, 0, (1 << 12) | 0xF8),
        mips::mtc0(0, 0), mips::mtc0(0, 1),
        mips::mtc0(1, 2),  // starts the first DMA
        mips::mtc0(1, 2),  // queues the second one
        mips::mfc0(2, 5), mips::bne(2, 0, -2), mips::nop(),
        mips::mfc0(3, 6), mips::bne(0, 3, -2), mips::nop(),
        mips::addiu(4, 0, 1),
        mips::r(0, 0, 0, 0, 0, 13),  // BREAK
    };
    words.insert(words.end(), body.begin(), body.end());
    return words;
}

}  // namespace

TEST_CASE("RSP run skips DMA wait loops with the same state and timing as stepping") {
    for (unsigned prelude : {0U, 1U, 2U}) {
        for (std::uint64_t bound_step : {1ULL, 5ULL, 7ULL, 64ULL, 1000ULL}) {
            for (std::uint64_t vi_period : {13ULL, 37ULL, 100000ULL}) {
                CAPTURE(prelude);
                CAPTURE(bound_step);
                CAPTURE(vi_period);
                World reference, skipping;
                reference.load(program(prelude));
                skipping.load(program(prelude));
                reference.scheduler.schedule(EventType::ViLine, vi_period);
                skipping.scheduler.schedule(EventType::ViLine, vi_period);
                for (std::uint64_t bound = 0; bound < 3000; bound += bound_step) {
                    reference.advance(bound, vi_period, false);
                    skipping.advance(bound, vi_period, true);
                    if (reference.state() != skipping.state() ||
                        reference.scheduler.next_event_time() != skipping.scheduler.next_event_time()) {
                        CAPTURE(bound);
                        FAIL("RSP state diverged");
                    }
                }
                CHECK(reference.rsp.halted());
                CHECK(reference.rsp.skipped_dma_wait_iterations() == 0);
                if (bound_step >= 64 && vi_period >= 37) CHECK(skipping.rsp.skipped_dma_wait_iterations() > 0);
            }
        }
    }
}
