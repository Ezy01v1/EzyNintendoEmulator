#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "n64/ai.hpp"
#include "n64/cartridge.hpp"
#include "n64/cpu/cpu_bus.hpp"
#include "n64/gfx_hle/f3dex.hpp"
#include "n64/mi.hpp"
#include "n64/pi.hpp"
#include "n64/pif.hpp"
#include "n64/rdp.hpp"
#include "n64/rsp_audio_hle.hpp"
#include "n64/rsp_regs.hpp"
#include "n64/scheduler.hpp"
#include "n64/si.hpp"
#include "n64/vi.hpp"

namespace ezy::n64 {

// The physical address map (n64brew "Memory map"):
//   0x00000000-0x007FFFFF  RDRAM (8 MB, as with the Expansion Pak)
//   0x03F00000-0x03FFFFFF  RDRAM registers
//   0x04000000-0x04001FFF  SP DMEM, IMEM          0x04040000 SP registers, 0x04080000 SP_PC
//   0x04100000 DP command   0x04300000 MI   0x04400000 VI   0x04500000 AI
//   0x04600000 PI           0x04700000 RI   0x04800000 SI
//   0x10000000-0x1FBFFFFF  cartridge domain 1 (ROM); 0x13FF0000 ISViewer debug output
//   0x1FC00000-0x1FC007BF  PIF ROM, 0x1FC007C0-0x1FC007FF PIF RAM
// Unmapped addresses read 0 and ignore writes. The RCP registers are 32 bits wide;
// 8- and 16-bit accesses to them use the word that holds the address.
class Bus final : public CpuBus {
public:
    static constexpr std::uint32_t kRdramSize = 8 * 1024 * 1024;

    Bus(Scheduler& scheduler, const Cartridge& cart, RdpMode mode = default_rdp_mode(),
        RspMode rsp_mode = default_rsp_mode());

    std::uint8_t read8(std::uint32_t paddr) override;
    std::uint16_t read16(std::uint32_t paddr) override;
    std::uint32_t read32(std::uint32_t paddr) override;
    std::uint64_t read64(std::uint32_t paddr) override;
    void write8(std::uint32_t paddr, std::uint8_t value) override;
    void write16(std::uint32_t paddr, std::uint16_t value) override;
    void write32(std::uint32_t paddr, std::uint32_t value) override;
    void write64(std::uint32_t paddr, std::uint64_t value) override;
    [[nodiscard]] std::uint32_t uncached_cycles(std::uint32_t paddr) const override;
    [[nodiscard]] bool interrupt_line() const override { return mi_.line(); }
    void prepare_memory(std::uint32_t address, std::size_t bytes, bool write) const override {
        rdp_.prepare_memory(address, bytes, write);
    }
    static_assert(RdpWorker::kRamSize == kRdramSize, "the RDP page tables must cover the RDRAM the CPU JIT indexes");
    [[nodiscard]] std::optional<jit::PageBarrierView> barrier_view() const override { return rdp_.barrier_view(); }
    void drain_rdp() const { rdp_.drain(); }
    // DMA engines touch only their own RDRAM range: wait for the RDP packets touching it (a
    // write also waits for the readers), not for all of them. Outside RDRAM: everything.
    void prepare_range(std::uint32_t address, std::uint64_t bytes, bool write) const {
        if (bytes == 0) return;
        if (address + bytes <= rdram_.size()) rdp_.prepare_memory(address, static_cast<std::size_t>(bytes), write);
        else drain_rdp();
    }
    // PI_RD_LEN (0x08) / PI_WR_LEN (0x0C) about to start a DMA of (value + 1) bytes at PI_DRAM_ADDR.
    // The cartridge -> RDRAM copy goes in blocks of up to 128 bytes that each end on an 8-byte
    // boundary (Pi::copy_to_rdram): at most 8 bytes past the length per block.
    // A cartridge -> RDRAM copy is also reported to the code watch.
    void prepare_pi_dma(std::uint32_t reg, std::uint32_t value) const {
        const std::uint32_t length = (value & 0x00FFFFFF) + 1;
        const std::uint32_t start = pi_.read(0x00) & ~7U;
        const std::uint64_t bytes = std::uint64_t{length} + 8 * (length / 128 + 2);
        prepare_range(start, bytes, reg == 0x0C);
        if (reg == 0x0C) note_dram_write(start, bytes);
    }
    // Audio DMA reads only its own buffer: wait for the RDP packets touching it, not all of them.
    void prepare_ai_read(Ai::ReadRange range) const {
        if (range.bytes == 0) return;
        if (range.address + range.bytes <= rdram_.size()) rdp_.prepare_memory(range.address, range.bytes, false);
        else drain_rdp();
    }
    // What the audio HLE does before it touches an RDRAM range (AudioHle::Access): waits
    // for the RDP packets over it and reports a write to the code watch.
    void audio_hle_access(std::uint32_t address, std::uint32_t bytes, bool write) const {
        if (address + bytes <= rdram_.size()) rdp_.prepare_memory(address, bytes, write);
        else drain_rdp();
        if (write) note_dram_write(address, bytes);
    }
    [[nodiscard]] bool rdp_threaded() const { return rdp_.threaded(); }
    [[nodiscard]] RdpWorker::Stats rdp_stats() const { return rdp_.stats(); }
    [[nodiscard]] Vi::Image image() const { drain_rdp(); return vi_.image(rdram_); }
    [[nodiscard]] const RdpHiddenBits& hidden_bits() const { return rdp_.hidden_bits(); }
    // The filtered 640-wide screen (Vi::output), after the RDP finishes its packets.
    [[nodiscard]] Vi::Image output() const { drain_rdp(); return vi_.output(rdram_, &hidden_bits()); }

