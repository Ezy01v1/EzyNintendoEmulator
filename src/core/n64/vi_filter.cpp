#include "n64/vi_filter.hpp"

#include <algorithm>

namespace ezy::n64::vi {
namespace {

constexpr unsigned kChannels = 3;
std::uint8_t& channel(Color& c, unsigned i) { return i == 0 ? c.r : i == 1 ? c.g : c.b; }
std::uint8_t channel(const Color& c, unsigned i) { return i == 0 ? c.r : i == 1 ? c.g : c.b; }

unsigned isqrt(unsigned value) {
    unsigned root = 0;
    while ((root + 1) * (root + 1) <= value) ++root;
    return root;
}

std::uint8_t median(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

} // namespace

Color fetch(const Framebuffer& fb, int x, int y) {
    const unsigned bytes = fb.rgba32 ? 4 : 2;
    const std::int64_t index = static_cast<std::int64_t>(y) * fb.width + x;
    const std::int64_t address = static_cast<std::int64_t>(fb.origin) + index * bytes;
    if (address < 0 || address + bytes > static_cast<std::int64_t>(fb.rdram.size())) return Color{};
    const auto* p = fb.rdram.data() + address;
    Color c;
    if (fb.rgba32) {
        c.r = p[0]; c.g = p[1]; c.b = p[2];
        c.coverage = fb.use_coverage ? static_cast<std::uint8_t>((p[3] >> 5) & 7) : 7;
    } else {
        const unsigned pixel = (static_cast<unsigned>(p[0]) << 8) | p[1];
        c.r = static_cast<std::uint8_t>(((pixel >> 11) & 31) << 3);
        c.g = static_cast<std::uint8_t>(((pixel >> 6) & 31) << 3);
        c.b = static_cast<std::uint8_t>(((pixel >> 1) & 31) << 3);
        if (fb.use_coverage) {
            const unsigned hidden = fb.hidden ? fb.hidden->read(static_cast<std::size_t>(address >> 1)) : 0;
            c.coverage = static_cast<std::uint8_t>(((pixel & 1) << 2) | (hidden & 3));
        }
    }
    return c;
}

Color fetch_filtered(const Framebuffer& fb, int x, int y, bool dither_filter) {
    const Color center = fetch(fb, x, y);
    if (center.coverage < 7) {
        // Antialias: the centre plus the full-coverage pixels of a 6-neighbour pattern.
        static constexpr int kOffsets[6][2] = {{-1, -1}, {1, -1}, {-2, 0}, {2, 0}, {-1, 1}, {1, 1}};
        std::uint8_t values[kChannels][7];
        unsigned count = 0;
        for (const auto& offset : kOffsets) {
            const Color n = fetch(fb, x + offset[0], y + offset[1]);
            if (n.coverage != 7) continue;
            for (unsigned i = 0; i < kChannels; ++i) values[i][count] = channel(n, i);
            ++count;
        }
        Color out = center;
        for (unsigned i = 0; i < kChannels; ++i) {
            const std::uint32_t c = channel(center, i);
            std::uint32_t penmin = c, penmax = c;
            if (count >= 2) {
                values[i][count] = static_cast<std::uint8_t>(c);
                std::sort(values[i], values[i] + count + 1);
                penmin = values[i][1];
                penmax = values[i][count - 1];
            }
            const std::uint32_t result = ((((penmin + penmax - 2 * c) * (7u - center.coverage)) + 4) >> 3) + c;
            channel(out, i) = static_cast<std::uint8_t>(result & 0xFF);
        }
        return out;
    }
    if (dither_filter) {
        static constexpr int kOffsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 1}, {0, 1}, {1, 1}, {-1, 0}, {1, 0}};
        Color out = center;
        int delta[kChannels] = {0, 0, 0};
        for (const auto& offset : kOffsets) {
            const Color n = fetch(fb, x + offset[0], y + offset[1]);
            for (unsigned i = 0; i < kChannels; ++i) {
                const unsigned nv = channel(n, i) >> 3, cv = channel(center, i) >> 3;
                delta[i] += nv > cv ? 1 : nv < cv ? -1 : 0;
            }
        }
        for (unsigned i = 0; i < kChannels; ++i)
            channel(out, i) = static_cast<std::uint8_t>((channel(center, i) + delta[i]) & 0xFF);
        return out;
    }
    return center;
}

Color divot(Color center, Color left, Color right) {
    if (center.coverage == 7 && left.coverage == 7 && right.coverage == 7) return center;
    Color out = center;
    out.r = median(left.r, center.r, right.r);
    out.g = median(left.g, center.g, right.g);
    out.b = median(left.b, center.b, right.b);
    return out;
}

Color lerp(Color a, Color b, unsigned fraction) {
    if (fraction == 0) return a;
    Color out = a;
    for (unsigned i = 0; i < kChannels; ++i) {
        const int av = channel(a, i), bv = channel(b, i);
        const int v = (((bv - av) * static_cast<int>(fraction) + 16) >> 5) + av;
        channel(out, i) = static_cast<std::uint8_t>(v & 0xFF);
    }
    return out;
}

Color gamma(Color c, bool gamma, bool gamma_dither, std::uint32_t noise) {
    if (!gamma && !gamma_dither) return c;
    const unsigned dither[kChannels] = {noise & 0x3Fu, (noise >> 6) & 0x3Fu, ((noise >> 9) & 0x38u) | (noise & 7u)};
    Color out = c;
    for (unsigned i = 0; i < kChannels; ++i) {
        const unsigned v = channel(c, i);
        unsigned result;
        if (!gamma) result = v == 255 ? v : v + ((noise >> i) & 1u);
        else result = 2 * isqrt((v << 6) | (gamma_dither ? dither[i] : 0u));
        channel(out, i) = static_cast<std::uint8_t>(result & 0xFF);
    }
    return out;
}

} // namespace ezy::n64::vi
