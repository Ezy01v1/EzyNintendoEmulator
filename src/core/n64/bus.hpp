#pragma once

#include <array>
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

    Bus(Scheduler& scheduler, const Cartridge& cart);

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

    void handle_event(EventType type, std::uint64_t when);
    // The RspStep event, without the dispatch (the step loop's fast path).
    void rsp_step(std::uint64_t when) { rsp_.step(mi_, rdp_, scheduler_, when); }
    void set_hle_gfx(bool enabled) { hle_gfx_ = enabled; }
    std::optional<std::uint64_t> rsp_run(std::uint64_t when, bool stop_on_line) {
        return rsp_.run(mi_, rdp_, scheduler_, when, stop_on_line);
    }

    [[nodiscard]] Mi& mi() { return mi_; }
    [[nodiscard]] Pi& pi() { return pi_; }
    [[nodiscard]] RspRegs& rsp() { return rsp_; }
    [[nodiscard]] Vi& vi() { return vi_; }
    [[nodiscard]] Si& si() { return si_; }
    [[nodiscard]] Ai& ai() { return ai_; }
    [[nodiscard]] Pif& pif() { return pif_; }
    // An optional real PIF ROM (1984 bytes) mapped at 0x1FC00000.
    void set_pif_rom(std::span<const std::uint8_t> rom);
    [[nodiscard]] Rdp& rdp() { return rdp_; }
    [[nodiscard]] const Vi& vi() const { return vi_; }
    [[nodiscard]] std::span<std::uint8_t> rdram() { return rdram_; }
    [[nodiscard]] std::span<const std::uint8_t> rdram() const { return rdram_; }
    // Text a program printed through the ISViewer port since the last call.
    [[nodiscard]] std::string take_isviewer_output();

    template <class Ar>
    void serialize(Ar& ar, std::uint32_t version) {
        ar(rdram_), ar(rdram_regs_), ar(ri_), ar(isviewer_);
        set_direct_ram(rdram_.data(), static_cast<std::uint32_t>(rdram_.size()));
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
    // 32-bit register/ROM read and write for everything that isn't RDRAM or SP memory.
    [[nodiscard]] std::uint32_t read_io(std::uint32_t paddr);
    void write_io(std::uint32_t paddr, std::uint32_t value);
    [[nodiscard]] std::uint16_t cart_read16(std::uint32_t paddr);
    [[nodiscard]] static bool is_cart(std::uint32_t paddr);
    // Byte-addressable memory (RDRAM, SP DMEM/IMEM, ISViewer buffer), or nullptr.
    [[nodiscard]] std::uint8_t* memory(std::uint32_t paddr);

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
    F3dex f3dex_;
    bool hle_gfx_ = false;  // a setting, not saved
    // The RSP was just started: runs the task in C++ when it is a graphics task the HLE knows.
    bool start_hle_task(std::uint64_t when);
    Vi vi_;
    Si si_;
    Pif pif_;
    Ai ai_;
    std::vector<std::uint8_t> pif_rom_;
};

}  // namespace ezy::n64
