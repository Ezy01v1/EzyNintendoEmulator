#include "n64/rsp_regs.hpp"

#include <cstdio>

namespace ezy::n64 {

namespace {

constexpr std::uint32_t kHalt = 1U << 0, kBroke = 1U << 1, kSingleStep = 1U << 5, kInterruptOnBreak = 1U << 6;

void log_once(bool& done, const char* what) {
    if (done) return;
    done = true;
    std::fprintf(stderr, "N64: %s (not emulated yet, ignored)\n", what);
}

bool g_logged_rsp_start = false;
bool g_logged_dp = false;

}  // namespace

RspRegs::RspRegs() { dp_[3] = 0x80; }  // DPC_STATUS: command buffer ready

std::uint32_t RspRegs::read_sp(std::uint32_t offset) {
    if (offset >= 0x40000) return sp_pc_;  // SP_PC (0x04080000)
    switch (offset & 0x1F) {
        case 0x10: return sp_status_;
        case 0x14: return 0;  // SP_DMA_FULL
        case 0x18: return 0;  // SP_DMA_BUSY
        case 0x1C: {          // SP_SEMAPHORE: reading takes it
            const std::uint32_t value = semaphore_;
            semaphore_ = 1;
            return value;
        }
        default: return sp_[(offset & 0x1F) / 4];
    }
}

void RspRegs::write_sp(std::uint32_t offset, std::uint32_t value, Mi& mi) {
    if (offset >= 0x40000) {
        sp_pc_ = value & 0xFFC;
        return;
    }
    switch (offset & 0x1F) {
        case 0x10:  // SP_STATUS: pairs of clear/set bits (n64brew "RSP", SP_STATUS write)
            if (value & (1U << 0)) log_once(g_logged_rsp_start, "a program started the RSP");
            if (value & (1U << 1)) sp_status_ |= kHalt;
            if (value & (1U << 2)) sp_status_ &= ~kBroke;
            if (value & (1U << 3)) mi.clear(MiInterrupt::Sp);
            if (value & (1U << 4)) mi.raise(MiInterrupt::Sp);
            if (value & (1U << 5)) sp_status_ &= ~kSingleStep;
            if (value & (1U << 6)) sp_status_ |= kSingleStep;
            if (value & (1U << 7)) sp_status_ &= ~kInterruptOnBreak;
            if (value & (1U << 8)) sp_status_ |= kInterruptOnBreak;
            for (unsigned s = 0; s < 8; ++s) {  // signals 0-7: status bits 7-14
                if (value & (1U << (9 + 2 * s))) sp_status_ &= ~(1U << (7 + s));
                if (value & (1U << (10 + 2 * s))) sp_status_ |= 1U << (7 + s);
            }
            return;
        case 0x1C: semaphore_ = 0; return;
        case 0x14: case 0x18: return;  // read-only
        default:
            sp_[(offset & 0x1F) / 4] = value;
            return;
    }
}

std::uint32_t RspRegs::read_dp(std::uint32_t offset) const { return dp_[(offset & 0x1F) / 4]; }

void RspRegs::write_dp(std::uint32_t offset, std::uint32_t value) {
    const std::uint32_t reg = (offset & 0x1F) / 4;
    if (reg <= 1) {  // DPC_START, DPC_END
        log_once(g_logged_dp, "a program sent RDP commands");
        dp_[reg] = value & 0xFFFFF8;
        if (reg == 1) dp_[2] = dp_[1];  // nothing to run: CURRENT catches up
    }
}

}  // namespace ezy::n64
