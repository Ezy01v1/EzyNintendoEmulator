#pragma once

#include <algorithm>
#include <cstdint>
#include "n64/rdp_worker.hpp"

namespace ezy::n64 {

// Producer-only shadow. Bounds overestimate the rasterizer's memory footprint,
// including pixels whose X coordinate is larger than the framebuffer stride.
struct RdpMemoryState {
    std::uint32_t color_address = 0, width = 0, pixel_size = 0, depth_address = 0;
    std::uint64_t scissor = 0, modes = 0;

    void apply(std::uint64_t word) {
        switch ((word >> 56) & 63) {
            case 0x2D: scissor = word; break;
            case 0x2F: modes = word; break;
            case 0x3E: depth_address = static_cast<std::uint32_t>(word) & 0x3FFFFFF; break;
            case 0x3F:
                color_address = static_cast<std::uint32_t>(word) & 0x3FFFFFF;
                width = (static_cast<std::uint32_t>(word >> 32) & 0x3FF) + 1;
                pixel_size = static_cast<std::uint32_t>(word >> 51) & 3;
                break;
            default: break;
        }
    }

    [[nodiscard]] RdpAccess access(const RdpPacket& packet) const {
        constexpr std::uint32_t ram_size = 8 * 1024 * 1024;
        const unsigned op = static_cast<unsigned>(packet.words[0] >> 56) & 63;
        if (op == 0x30 || op == 0x33 || op == 0x34) return RdpAccess::range(0, ram_size, false);
        RdpAccess result;
        if (!((op >= 0x08 && op <= 0x0F) || op == 0x24 || op == 0x36)) return result;
        const auto right = static_cast<unsigned>(scissor >> 12) & 0xFFF;
        const auto bottom = (static_cast<unsigned>(scissor) & 0xFFF) + 3;
        // COPY and fill triangles include the integer right edge, unlike the
        // fractional coverage paths. Reserve that extra pixel for every draw.
        const std::uint64_t x_end = (right >> 2) + 1, y_end = bottom >> 2;
        if (x_end == 0 || y_end == 0 || width == 0) return result;
        const auto pixels = (y_end - 1) * width + x_end;
        const unsigned bytes = pixel_size >= 1 ? 1U << (pixel_size - 1) : 4;
        const auto add = [&](std::uint32_t address, unsigned bpp) {
            const auto begin = address & ~(bpp - 1U);
            const auto end = std::min(std::uint64_t{address} + pixels * bpp, std::uint64_t{ram_size});
            if (begin < end) result.ranges[result.count++] = {begin, static_cast<std::uint32_t>(end), true};
        };
        add(color_address, bytes);
        if (modes & ((1U << 4) | (1U << 5))) add(depth_address, 2);
        return result;
    }
};

} // namespace ezy::n64
