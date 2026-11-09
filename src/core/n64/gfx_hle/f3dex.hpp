#pragma once

#include <cstdint>
#include <span>

#include "n64/gfx_hle/task.hpp"
#include "n64/mi.hpp"
#include "n64/rdp.hpp"

namespace ezy::n64 {

// High-level emulation of the F3DEX 0.95 / F3DLX 0.95 graphics microcodes: walks a task's
// display list in RDRAM and hands the RDP the commands the microcode would have sent.
class F3dex {
public:
    struct Result {
        std::uint32_t work;  // an estimate of the RSP's effort, for the task's duration
        bool ok;             // false: the list ended on an error (bad address, depth, unknown ucode)
    };

    Result run_task(const OsTask& task, GfxUcode ucode, std::span<std::uint8_t> rdram, Rdp& rdp, Mi& mi);
};

}  // namespace ezy::n64
