#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "n64/mi.hpp"
#include "n64/rdp.hpp"
#include "n64/rsp_vector.hpp"
#include "n64/rsp_jit.hpp"
#include "n64/scheduler.hpp"

namespace ezy::n64 {

enum class RspMode { Interpreter, Jit };
RspMode default_rsp_mode();
void set_default_rsp_mode(RspMode mode);

// RSP scalar and vector cores, their two 4 KiB memory banks and the SP DMA interface.
class RspRegs {
public:
    explicit RspRegs(RspMode mode = default_rsp_mode());
    ~RspRegs();
    [[nodiscard]] bool jit_active() const { return jit_ && jit_->available(); }
    [[nodiscard]] RspJit::Stats jit_stats() const { return jit_ ? jit_->stats() : RspJit::Stats{}; }
    [[nodiscard]] const std::uint8_t* mem() const { return mem_.data(); } // inspection only
    [[nodiscard]] std::uint8_t read_memory8(std::uint32_t address) const { return mem_[address & 0x1FFF]; }
    void write_memory(std::uint32_t address, std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::span<const std::uint8_t, 0x1000> dmem() const { return std::span(mem_).first<0x1000>(); }
    [[nodiscard]] std::uint32_t read_sp(std::uint32_t offset);
    void write_sp(std::uint32_t offset, std::uint32_t value, Mi& mi, Scheduler& scheduler, std::uint64_t when);
    void finish_dma_row(Scheduler& scheduler, std::span<std::uint8_t> rdram, std::uint64_t when);
    void step(Mi& mi, Rdp& rdp, Scheduler& scheduler, std::uint64_t when);
    // Steps from `when` while each next one is due and before every other event (see rsp_scalar.cpp).
    std::optional<std::uint64_t> run(Mi& mi, Rdp& rdp, Scheduler& scheduler, std::uint64_t when, bool stop_on_line);

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version, Rdp& rdp) {
        if constexpr (Ar::kLoading) invalidate_jit();
        ar(mem_);
        std::array<std::uint32_t, 8> legacy_regs{};
        if (version == 1) ar(legacy_regs);
        ar(sp_status_), ar(semaphore_), ar(sp_pc_);
        if (version < 4) rdp.load_legacy(ar);
        if (version == 1) {
            dma_ = {};
            pending_ = {};
            dma_.sp_address = pending_.sp_address = legacy_regs[0] & 0x1FF8;
            dma_.dram_address = pending_.dram_address = legacy_regs[1] & 0xFFFFF8;
            gpr_ = {};
            next_pc_ = (sp_pc_ + 4) & kPcMask;
            short_step_ = false;
        } else {
            ar(dma_), ar(pending_), ar(gpr_), ar(next_pc_), ar(short_step_);
        }
        if (version >= 3) vector_.serialize(ar);
        else vector_ = {};
    }

private:
    static void jit_vector(void* self, std::uint32_t word) noexcept;
    void invalidate_jit() { ++imem_generation_; }
    std::uint64_t imem_generation_ = 0; // host cache identity, never serialized
    static constexpr std::uint32_t kPcMask = 0xFFC;
    static constexpr std::uint32_t kHalt = 1U << 0, kBroke = 1U << 1, kSingleStep = 1U << 5,
                                   kInterruptOnBreak = 1U << 6;
    enum class DmaDirection : std::uint8_t { Idle, ToSp, ToDram };
    struct Dma {
        std::uint32_t sp_address = 0;
        std::uint32_t dram_address = 0;
        std::uint32_t length = 0;
        DmaDirection direction = DmaDirection::Idle;

        template <class Ar>
        void serialize(Ar& ar) { ar(sp_address), ar(dram_address), ar(length), ar(direction); }
    };

    void start_dma(Scheduler& scheduler, std::uint64_t when);
    void schedule_dma_row(Scheduler& scheduler, std::uint64_t when) const;
    void write_status(std::uint32_t value, Mi& mi, Scheduler& scheduler, std::uint64_t when);
    [[nodiscard]] std::uint32_t load_dmem(std::uint32_t address, unsigned width) const;
    void store_dmem(std::uint32_t address, unsigned width, std::uint32_t value);
    void execute_scalar(std::uint32_t word, std::uint32_t pc, Mi& mi, Rdp& rdp, Scheduler& scheduler, std::uint64_t when);
    void execute_special(std::uint32_t word, std::uint32_t pc, Mi& mi);

    std::array<std::uint8_t, 0x2000> mem_{};
    std::uint32_t sp_status_ = kHalt;
    std::uint32_t semaphore_ = 0;
    std::uint32_t sp_pc_ = 0;
    Dma dma_, pending_;
    std::array<std::uint32_t, 32> gpr_{};
    std::uint32_t next_pc_ = 4;  // a branch sets the PC after its delay slot
    bool short_step_ = false;   // alternate 2 and 1 CPU cycles per RSP cycle
    RspVector vector_;
    std::unique_ptr<RspJit> jit_;
    std::uint64_t jit_generation_ = ~std::uint64_t{0};
};

}  // namespace ezy::n64
