#pragma once

#include "n64/rdp_hidden_bits.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

// The N64 VI's output filter stages as pure functions over an RDRAM image: coverage fetch,
// antialias, dither filter, divot, interpolation and gamma. Vi::output() chains them. The
// stages it runs per pixel are inline here.
// The per-pixel stages must inline into Vi::output's loops: MSVC otherwise keeps some calls.
#if defined(_MSC_VER)
#define EZY_VI_INLINE __forceinline
#else
#define EZY_VI_INLINE inline __attribute__((always_inline))
#endif

namespace ezy::n64::vi {

struct Color { std::uint8_t r = 0, g = 0, b = 0; std::uint8_t coverage = 7; };

// A view of the image VI_ORIGIN points at.
struct Framebuffer {
    std::span<const std::uint8_t> rdram;
    const RdpHiddenBits* hidden = nullptr; // nullptr: hidden bits read as 0
    std::uint32_t origin = 0, width = 0;   // VI_ORIGIN & 0xFFFFFF, VI_WIDTH & 0xFFF (pixels)
    bool rgba32 = false;
    bool use_coverage = true;              // AA_MODE 0 or 1; otherwise coverage reads as 7
};

// Pixel (x, y) with its coverage; outside RDRAM: black with coverage 7.
Color fetch(const Framebuffer& fb, int x, int y);
// Antialias (coverage < 7) or dither filter (coverage == 7 and dither_filter), as the hardware's fetch does.
Color fetch_filtered(const Framebuffer& fb, int x, int y, bool dither_filter);
// 2 * isqrt(i) for every gamma input (an 8-bit channel << 6 plus 6 dither bits): 0..254.
extern const std::array<std::uint8_t, 16384> kGammaTable;

namespace detail {

// What the antialias formula needs of one channel: the second smallest and second largest
// of the centre and the full-coverage neighbours (the hardware sorts them; this scans).
struct Pen {
    int min1 = 256, min2 = 256, max1 = -1, max2 = -1;
    void add(int v) {
        if (v < min1) { min2 = min1; min1 = v; } else if (v < min2) { min2 = v; }
        if (v > max1) { max2 = max1; max1 = v; } else if (v > max2) { max2 = v; }
    }
};

inline std::uint8_t antialias(std::uint32_t c, std::uint32_t penmin, std::uint32_t penmax, std::uint32_t coverage) {
    return static_cast<std::uint8_t>((((((penmin + penmax - 2 * c) * (7u - coverage)) + 4) >> 3) + c) & 0xFF);
}

// +1, -1 or 0 as the neighbour's top five bits are above, below or equal to the centre's
// (branchless: the dither noise makes the comparisons unpredictable).
EZY_VI_INLINE int compare5(std::uint8_t neighbour, std::uint8_t centre) {
    const unsigned nv = neighbour >> 3u, cv = centre >> 3u;
    return static_cast<int>(nv > cv) - static_cast<int>(nv < cv);
}

}  // namespace detail

namespace detail {

// Antialias: the centre plus the full-coverage pixels of a 6-neighbour pattern.
inline Color antialias_pixel(const Color* above, const Color* mid, const Color* below) {
    const Color center = mid[0];
    const Color* const neighbours[6] = {above - 1, above + 1, mid - 2, mid + 2, below - 1, below + 1};
    Pen r, g, b;
    r.add(center.r), g.add(center.g), b.add(center.b);
    unsigned count = 0;
    for (const Color* n : neighbours) {
        if (n->coverage != 7) continue;
        r.add(n->r), g.add(n->g), b.add(n->b);
        ++count;
    }
    if (count < 2) {  // penmin = penmax = centre: the formula gives the centre back
        r.min2 = r.max2 = center.r, g.min2 = g.max2 = center.g, b.min2 = b.max2 = center.b;
    }
    Color out = center;
    out.r = antialias(center.r, static_cast<std::uint32_t>(r.min2), static_cast<std::uint32_t>(r.max2), center.coverage);
    out.g = antialias(center.g, static_cast<std::uint32_t>(g.min2), static_cast<std::uint32_t>(g.max2), center.coverage);
    out.b = antialias(center.b, static_cast<std::uint32_t>(b.min2), static_cast<std::uint32_t>(b.max2), center.coverage);
    return out;
}

// Dither filter: each channel moves by one toward each of the 8 neighbours that differ in
// its top five bits.
EZY_VI_INLINE Color dither_pixel(const Color* above, const Color* mid, const Color* below) {
    const Color center = mid[0];
    int dr = 0, dg = 0, db = 0;
    const auto add = [&](const Color& n) {
        dr += compare5(n.r, center.r);
        dg += compare5(n.g, center.g);
        db += compare5(n.b, center.b);
    };
    add(above[-1]), add(above[0]), add(above[1]), add(mid[-1]), add(mid[1]), add(below[-1]), add(below[0]), add(below[1]);
    Color out = center;
    out.r = static_cast<std::uint8_t>((center.r + dr) & 0xFF);
    out.g = static_cast<std::uint8_t>((center.g + dg) & 0xFF);
    out.b = static_cast<std::uint8_t>((center.b + db) & 0xFF);
    return out;
}

}  // namespace detail

// fetch_filtered's stage on pixels already fetched: above, mid and below point at the
// centre's column in the rows y - 1, y and y + 1 (columns -2..+2 of mid and -1..+1 of the
// others are read).
inline Color filter(const Color* above, const Color* mid, const Color* below, bool dither_filter) {
    if (mid->coverage < 7) return detail::antialias_pixel(above, mid, below);
    if (dither_filter) return detail::dither_pixel(above, mid, below);
    return *mid;
}

EZY_VI_INLINE Color divot(Color center, Color left, Color right) {
    if (center.coverage == 7 && left.coverage == 7 && right.coverage == 7) return center;
    const auto median = [](std::uint8_t a, std::uint8_t b, std::uint8_t c) {
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    };
    Color out = center;
    out.r = median(left.r, center.r, right.r);
    out.g = median(left.g, center.g, right.g);
    out.b = median(left.b, center.b, right.b);
    return out;
}

// fraction 0..31
EZY_VI_INLINE Color lerp(Color a, Color b, unsigned fraction) {
    if (fraction == 0) return a;
    const auto mix = [fraction](int av, int bv) {
        return static_cast<std::uint8_t>(((((bv - av) * static_cast<int>(fraction) + 16) >> 5) + av) & 0xFF);
    };
    Color out = a;
    out.r = mix(a.r, b.r);
    out.g = mix(a.g, b.g);
    out.b = mix(a.b, b.b);
    return out;
}

// Gamma (CTRL bit 3) and gamma dither (bit 2) with the noise for this pixel.
EZY_VI_INLINE Color gamma(Color c, bool gamma, bool gamma_dither, std::uint32_t noise) {
    if (!gamma && !gamma_dither) return c;
    Color out = c;
    if (!gamma) {  // dither only: add one noise bit per channel, except to 255
        const auto add = [noise](std::uint8_t v, unsigned bit) {
            return static_cast<std::uint8_t>(v + ((noise >> bit) & static_cast<unsigned>(v != 255)));
        };
        out.r = add(c.r, 0), out.g = add(c.g, 1), out.b = add(c.b, 2);
        return out;
    }
    const unsigned dr = gamma_dither ? noise & 0x3Fu : 0u;
    const unsigned dg = gamma_dither ? (noise >> 6) & 0x3Fu : 0u;
    const unsigned db = gamma_dither ? ((noise >> 9) & 0x38u) | (noise & 7u) : 0u;
    out.r = kGammaTable[(unsigned{c.r} << 6) | dr];
    out.g = kGammaTable[(unsigned{c.g} << 6) | dg];
    out.b = kGammaTable[(unsigned{c.b} << 6) | db];
    return out;
}

} // namespace ezy::n64::vi
