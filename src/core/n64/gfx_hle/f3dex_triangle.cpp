// F3DEX HLE: triangle setup. Turns three screen vertices into an RDP triangle command
// (edges, then optional shade, texture and depth coefficients). Computed at full precision:
// close to, but not bit-for-bit, what the microcode's fixed-point sequence produces.
#include <algorithm>
#include <array>
#include <cmath>

#include "n64/gfx_hle/f3dex.hpp"

namespace ezy::n64 {

using namespace gbi;

namespace {

constexpr std::uint32_t kTexturePerspective = 1U << 19;  // G_TP_PERSP in other mode H
constexpr double kZScale = 32.0;                         // screen Z (0..1023) to the RDP's 0..0x7FFF

std::int64_t to_fixed(double value) {
    return static_cast<std::int64_t>(std::trunc(value * 65536.0));  // toward zero, like the RSP's products
}

// Four attributes as the RDP takes them: integer halves of value and DxD? in one word, the
// fractional halves in another.
std::uint64_t pack_int(const std::array<std::int64_t, 4>& v) {
    std::uint64_t out = 0;
    for (const auto x : v) out = (out << 16) | (static_cast<std::uint64_t>(x >> 16) & 0xFFFF);
    return out;
}

std::uint64_t pack_frac(const std::array<std::int64_t, 4>& v) {
    std::uint64_t out = 0;
    for (const auto x : v) out = (out << 16) | (static_cast<std::uint64_t>(x) & 0xFFFF);
    return out;
}

std::uint64_t edge_word(double x, double slope) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(to_fixed(x))) << 32) |
           static_cast<std::uint32_t>(to_fixed(slope));
}

}  // namespace

