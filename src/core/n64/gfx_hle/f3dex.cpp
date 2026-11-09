#include "n64/gfx_hle/f3dex.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace ezy::n64 {

using namespace gbi;

namespace {

constexpr std::uint32_t kMaxCommands = 1U << 20;  // a runaway list ends the task

std::uint32_t read32(std::span<const std::uint8_t> rdram, std::uint32_t address) {
    return (std::uint32_t{rdram[address]} << 24) | (std::uint32_t{rdram[address + 1]} << 16) |
           (std::uint32_t{rdram[address + 2]} << 8) | rdram[address + 3];
}

}  // namespace

F3dex::Result F3dex::run_task(const OsTask& task, GfxUcode ucode, std::span<std::uint8_t> rdram, Rdp& rdp, Mi& mi) {
    ucode_ = ucode;
    segments_ = {};
    modelview_depth_ = 0;
    geometry_mode_ = 0;
    num_lights_ = 0;
    clip_ratio_ = 2;
    other_mode_h_ = kInitialOtherModeH;
    other_mode_l_ = 0;
    dl_depth_ = 0;
    ended_ = false;
    out_.clear();
    pc_ = task.data_ptr & 0xFFFFF8;

    bool ok = true;
    std::uint32_t done = 0;
    for (; done < kMaxCommands && !ended_; ++done) {
        if (pc_ + 8 > rdram.size()) {
            ok = false;
            break;
        }
        const std::uint32_t w0 = read32(rdram, pc_), w1 = read32(rdram, pc_ + 4);
        pc_ += 8;
        if (!execute(w0, w1, rdram)) {
            ok = false;
            break;
        }
    }
    if (done == kMaxCommands) ok = false;
    rdp.submit_words(out_, mi, rdram);
    out_.clear();
    return {done, ok};
}

bool F3dex::execute(std::uint32_t w0, std::uint32_t w1, std::span<std::uint8_t> rdram) {
    const unsigned op = w0 >> 24;
    switch (op) {
        case G_DL: {
            const std::uint32_t target = segment_address(w1) & ~7U;
            if (target + 8 > rdram.size()) return false;
            if (((w0 >> 16) & 0xFF) == 0) {  // G_DL_PUSH; otherwise a branch
                if (dl_depth_ == kDlStackDepth) return false;
                dl_stack_[dl_depth_++] = pc_;
            }
            pc_ = target;
            return true;
        }
        case G_ENDDL:
            if (dl_depth_ == 0) ended_ = true;
            else pc_ = dl_stack_[--dl_depth_];
            return true;
        case G_MOVEWORD:
            switch (w0 & 0xFF) {
                case G_MW_SEGMENT: segments_[((w0 >> 8) & 0xFFFF) / 4 & 15] = w1 & 0xFFFFFF; break;
                case G_MW_NUMLIGHT: num_lights_ = std::min(7U, ((w1 & 0x7FFFFFFF) / 32) - 1); break;
                case G_MW_CLIP:  // gSPClipRatio writes r and -r to the four planes
                    clip_ratio_ = std::abs(static_cast<std::int16_t>(w1 & 0xFFFF));
                    break;
                case G_MW_FOG:
                    fog_multiplier_ = static_cast<std::int16_t>(w1 >> 16);
                    fog_offset_ = static_cast<std::int16_t>(w1);
                    break;
                default: break;
            }
            return true;
        case G_SETOTHERMODE_H:
        case G_SETOTHERMODE_L: {
            // w0 carries the field's shift and length; the microcode merges it and sends the whole mode.
            const unsigned shift = (w0 >> 8) & 0xFF, length = w0 & 0xFF;
            const std::uint64_t field = length >= 32 ? 0xFFFFFFFFULL : ((1ULL << length) - 1);
            const auto mask = static_cast<std::uint32_t>(field << shift);
            std::uint32_t& mode = op == G_SETOTHERMODE_H ? other_mode_h_ : other_mode_l_;
            mode = (mode & ~mask) | (w1 & mask);
            emit((std::uint64_t{0xEF000000U | (other_mode_h_ & 0xFFFFFF)} << 32) | other_mode_l_);
            return true;
        }
        case G_MTX: load_matrix(w0, segment_address(w1), rdram); return true;
        case G_POPMTX:
            if (modelview_depth_ > 0) --modelview_depth_;
            mvp_ = mul_mtx(modelview_[modelview_depth_], projection_);
            return true;
        case G_MOVEMEM: move_memory((w0 >> 16) & 0xFF, segment_address(w1), rdram); return true;
        case G_VTX: load_vertices(segment_address(w1), ((w0 >> 16) & 0xFF) / 2, (w0 >> 10) & 0x3F, rdram); return true;
        case G_TEXTURE:
            texture_ = w0 & 0xFFFFFF;
            texture_scale_s_ = static_cast<std::uint16_t>(w1 >> 16);
            texture_scale_t_ = static_cast<std::uint16_t>(w1);
            return true;
        case G_SETGEOMETRYMODE: geometry_mode_ |= w1; return true;
        case G_CLEARGEOMETRYMODE: geometry_mode_ &= ~w1; return true;
        case G_RDPHALF_1: rdp_half_1_ = w1; return true;
        case G_RDPHALF_2: rdp_half_2_ = w1; return true;
        case G_LOAD_UCODE: {
            // The new microcode's data segment comes from the preceding G_RDPHALF_1.
            const GfxUcode next = identify_gfx_ucode(rdp_half_1_, (w0 & 0xFFFF) + 1, rdram);
            if (next == GfxUcode::Unknown) {
                std::fprintf(stderr, "N64 gfx HLE: G_LOAD_UCODE of an unknown microcode, task ended\n");
                return false;
            }
            ucode_ = next;
            return true;
        }
        case G_TEXRECT:
        case G_TEXRECTFLIP: {
            // The microcode reads S, T and DsDx, DtDy from the two G_RDPHALF commands that follow.
            if (pc_ + 16 > rdram.size()) return false;
            const std::uint32_t s_t = read32(rdram, pc_ + 4), ds_dt = read32(rdram, pc_ + 12);
            pc_ += 16;
            emit((std::uint64_t{w0} << 32) | w1);
            emit((std::uint64_t{s_t} << 32) | ds_dt);
            return true;
        }
        case G_SETTIMG:
        case G_SETZIMG:
        case G_SETCIMG:
            emit((std::uint64_t{w0} << 32) | segment_address(w1));
            return true;
        default:
            if (op >= 0xC0) emit((std::uint64_t{w0} << 32) | w1);  // RDP commands pass straight through
            return true;  // geometry commands come in later tasks
    }
}

}  // namespace ezy::n64
