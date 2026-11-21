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
    std::uint64_t texture_image = 0;

    void apply(std::uint64_t word) {
        switch ((word >> 56) & 63) {
            case 0x2D: scissor = word; break;
            case 0x2F: modes = word; break;
            case 0x3D: texture_image = word; break;
            case 0x3E: depth_address = static_cast<std::uint32_t>(word) & 0x3FFFFFF; break;
            case 0x3F:
                color_address = static_cast<std::uint32_t>(word) & 0x3FFFFFF;
                width = (static_cast<std::uint32_t>(word >> 32) & 0x3FF) + 1;
                pixel_size = static_cast<std::uint32_t>(word >> 51) & 3;
                break;
            default: break;
        }
    }

    // Texture loads read whole 64-bit words starting at the first source texel, so the
    // bound covers one extra word past the last texel. Anything that could wrap the
    // 24-bit address falls back to reserving all of RDRAM.
    [[nodiscard]] RdpAccess load_access(std::uint64_t word, unsigned op) const {
        constexpr std::uint32_t ram_size = 8 * 1024 * 1024;
        const std::uint64_t image = static_cast<std::uint32_t>(texture_image) & 0xFFFFFF;
        const std::uint64_t image_width = (static_cast<std::uint32_t>(texture_image >> 32) & 0x3FF) + 1;
        const unsigned size = static_cast<unsigned>(texture_image >> 51) & 3;
        const auto field = [word](unsigned shift) { return std::uint64_t{(word >> shift) & 0xFFF}; };
        const std::uint64_t sl = field(44), tl = field(32), sh = field(12), th = field(0);
        std::uint64_t first = 0, last = 0, bytes = 1ULL << (size == 0 ? 0 : size - 1);
        if (op == 0x30) {  // Load TLUT: 16-bit entries on one row
            bytes = 2;
            first = (tl >> 2) * image_width + (sl >> 2);
            last = (tl >> 2) * image_width + (sh >> 2);
        } else if (op == 0x33) {  // Load Block: a run of texels from (sl, tl)
            first = tl * image_width + sl;
            last = first + ((sh - sl + 1) & 0xFFF);
        } else {  // Load Tile: rows top..bottom, texels left..right
            first = (tl >> 2) * image_width + (sl >> 2);
            last = (th >> 2) * image_width + (sh >> 2);
        }
        if (last < first) return {};  // the rasterizer rejects inverted rectangles
        const std::uint64_t begin = image + first * bytes;
        const std::uint64_t end = image + (last + 8) * bytes + 8;
        if (end > 0x1000000) return RdpAccess::range(0, ram_size, false);
        if (begin >= ram_size) return {};
        return RdpAccess::range(static_cast<std::uint32_t>(begin) & ~7U,
                                static_cast<std::uint32_t>(std::min<std::uint64_t>(end, ram_size)), false);
    }

    [[nodiscard]] RdpAccess access(const RdpPacket& packet) const {
        constexpr std::uint32_t ram_size = 8 * 1024 * 1024;
        const unsigned op = static_cast<unsigned>(packet.words[0] >> 56) & 63;
        if (op == 0x30 || op == 0x33 || op == 0x34) return load_access(packet.words[0], op);
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
