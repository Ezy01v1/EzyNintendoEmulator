#include "n64/gfx_hle/task.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace ezy::n64 {

OsTask read_os_task(std::span<const std::uint8_t, 0x1000> dmem) {
    std::array<std::uint32_t, 16> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::size_t at = 0xFC0 + i * 4;
        fields[i] = (std::uint32_t{dmem[at]} << 24) | (std::uint32_t{dmem[at + 1]} << 16) |
                    (std::uint32_t{dmem[at + 2]} << 8) | dmem[at + 3];
    }
    return {fields[0], fields[1], fields[2],  fields[3],  fields[4],  fields[5],  fields[6],  fields[7],
            fields[8], fields[9], fields[10], fields[11], fields[12], fields[13], fields[14], fields[15]};
}

GfxUcode identify_gfx_ucode(std::uint32_t ucode_data, std::uint32_t ucode_data_size,
                            std::span<const std::uint8_t> rdram) {
    const std::size_t start = std::min<std::size_t>(ucode_data & 0xFFFFFF, rdram.size());
    const std::size_t end = std::min<std::size_t>(start + ucode_data_size, rdram.size());
    const std::string_view data(reinterpret_cast<const char*>(rdram.data() + start), end - start);
    constexpr std::string_view kF3dex = "RSP Gfx ucode F3DEX         0.95";
    constexpr std::string_view kF3dlx = "RSP Gfx ucode F3DLX         0.95";
    if (data.find(kF3dex) != std::string_view::npos) return GfxUcode::F3dex095;
    if (data.find(kF3dlx) != std::string_view::npos) return GfxUcode::F3dlx095;
    return GfxUcode::Unknown;
}

}  // namespace ezy::n64
