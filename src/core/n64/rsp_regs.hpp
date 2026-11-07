#pragma once

#include <array>
#include <cstdint>

#include "n64/mi.hpp"

namespace ezy::n64 {

// What the CPU sees of the RSP and RDP before they are emulated (subprojects 2 and 3):
// SP DMEM/IMEM (8 KB at 0x04000000), the SP registers (0x04040000, SP_PC at 0x04080000)
// and the DP command registers (0x04100000). The RSP stays halted: a write that would
// start it, or a DP command list, is logged once and otherwise ignored.
class RspRegs {
public:
    RspRegs();

    [[nodiscard]] std::uint8_t* mem() { return mem_.data(); }  // DMEM then IMEM
    [[nodiscard]] std::uint32_t read_sp(std::uint32_t offset);
    void write_sp(std::uint32_t offset, std::uint32_t value, Mi& mi);
    [[nodiscard]] std::uint32_t read_dp(std::uint32_t offset) const;
    void write_dp(std::uint32_t offset, std::uint32_t value);

    template <class Ar>
    void serialize(Ar& ar) {
        ar(mem_), ar(sp_), ar(sp_status_), ar(semaphore_), ar(sp_pc_), ar(dp_);
    }

private:
    std::array<std::uint8_t, 0x2000> mem_{};
    std::array<std::uint32_t, 8> sp_{};  // MEM_ADDR, DRAM_ADDR, RD_LEN, WR_LEN (indices 0-3)
    std::uint32_t sp_status_ = 1;        // halted
    std::uint32_t semaphore_ = 0;
    std::uint32_t sp_pc_ = 0;
    std::array<std::uint32_t, 8> dp_{};
};

}  // namespace ezy::n64