    // Audio tasks of a known microcode run at a high level instead of on the RSP.
    void set_audio_hle(bool enabled) { audio_hle_ = enabled; }
    // Graphics tasks of F3DEX/F3DLX 0.95 run in C++ (gfx_hle/) instead of on the RSP. Close
    // to the microcode's results but not bit-exact, and the task takes an estimated time.
    void set_gfx_hle(bool enabled) { gfx_hle_ = enabled; }
    // Debugging: the RSP keeps running audio tasks, and each one is also run at a high
    // level on a copy of RDRAM whose written bytes are compared when the RSP stops.
    void set_audio_hle_check(bool enabled);
    struct AudioHleCheck {
        std::uint64_t tasks = 0, bytes = 0, mismatched_bytes = 0, mismatched_tasks = 0;
    };
    [[nodiscard]] AudioHleCheck audio_hle_check() const { return check_; }

    void handle_event(EventType type, std::uint64_t when);
    // The RspStep event, without the dispatch (the step loop's fast path).
    void rsp_step(std::uint64_t when) {
        rsp_.step(mi_, rdp_, scheduler_, when);
        if (shadow_) finish_shadow();
    }
    std::optional<std::uint64_t> rsp_run(std::uint64_t when, bool stop_on_line) {
        const auto result = rsp_.run(mi_, rdp_, scheduler_, when, stop_on_line);
        if (shadow_) finish_shadow();
        return result;
    }

    [[nodiscard]] Mi& mi() { return mi_; }
    [[nodiscard]] Pi& pi() { return pi_; }
    [[nodiscard]] RspRegs& rsp() { return rsp_; }
    [[nodiscard]] Rdp& rdp() { return rdp_; }
    // Raw RDRAM, for code that already waits for the RDP itself (HLE, tests). Writes through
    // it are not reported to the code watch: a writer must call note_ram_write() itself.
    [[nodiscard]] std::span<std::uint8_t> rdram() { return rdram_; }
    [[nodiscard]] Vi& vi() { return vi_; }
    [[nodiscard]] Si& si() { return si_; }
    [[nodiscard]] Ai& ai() { return ai_; }
    [[nodiscard]] Pif& pif() { return pif_; }
    // An optional real PIF ROM (1984 bytes) mapped at 0x1FC00000.
    void set_pif_rom(std::span<const std::uint8_t> rom);
    [[nodiscard]] const Vi& vi() const { return vi_; }
    // Text a program printed through the ISViewer port since the last call.
    [[nodiscard]] std::string take_isviewer_output();

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        drain_rdp();
        ar(rdram_), ar(rdram_regs_), ar(ri_), ar(isviewer_);
        set_direct_ram(rdram_.data(), static_cast<std::uint32_t>(rdram_.size()));
        if constexpr (Ar::kLoading) rdp_.bind_memory(rdram_);
        mi_.serialize(ar);
        pi_.serialize(ar);
        rsp_.serialize(ar, version, rdp_);
        vi_.serialize(ar);
        si_.serialize(ar);
        pif_.serialize(ar, version);
        ai_.serialize(ar);
        if (version >= 4) rdp_.serialize(ar, version);
    }

private:
    // A write by an engine whose RDRAM address wraps at 16 MB (PI, SP DMA, audio HLE): the
    // code watch gets the parts inside RDRAM, the wrapped one included.
    void note_dram_write(std::uint32_t address, std::uint64_t bytes) const {
        constexpr std::uint64_t kWrap = 0x1000000;
        note_ram_write(address, static_cast<std::size_t>(std::min<std::uint64_t>(bytes, kWrap)));
        if (address + bytes > kWrap) note_ram_write(0, static_cast<std::size_t>(std::min(address + bytes - kWrap, kWrap)));
    }
    // 32-bit register/ROM read and write for everything that isn't RDRAM or SP memory.
    [[nodiscard]] std::uint32_t read_io(std::uint32_t paddr);
    void write_io(std::uint32_t paddr, std::uint32_t value);
    [[nodiscard]] std::uint16_t cart_read16(std::uint32_t paddr);
    [[nodiscard]] static bool is_cart(std::uint32_t paddr);
    // Byte-addressable memory (RDRAM, SP DMEM/IMEM, ISViewer buffer), or nullptr.
    [[nodiscard]] std::uint8_t* memory(std::uint32_t paddr);
    void on_rsp_start();
    // A graphics task the HLE knows: runs it and schedules its end; false leaves it to the RSP.
    bool start_gfx_hle_task();
    void finish_shadow();

    Scheduler& scheduler_;
    const Cartridge& cart_;
    std::vector<std::uint8_t> rdram_ = std::vector<std::uint8_t>(kRdramSize, 0);
    std::array<std::uint32_t, 16> rdram_regs_{};
    std::array<std::uint32_t, 8> ri_{};
    std::array<std::uint8_t, 0x10000> isviewer_{};  // 0x13FF0000-0x13FFFFFF
    std::string isviewer_out_;
    Mi mi_;
    Pi pi_;
    RspRegs rsp_;
    Rdp rdp_;
    Vi vi_;
    Si si_;
    Pif pif_;
    Ai ai_;
    std::vector<std::uint8_t> pif_rom_;
    bool audio_hle_ = false;
    bool gfx_hle_ = false;  // a setting, not saved
    F3dex f3dex_;           // keeps nothing between tasks
    struct Shadow {
        std::vector<std::uint8_t> rdram;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;  // address, bytes
    };
    std::unique_ptr<Shadow> shadow_;     // the audio task the RSP is running, checked
    bool check_audio_hle_ = false;
    AudioHleCheck check_;
};

}  // namespace ezy::n64
