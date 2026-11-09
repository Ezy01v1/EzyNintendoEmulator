#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "n64/gfx_hle/fixed.hpp"
#include "n64/gfx_hle/task.hpp"
#include "n64/mi.hpp"
#include "n64/rdp.hpp"

namespace ezy::n64 {

// GBI 1 / F3DEX 0.95 display-list opcodes (the high byte of w0).
namespace gbi {
inline constexpr unsigned G_SPNOOP = 0x00, G_MTX = 0x01, G_MOVEMEM = 0x03, G_VTX = 0x04, G_DL = 0x06,
                          G_LOAD_UCODE = 0xAF, G_TRI2 = 0xB1, G_RDPHALF_2 = 0xB3, G_QUAD = 0xB5, G_RDPHALF_1 = 0xB4,
                          G_CLEARGEOMETRYMODE = 0xB6, G_SETGEOMETRYMODE = 0xB7, G_ENDDL = 0xB8,
                          G_SETOTHERMODE_L = 0xB9, G_SETOTHERMODE_H = 0xBA, G_TEXTURE = 0xBB,
                          G_MOVEWORD = 0xBC, G_POPMTX = 0xBD, G_CULLDL = 0xBE, G_TRI1 = 0xBF,
                          G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5, G_SETTIMG = 0xFD, G_SETZIMG = 0xFE,
                          G_SETCIMG = 0xFF;
inline constexpr unsigned G_MW_NUMLIGHT = 0x02, G_MW_CLIP = 0x04, G_MW_SEGMENT = 0x06, G_MW_FOG = 0x08,
                          G_MW_LIGHTCOL = 0x0A, G_MW_PERSPNORM = 0x0E;
inline constexpr unsigned G_MV_VIEWPORT = 0x80, G_MV_LOOKATY = 0x82, G_MV_LOOKATX = 0x84, G_MV_L0 = 0x86, G_MV_L7 = 0x94;
inline constexpr std::uint32_t G_MTX_PROJECTION = 0x01, G_MTX_LOAD = 0x02, G_MTX_PUSH = 0x04;
inline constexpr std::uint32_t G_ZBUFFER = 0x1, G_SHADE = 0x4, G_SHADING_SMOOTH = 0x200, G_CULL_FRONT = 0x1000,
                               G_CULL_BACK = 0x2000, G_FOG = 0x10000, G_LIGHTING = 0x20000,
                               G_TEXTURE_GEN = 0x40000, G_TEXTURE_GEN_LINEAR = 0x80000;
}  // namespace gbi

// High-level emulation of the F3DEX 0.95 / F3DLX 0.95 graphics microcodes: walks a task's
// display list in RDRAM and hands the RDP the commands the microcode would have sent.
class F3dex {
public:
    // Clip flags of a vertex: outside the guard band (clip ratio x W) on each side, or at or
    // behind the near plane.
    static constexpr std::uint16_t kClipNegX = 1, kClipPosX = 2, kClipNegY = 4, kClipPosY = 8, kClipNear = 16;

    // A vertex after G_VTX: what triangle setup needs.
    struct ScreenVertex {
        std::int32_t x = 0, y = 0;  // screen position in quarter pixels (10.2)
        double px = 0, py = 0;      // the same in pixels, unrounded (triangle setup uses these)
        std::int32_t z = 0;         // screen depth, s15.16
        std::int32_t w = 0;         // clip-space W, s15.16
        std::uint32_t inv_w = 0;    // VRCP of W
        std::array<std::int32_t, 4> clip_pos{};  // clip-space x, y, z, w (s15.16), for clipping
        std::int16_t s = 0, t = 0;  // texture coordinates after G_TEXTURE's scale (s10.5)
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
        std::uint16_t clip = 0;
    };

    struct Result {
        std::uint32_t work;  // an estimate of the RSP's effort, for the task's duration
        bool ok;             // false: the list ended on an error (bad address, depth, unknown ucode)
    };

