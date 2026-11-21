#include "n64/vi_filter.hpp"

#include <array>

namespace ezy::n64::vi {
const std::array<std::uint8_t, 16384> kGammaTable = [] {
    std::array<std::uint8_t, 16384> table{};
    unsigned root = 0;
    for (unsigned i = 0; i < table.size(); ++i) {
        while ((root + 1) * (root + 1) <= i) ++root;
        table[i] = static_cast<std::uint8_t>(2 * root);
    }
    return table;
}();

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
    Color rows[3][5];
    for (int dy = 0; dy < 3; ++dy)
        for (int dx = 0; dx < 5; ++dx) rows[dy][dx] = fetch(fb, x + dx - 2, y + dy - 1);
    return filter(rows[0] + 2, rows[1] + 2, rows[2] + 2, dither_filter);
}

} // namespace ezy::n64::vi
