#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "n64/gfx_hle/task.hpp"
#include "n64/mi.hpp"
#include "n64/rdp.hpp"

namespace ezy::n64 {

// GBI 1 / F3DEX 0.95 display-list opcodes (the high byte of w0).
namespace gbi {
inline constexpr unsigned G_SPNOOP = 0x00, G_MTX = 0x01, G_MOVEMEM = 0x03, G_VTX = 0x04, G_DL = 0x06,
                          G_LOAD_UCODE = 0xAF, G_TRI2 = 0xB1, G_RDPHALF_2 = 0xB3, G_RDPHALF_1 = 0xB4,
                          G_CLEARGEOMETRYMODE = 0xB6, G_SETGEOMETRYMODE = 0xB7, G_ENDDL = 0xB8,
                          G_SETOTHERMODE_L = 0xB9, G_SETOTHERMODE_H = 0xBA, G_TEXTURE = 0xBB,
                          G_MOVEWORD = 0xBC, G_POPMTX = 0xBD, G_CULLDL = 0xBE, G_TRI1 = 0xBF,
                          G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5, G_SETTIMG = 0xFD, G_SETZIMG = 0xFE,
                          G_SETCIMG = 0xFF;
inline constexpr unsigned G_MW_SEGMENT = 0x06;
}  // namespace gbi

// High-level emulation of the F3DEX 0.95 / F3DLX 0.95 graphics microcodes: walks a task's
// display list in RDRAM and hands the RDP the commands the microcode would have sent.
class F3dex {
public:
    struct Result {
        std::uint32_t work;  // an estimate of the RSP's effort, for the task's duration
        bool ok;             // false: the list ended on an error (bad address, depth, unknown ucode)
    };

    Result run_task(const OsTask& task, GfxUcode ucode, std::span<std::uint8_t> rdram, Rdp& rdp, Mi& mi);

    // A segmented address (segment in bits 27:24) made physical.
    [[nodiscard]] std::uint32_t segment_address(std::uint32_t address) const {
        return (segments_[(address >> 24) & 15] + (address & 0xFFFFFF)) & 0xFFFFFF;
    }

private:
    static constexpr unsigned kDlStackDepth = 10;

    // One display-list command; false ends the task with an error.
    bool execute(std::uint32_t w0, std::uint32_t w1, std::span<std::uint8_t> rdram);
    void emit(std::uint64_t word) { out_.push_back(word); }

    GfxUcode ucode_ = GfxUcode::Unknown;
    std::array<std::uint32_t, 16> segments_{};
    std::array<std::uint32_t, kDlStackDepth> dl_stack_{};
    unsigned dl_depth_ = 0;
    std::uint32_t pc_ = 0;
    bool ended_ = false;
    std::uint32_t rdp_half_1_ = 0, rdp_half_2_ = 0;
    // Set Other Modes as the microcode keeps it (H: 24 bits); every task starts from its defaults.
    static constexpr std::uint32_t kInitialOtherModeH = 0x080CFF;
    std::uint32_t other_mode_h_ = kInitialOtherModeH, other_mode_l_ = 0;
    std::vector<std::uint64_t> out_;  // RDP words not yet handed over
};

}  // namespace ezy::n64