void F3dex::draw_triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    const ScreenVertex* v[3] = {&a, &b, &c};
    ++triangles_drawn_;

    // Winding in screen space (Y down): negative area is counter-clockwise in view, the front.
    const double area = (v[1]->px - v[0]->px) * (v[2]->py - v[0]->py) - (v[2]->px - v[0]->px) * (v[1]->py - v[0]->py);
    if (area == 0) return;
    if ((geometry_mode_ & G_CULL_BACK) && area > 0) return;
    if ((geometry_mode_ & G_CULL_FRONT) && area < 0) return;

    // Top, middle, bottom by Y (stable for equal Y).
    std::sort(std::begin(v), std::end(v), [](const ScreenVertex* a, const ScreenVertex* b) { return a->py < b->py; });
    const ScreenVertex &h = *v[0], &m = *v[1], &l = *v[2];
    const double xh = h.px, yh = h.py, xm = m.px, ym = m.py, xl = l.px, yl = l.py;

    const double dxhdy = yl != yh ? (xl - xh) / (yl - yh) : 0;
    const double dxmdy = ym != yh ? (xm - xh) / (ym - yh) : 0;
    const double dxldy = yl != ym ? (xl - xm) / (yl - ym) : 0;
    // Major edge on the left when the middle vertex lies to its right.
    const bool major_left = (xm - xh) * (yl - yh) - (xl - xh) * (ym - yh) > 0;

    // The RDP walks from the start of YH's scanline; XL starts at YM itself.
    const double y_start = (h.y & ~3) / 4.0;
    const double x_major = xh + dxhdy * (y_start - yh);
    const double x_minor = xh + dxmdy * (y_start - yh);

    const bool shade = (geometry_mode_ & G_SHADE) != 0;
    const bool texture = (texture_ & 1) != 0;
    const bool depth = (geometry_mode_ & G_ZBUFFER) != 0;
    const unsigned op = 0x08 | (shade ? 4 : 0) | (texture ? 2 : 0) | (depth ? 1 : 0);
    const unsigned level = (texture_ >> 11) & 7, tile = (texture_ >> 8) & 7;

    emit((std::uint64_t{0xC0U | op} << 56) | (std::uint64_t{major_left} << 55) | (std::uint64_t{level} << 51) |
         (std::uint64_t{tile} << 48) | (static_cast<std::uint64_t>(l.y & 0x3FFF) << 32) |
         (static_cast<std::uint64_t>(m.y & 0x3FFF) << 16) | static_cast<std::uint64_t>(h.y & 0x3FFF));
    emit(edge_word(xm + dxldy * (m.y / 4.0 - ym), dxldy));  // XL at the header's (rounded) YM
    emit(edge_word(x_major, dxhdy));
    emit(edge_word(x_minor, dxmdy));

    // Plane gradients of a per-vertex attribute (in screen pixels), and its value where the
    // RDP starts: on the major edge at the start scanline.
    const double det = (xm - xh) * (yl - yh) - (xl - xh) * (ym - yh);
    struct Gradient {
        std::int64_t value, dx, de, dy;
    };
    const auto gradient = [&](double a_h, double a_m, double a_l) -> Gradient {
        const double dadx = det != 0 ? ((a_m - a_h) * (yl - yh) - (a_l - a_h) * (ym - yh)) / det : 0;
        const double dady = det != 0 ? ((a_l - a_h) * (xm - xh) - (a_m - a_h) * (xl - xh)) / det : 0;
        const double dade = dady + dadx * dxhdy;
        return {to_fixed(a_h + dade * (y_start - yh)), to_fixed(dadx), to_fixed(dade), to_fixed(dady)};
    };
    const auto emit_block = [&](const std::array<Gradient, 4>& g) {
        emit(pack_int({g[0].value, g[1].value, g[2].value, g[3].value}));
        emit(pack_int({g[0].dx, g[1].dx, g[2].dx, g[3].dx}));
        emit(pack_frac({g[0].value, g[1].value, g[2].value, g[3].value}));
        emit(pack_frac({g[0].dx, g[1].dx, g[2].dx, g[3].dx}));
        emit(pack_int({g[0].de, g[1].de, g[2].de, g[3].de}));
        emit(pack_int({g[0].dy, g[1].dy, g[2].dy, g[3].dy}));
        emit(pack_frac({g[0].de, g[1].de, g[2].de, g[3].de}));
        emit(pack_frac({g[0].dy, g[1].dy, g[2].dy, g[3].dy}));
    };

    if (shade) {
        const bool smooth = (geometry_mode_ & G_SHADING_SMOOTH) != 0;
        const ScreenVertex& flat = a;  // flat shading takes the first vertex's color
        // The microcode adds half a step so the RDP's truncation rounds the colors.
        const auto channel = [&](auto member) {
            if (!smooth) return gradient(flat.*member + 0.5, flat.*member + 0.5, flat.*member + 0.5);
            return gradient(h.*member + 0.5, m.*member + 0.5, l.*member + 0.5);
        };
        emit_block({channel(&ScreenVertex::r), channel(&ScreenVertex::g), channel(&ScreenVertex::b),
                    channel(&ScreenVertex::a)});
    }
    if (texture) {
        // With perspective, the RDP gets S/W, T/W and 1/W and divides per pixel. 1/W is scaled
        // so the nearest vertex sits at 0x3FFF, half the range like the microcode, which leaves
        // room for the value extrapolated to the start scanline.
        const bool perspective = (other_mode_h_ & kTexturePerspective) != 0;
        const auto inv = [](const ScreenVertex& p) { return p.w > 0 ? 65536.0 / p.w : 0.0; };
        const double max_inv = std::max({inv(h), inv(m), inv(l), 1e-9});
        const auto w_of = [&](const ScreenVertex& p) { return perspective ? 0x3FFF * inv(p) / max_inv : 0x7FFF; };
        const auto s_of = [&](const ScreenVertex& p) { return perspective ? p.s * w_of(p) / 0x8000 : p.s; };
        const auto t_of = [&](const ScreenVertex& p) { return perspective ? p.t * w_of(p) / 0x8000 : p.t; };
        const Gradient zero{};
        emit_block({gradient(s_of(h), s_of(m), s_of(l)), gradient(t_of(h), t_of(m), t_of(l)),
                    gradient(w_of(h), w_of(m), w_of(l)), zero});
    }
    if (depth) {
        const Gradient z = gradient(h.z / 65536.0 * kZScale, m.z / 65536.0 * kZScale, l.z / 65536.0 * kZScale);
        emit((static_cast<std::uint64_t>(static_cast<std::uint32_t>(z.value)) << 32) | static_cast<std::uint32_t>(z.dx));
        emit((static_cast<std::uint64_t>(static_cast<std::uint32_t>(z.de)) << 32) | static_cast<std::uint32_t>(z.dy));
    }
}

}  // namespace ezy::n64
