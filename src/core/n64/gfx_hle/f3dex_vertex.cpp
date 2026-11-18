// F3DEX HLE: matrices, lights and the vertex pipeline (G_MTX, G_MOVEMEM, G_VTX).
#include <algorithm>
#include <cmath>

#include "n64/gfx_hle/f3dex.hpp"

namespace ezy::n64 {

using namespace gbi;

namespace {

std::int16_t read16(std::span<const std::uint8_t> rdram, std::uint32_t address) {
    return static_cast<std::int16_t>((rdram[address] << 8) | rdram[address + 1]);
}

std::uint8_t clamp_color(std::int32_t value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }

}  // namespace

void F3dex::load_matrix(std::uint32_t w0, std::uint32_t address, std::span<const std::uint8_t> rdram) {
    const std::uint32_t params = (w0 >> 16) & 0xFF;
    wait_for(address, 64);
    const Mtx m = load_mtx(rdram, address);
    if (params & G_MTX_PROJECTION) {
        projection_ = (params & G_MTX_LOAD) ? m : mul_mtx(m, projection_);
    } else {
        if ((params & G_MTX_PUSH) && modelview_depth_ + 1 < kMatrixStackDepth) {
            modelview_[modelview_depth_ + 1] = modelview_[modelview_depth_];
            ++modelview_depth_;
        }
        Mtx& current = modelview_[modelview_depth_];
        current = (params & G_MTX_LOAD) ? m : mul_mtx(m, current);
    }
    mvp_ = mul_mtx(modelview_[modelview_depth_], projection_);
}

void F3dex::move_memory(unsigned index, std::uint32_t address, std::span<const std::uint8_t> rdram) {
    if (address + 16 > rdram.size()) return;
    wait_for(address, 16);
    if (index == G_MV_VIEWPORT) {
        for (unsigned i = 0; i < 4; ++i) {
            viewport_scale_[i] = read16(rdram, address + i * 2);
            viewport_translate_[i] = read16(rdram, address + 8 + i * 2);
        }
    } else if (index == G_MV_LOOKATX || index == G_MV_LOOKATY) {
        auto& axis = lookat_[index == G_MV_LOOKATX ? 0 : 1];
        for (unsigned i = 0; i < 3; ++i) axis[i] = static_cast<std::int8_t>(rdram[address + 8 + i]);
        if (index == G_MV_LOOKATY) lookat_y_green_ = rdram[address + 1];
    } else if (index >= G_MV_L0 && index <= G_MV_L7 && (index & 1) == 0) {
        Light& light = lights_[(index - G_MV_L0) / 2];
        for (unsigned i = 0; i < 3; ++i) {
            light.color[i] = rdram[address + i];
            light.direction[i] = static_cast<std::int8_t>(rdram[address + 8 + i]);
        }
    }
}

// Directional lights in the modelview's space, plus the ambient (the light after the last one).
std::array<std::uint8_t, 3> F3dex::light_vertex(std::array<std::int8_t, 3> normal) const {
    std::array<double, 3> color{};
    const Light& ambient = lights_[num_lights_];
    for (unsigned c = 0; c < 3; ++c) color[c] = ambient.color[c];
    for (unsigned i = 0; i < num_lights_; ++i) {
        const Light& light = lights_[i];
        // The microcode does not normalize the normal: a shorter one is lit less
        // (tools/gfx_oracle). 127 x 127 is nearly full intensity.
        const double intensity = std::max(0.0, model_dot(normal, light.direction)) / 16384.0;
        for (unsigned c = 0; c < 3; ++c) color[c] += light.color[c] * intensity;
    }
    return {clamp_color(static_cast<std::int32_t>(color[0])), clamp_color(static_cast<std::int32_t>(color[1])),
            clamp_color(static_cast<std::int32_t>(color[2]))};
}

double F3dex::model_dot(std::array<std::int8_t, 3> normal, std::array<std::int8_t, 3> direction) const {
    const Mtx& mv = modelview_[modelview_depth_];
    std::array<double, 3> d{};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned k = 0; k < 3; ++k) d[r] += mv.m[r * 4 + k] / 65536.0 * direction[k];
    }
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len == 0) return 0;
    // The microcode keeps the result as bytes, after a reciprocal square root that overshoots
    // a little: the unit vector times 127.8, rounded down, matches what it stores: (127, 0, 0)
    // for +X, (112, 0, 61) where the exact value is (111.45, 0, 60.89), and -91 for -90.37.
    double dot = 0;
    for (unsigned i = 0; i < 3; ++i) dot += normal[i] * std::floor(d[i] / len * 127.8);
    return dot;
}

