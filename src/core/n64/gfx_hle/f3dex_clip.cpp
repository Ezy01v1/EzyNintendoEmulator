// F3DEX HLE: triangle rejection and clipping against the guard band and the near plane.
#include <algorithm>
#include <cmath>
#include <vector>

#include "n64/gfx_hle/f3dex.hpp"

namespace ezy::n64 {

namespace {

// A vertex in clip space with the attributes that get interpolated along clipped edges.
struct ClipVertex {
    double x, y, z, w, r, g, b, a, s, t;
};

ClipVertex lerp(const ClipVertex& p, const ClipVertex& q, double f) {
    const auto mix = [f](double u, double v) { return u + (v - u) * f; };
    return {mix(p.x, q.x), mix(p.y, q.y), mix(p.z, q.z), mix(p.w, q.w), mix(p.r, q.r),
            mix(p.g, q.g), mix(p.b, q.b), mix(p.a, q.a), mix(p.s, q.s), mix(p.t, q.t)};
}

// Sutherland-Hodgman against one plane; distance >= 0 is inside.
template <class Distance>
std::vector<ClipVertex> clip_polygon(const std::vector<ClipVertex>& in, Distance distance) {
    std::vector<ClipVertex> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const ClipVertex& p = in[i];
        const ClipVertex& q = in[(i + 1) % in.size()];
        const double dp = distance(p), dq = distance(q);
        if (dp >= 0) out.push_back(p);
        if ((dp >= 0) != (dq >= 0)) out.push_back(lerp(p, q, dp / (dp - dq)));
    }
    return out;
}

}  // namespace

void F3dex::clip_and_draw(unsigned i0, unsigned i1, unsigned i2) {
    const ScreenVertex& a = vertices_[i0 & 31];
    const ScreenVertex& b = vertices_[i1 & 31];
    const ScreenVertex& c = vertices_[i2 & 31];
    if (a.clip & b.clip & c.clip) return;  // all off the same plane
    const std::uint16_t any = a.clip | b.clip | c.clip;
    if (any == 0) {
        draw_triangle(a, b, c);
        return;
    }
    const bool near_clipping = ucode_ != GfxUcode::F3dlx095;  // F3DLX has no near clipping
    if ((any & kClipNear) && !near_clipping) return;

    const auto to_clip = [](const ScreenVertex& v) {
        return ClipVertex{v.clip_pos[0] / 65536.0, v.clip_pos[1] / 65536.0, v.clip_pos[2] / 65536.0,
                          v.clip_pos[3] / 65536.0, double(v.r), double(v.g), double(v.b), double(v.a),
                          double(v.s), double(v.t)};
    };
    std::vector<ClipVertex> poly = {to_clip(a), to_clip(b), to_clip(c)};
    const double ratio = clip_ratio_;
    if (any & kClipNear) poly = clip_polygon(poly, [](const ClipVertex& v) { return v.z + v.w; });
    if (any & kClipPosX) poly = clip_polygon(poly, [ratio](const ClipVertex& v) { return ratio * v.w - v.x; });
    if (any & kClipNegX) poly = clip_polygon(poly, [ratio](const ClipVertex& v) { return ratio * v.w + v.x; });
    if (any & kClipPosY) poly = clip_polygon(poly, [ratio](const ClipVertex& v) { return ratio * v.w - v.y; });
    if (any & kClipNegY) poly = clip_polygon(poly, [ratio](const ClipVertex& v) { return ratio * v.w + v.y; });
    if (poly.size() < 3) return;

    std::vector<ScreenVertex> screen(poly.size());
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const ClipVertex& p = poly[i];
        ScreenVertex& v = screen[i];
        const auto fixed = [](double value) {
            return static_cast<std::int32_t>(std::clamp(value * 65536.0, -2147483648.0, 2147483647.0));
        };
        v.clip_pos = {fixed(p.x), fixed(p.y), fixed(p.z), fixed(std::max(p.w, 1.0 / 65536))};
        v.w = v.clip_pos[3];
        project(v);
        const auto color = [](double value) { return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L)); };
        v.r = color(p.r), v.g = color(p.g), v.b = color(p.b), v.a = color(p.a);
        v.s = static_cast<std::int16_t>(std::lround(p.s));
        v.t = static_cast<std::int16_t>(std::lround(p.t));
    }
    // Flat shading keeps the first original vertex's color on every piece.
    if (!(geometry_mode_ & gbi::G_SHADING_SMOOTH)) {
        for (auto& v : screen) v.r = a.r, v.g = a.g, v.b = a.b, v.a = a.a;
    }
    for (std::size_t i = 1; i + 1 < screen.size(); ++i) draw_triangle(screen[0], screen[i], screen[i + 1]);
}

}  // namespace ezy::n64
