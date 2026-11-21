#pragma once

#include <cstdint>
#include <span>

namespace ezy::n64 {

// libultra's OSTask, which the CPU leaves at the end of DMEM (0xFC0) before starting the RSP.
struct OsTask {
    std::uint32_t type, flags, ucode_boot, ucode_boot_size, ucode, ucode_size, ucode_data, ucode_data_size,
        dram_stack, dram_stack_size, output_buff, output_buff_size, data_ptr, data_size, yield_data_ptr,
        yield_data_size;
};

inline constexpr std::uint32_t kOsTaskGraphics = 1, kOsTaskAudio = 2;

[[nodiscard]] OsTask read_os_task(std::span<const std::uint8_t, 0x1000> dmem);

// The graphics microcodes the HLE implements.
enum class GfxUcode : std::uint8_t { Unknown, F3dex095, F3dlx095 };

// Finds the microcode's version string in its data segment ([ucode_data, + ucode_data_size)
// in RDRAM, clamped to RDRAM).
[[nodiscard]] GfxUcode identify_gfx_ucode(std::uint32_t ucode_data, std::uint32_t ucode_data_size,
                                          std::span<const std::uint8_t> rdram);
[[nodiscard]] inline GfxUcode identify_gfx_ucode(const OsTask& task, std::span<const std::uint8_t> rdram) {
    return identify_gfx_ucode(task.ucode_data, task.ucode_data_size, rdram);
}

}  // namespace ezy::n64