void F3dex::project(ScreenVertex& v) const {
    const auto& clip = v.clip_pos;
    // Perspective divide at full precision (the HLE does not reproduce the microcode's
    // reciprocal rounding), then the viewport. Vertices at or behind the camera get
    // clipped before they are drawn; they only need finite values here.
    v.inv_w = rsp_reciprocal(v.w, false);
    const std::int64_t w = v.w > 0 ? v.w : 1;
    const std::int64_t ndc_x = (std::int64_t{clip[0]} * 65536) / w;  // s15.16
    const std::int64_t ndc_y = (std::int64_t{clip[1]} * 65536) / w;
    const std::int64_t ndc_z = (std::int64_t{clip[2]} * 65536) / w;
    v.x = static_cast<std::int32_t>(viewport_translate_[0] + ((ndc_x * viewport_scale_[0]) >> 16));
    v.y = static_cast<std::int32_t>(viewport_translate_[1] + ((-ndc_y * viewport_scale_[1]) >> 16));
    const double w_real = v.w > 0 ? v.w : 1.0;
    v.px = (viewport_translate_[0] + clip[0] / w_real * viewport_scale_[0]) / 4.0;
    v.py = (viewport_translate_[1] - clip[1] / w_real * viewport_scale_[1]) / 4.0;
    v.z = clamp_s15_16(std::int64_t{viewport_translate_[2]} * 65536 + ndc_z * viewport_scale_[2]);
}

void F3dex::load_vertices(std::uint32_t address, unsigned first, unsigned count, std::span<const std::uint8_t> rdram) {
    wait_for(address, count * 16);
    for (unsigned i = 0; i < count && first + i < vertices_.size(); ++i) {
        const std::uint32_t at = address + i * 16;
        if (at + 16 > rdram.size()) return;
        ScreenVertex& v = vertices_[first + i];
        ++vertices_loaded_;
        const std::int16_t x = read16(rdram, at), y = read16(rdram, at + 2), z = read16(rdram, at + 4);
        const auto clip = transform(mvp_, x, y, z);
        v.clip_pos = clip;
        v.w = clip[3];

        v.clip = 0;
        const std::int64_t guard = std::int64_t{clip_ratio_} * v.w;
        if (clip[0] > guard) v.clip |= kClipPosX;
        if (clip[0] < -guard) v.clip |= kClipNegX;
        if (clip[1] > guard) v.clip |= kClipPosY;
        if (clip[1] < -guard) v.clip |= kClipNegY;
        if (v.w <= 0 || clip[2] < -v.w) v.clip |= kClipNear;

        project(v);
        const std::int64_t ndc_z = v.w > 0 ? (std::int64_t{clip[2]} * 65536) / v.w : 0;  // for fog

        std::int32_t s = read16(rdram, at + 8), t = read16(rdram, at + 10);
        if (geometry_mode_ & G_TEXTURE_GEN) {
            // Sphere mapping, as F3DEX 0.95 does it (measured with tools/gfx_oracle): the
            // normal as stored (not normalized) against each lookat axis brought into model
            // space, 0x4000 plus that dot product (127 x 127 at most), so with gsSPTexture's
            // usual 0x07C0 scale about 0 to 32 texels (s10.5) across a 32x32 environment map.
            // t's product is scaled by the Y light's green over 0x80. G_TEXTURE_GEN_LINEAR
            // bends it with the microcode's cubic, close to an arcsine.
            const std::array<std::int8_t, 3> normal = {static_cast<std::int8_t>(rdram[at + 12]),
                                                       static_cast<std::int8_t>(rdram[at + 13]),
                                                       static_cast<std::int8_t>(rdram[at + 14])};
            const auto generate = [&](double dot) {
                double u = dot / 16384.0;
                if (geometry_mode_ & G_TEXTURE_GEN_LINEAR) u *= 0.680554 + 0.363436 * u * u;
                return static_cast<std::int32_t>(std::clamp(std::lround(16384.0 * (1.0 + u)), -32768L, 32767L));
            };
            s = generate(model_dot(normal, lookat_[0]));
            t = generate(model_dot(normal, lookat_[1]) * lookat_y_green_ / 128.0);
        }
        v.s = static_cast<std::int16_t>((s * texture_scale_s_) >> 16);
        v.t = static_cast<std::int16_t>((t * texture_scale_t_) >> 16);

        v.a = rdram[at + 15];
        if (geometry_mode_ & G_LIGHTING) {
            const auto lit = light_vertex({static_cast<std::int8_t>(rdram[at + 12]), static_cast<std::int8_t>(rdram[at + 13]),
                                           static_cast<std::int8_t>(rdram[at + 14])});
            v.r = lit[0], v.g = lit[1], v.b = lit[2];
        } else {
            v.r = rdram[at + 12], v.g = rdram[at + 13], v.b = rdram[at + 14];
        }
        if (geometry_mode_ & G_FOG) {
            v.a = clamp_color(static_cast<std::int32_t>(((ndc_z * fog_multiplier_) >> 16) + fog_offset_));
        }
    }
}

}  // namespace ezy::n64