    Result run_task(const OsTask& task, GfxUcode ucode, std::span<std::uint8_t> rdram, Rdp& rdp, Mi& mi);

    [[nodiscard]] const ScreenVertex& vertex(unsigned index) const { return vertices_[index & 31]; }
    // G_VTX: count vertices from RDRAM into the buffer starting at first.
    void load_vertices(std::uint32_t address, unsigned first, unsigned count, std::span<const std::uint8_t> rdram);

    // A segmented address (segment in bits 27:24) made physical.
    [[nodiscard]] std::uint32_t segment_address(std::uint32_t address) const {
        return (segments_[(address >> 24) & 15] + (address & 0xFFFFFF)) & 0xFFFFFF;
    }

private:
    static constexpr unsigned kDlStackDepth = 10, kMatrixStackDepth = 10;
    struct Light {
        std::array<std::uint8_t, 3> color{};
        std::array<std::int8_t, 3> direction{};
    };

    // One display-list command; false ends the task with an error.
    bool execute(std::uint32_t w0, std::uint32_t w1, std::span<std::uint8_t> rdram);
    void emit(std::uint64_t word) { out_.push_back(word); }
    void end_display_list() {
        if (dl_depth_ == 0) ended_ = true;
        else pc_ = dl_stack_[--dl_depth_];
    }
    // Rejects, clips (guard band, and the near plane for F3DEX) and draws a triangle of the buffer.
    void clip_and_draw(unsigned i0, unsigned i1, unsigned i2);
    void draw_triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c);
    // Screen position and depth from clip_pos (viewport after the perspective divide).
    void project(ScreenVertex& v) const;

    GfxUcode ucode_ = GfxUcode::Unknown;
    std::array<std::uint32_t, 16> segments_{};
    std::array<std::uint32_t, kDlStackDepth> dl_stack_{};
    unsigned dl_depth_ = 0;
    std::uint32_t pc_ = 0;
    bool ended_ = false;
    std::uint32_t rdp_half_1_ = 0, rdp_half_2_ = 0;
    // Set Other Modes as the microcode keeps it (H: 24 bits); every task starts from its defaults.
    static constexpr std::uint32_t kInitialOtherModeH = 0x080CFF;
    std::uint32_t other_mode_h_ = kInitialOtherModeH, other_mode_l_ = 0;
    std::vector<std::uint64_t> out_;  // RDP words not yet handed over

    void load_matrix(std::uint32_t w0, std::uint32_t address, std::span<const std::uint8_t> rdram);
    void move_memory(unsigned index, std::uint32_t address, std::span<const std::uint8_t> rdram);
    [[nodiscard]] std::array<std::uint8_t, 3> light_vertex(std::array<std::int8_t, 3> normal) const;
    // The normal in view space (modelview 3x3), normalized; for texture generation.
    [[nodiscard]] std::array<double, 3> view_normal(std::array<std::int8_t, 3> normal) const;

    Mtx projection_{}, mvp_{};
    std::array<Mtx, kMatrixStackDepth> modelview_{};
    unsigned modelview_depth_ = 0;  // index of the current modelview
    std::array<std::int16_t, 4> viewport_scale_{}, viewport_translate_{};
    std::array<Light, 8> lights_{};
    std::array<std::array<std::int8_t, 3>, 2> lookat_{};  // G_TEXTURE_GEN axes (X, Y)
    unsigned num_lights_ = 0;  // directional lights; the ambient follows them
    std::uint32_t geometry_mode_ = 0;
    std::uint16_t texture_scale_s_ = 0, texture_scale_t_ = 0;
    std::uint32_t texture_ = 0;  // G_TEXTURE w0: level, tile, on
    std::int16_t fog_multiplier_ = 0, fog_offset_ = 0;
    std::int32_t clip_ratio_ = 2;
    std::array<ScreenVertex, 32> vertices_{};
};

}  // namespace ezy::n64
