#include "gc/gx_fifo.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "gc/gekko/gekko.hpp"
#include "gc/memory.hpp"
#include "gc/pi.hpp"
#include "gc/vi.hpp"

namespace ezy::gc {

namespace {
constexpr std::uint32_t kCpBlock = Memory::mmio_block_base(MmioBlock::Cp);
constexpr std::uint32_t kPeBlock = Memory::mmio_block_base(MmioBlock::Pe);

// CP CTRL (Dolphin's UCPCtrlReg) and STATUS (UCPStatusReg) bits.
constexpr std::uint16_t kGpReadEnable = 1u << 0, kBpEnable = 1u << 1, kOverflowIntEnable = 1u << 2,
                        kUnderflowIntEnable = 1u << 3, kGpLinkEnable = 1u << 4, kBpInt = 1u << 5;
constexpr std::uint16_t kCtrlBits = 0x3F;
constexpr std::uint16_t kStOverflow = 1u << 0, kStUnderflow = 1u << 1, kStReadIdle = 1u << 2,
                        kStCommandIdle = 1u << 3, kStBreakpoint = 1u << 4;
// The FIFO registers' halves: LO 32-byte aligned, HI the GameCube's 26-bit physical range
// (Dolphin's WMASK_LO_ALIGN_32BIT and WMASK_HI_RESTRICT).
constexpr std::uint16_t kLoMask = 0xFFE0, kHiMask = 0x03FF;
constexpr std::uint32_t kAddressMask = 0x03FFFFE0;
constexpr std::uint32_t kCpStatus = 0x00, kCpCtrl = 0x02, kCpClear = 0x04, kCpPerfSelect = 0x06,
                        kCpFifoFirst = 0x20, kCpFifoLast = 0x3E, kCpDistanceHi = 0x32;

// PE (Dolphin's PixelEngine.h): CTRL bits and registers.
constexpr std::uint16_t kPeTokenEnable = 1u << 0, kPeFinishEnable = 1u << 1, kPeTokenAck = 1u << 2,
                        kPeFinishAck = 1u << 3;
constexpr std::uint32_t kPeConfigLast = 0x08, kPeCtrl = 0x0A, kPeToken = 0x0E;

// BP registers with side effects or that the EFB copy reads (BPMemory.h).
constexpr std::uint8_t kBpDrawDone = 0x45, kBpToken = 0x47, kBpTokenInt = 0x48, kBpEfbTopLeft = 0x49,
                       kBpEfbSize = 0x4A, kBpCopyDest = 0x4B, kBpCopyStride = 0x4D, kBpCopyYScale = 0x4E,
                       kBpClearAr = 0x4F, kBpClearGb = 0x50, kBpCopy = 0x52, kBpMask = 0xFE;
constexpr std::uint32_t kBpValueMask = 0xFFFFFF;
// UPE_Copy bits.
constexpr std::uint32_t kCopyScaleInvert = 1u << 10, kCopyClear = 1u << 11, kCopyToXfb = 1u << 14;
constexpr std::uint32_t kEfbWidth = 640, kEfbHeight = 528;

std::string hex32(std::uint32_t value) {
    char b[11];
    std::snprintf(b, sizeof b, "0x%08X", value);
    return b;
}
std::string hex8(unsigned value) {
    char b[5];
    std::snprintf(b, sizeof b, "0x%02X", value & 0xFFu);
    return b;
}

[[noreturn]] void unmodeled(const char* block, std::uint32_t base, const char* what, std::uint32_t offset) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", offset);
    throw BusError(std::string(what) + " of the unmodeled " + block + " register at offset 0x" + b, base + offset);
}

std::uint32_t be16(const std::uint8_t* p) { return std::uint32_t{p[0]} << 8 | p[1]; }
std::uint32_t be24(const std::uint8_t* p) { return std::uint32_t{p[0]} << 16 | std::uint32_t{p[1]} << 8 | p[2]; }
std::uint32_t be32(const std::uint8_t* p) { return be16(p) << 16 | be16(p + 2); }

// The next 32-byte block of a FIFO whose `end` is the address of its last block.
std::uint32_t next_block(std::uint32_t at, std::uint32_t base, std::uint32_t end) {
    return at == end ? base : (at + GxFifo::kBurst) & kAddressMask;
}
}  // namespace

GxFifo::GxFifo(Pi& pi, std::span<std::uint8_t> mem1) : pi_(pi), mem1_(mem1) {
    bp_[kBpMask] = kBpValueMask;  // Dolphin's BPInit
}

// --- The write-gather pipe ---

void GxFifo::write(std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        gather_[gathered_++] = byte;
        if (gathered_ == kBurst) burst();
    }
}

void GxFifo::burst() {
    gathered_ = 0;  // first: a burst that throws is dropped, the next store starts a new one
    const std::uint32_t at = pi_.fifo_write_pointer();
    if (std::uint64_t{at} + kBurst > mem1_.size()) {
        throw BusError("the write-gather pipe's burst to " + hex32(at) + " is outside MEM1", at);
    }
    std::copy(gather_.begin(), gather_.end(), mem1_.begin() + at);
    counters_.fifo_bytes += kBurst;
    pi_.set_fifo_write_pointer(at == pi_.fifo_end() ? pi_.fifo_base() : at + kBurst);
    if (cp_ctrl_ & kGpLinkEnable) {  // Dolphin's GatherPipeBursted
        wptr_ = next_block(wptr_, base_, end_);
        distance_ += kBurst;
    }
    run_cp();
}

void GxFifo::reset_pipe() {
    gathered_ = 0;
    pending_.clear();
    pending_blocks_.clear();
    pending_pos_ = 0;
}

// --- The command processor ---

bool GxFifo::at_breakpoint() const { return (cp_ctrl_ & kBpEnable) && rptr_ == breakpoint_; }

std::uint16_t GxFifo::cp_status() const {
    std::uint16_t status = 0;
    if (distance_ > hi_mark_) status |= kStOverflow;
    if (distance_ < lo_mark_) status |= kStUnderflow;
    if (distance_ == 0 || rptr_ == wptr_) status |= kStReadIdle;
    if (distance_ == 0 || at_breakpoint() || !(cp_ctrl_ & kGpReadEnable)) status |= kStCommandIdle;
    if (at_breakpoint()) status |= kStBreakpoint;
    return status;
}

void GxFifo::update_cp_irq() {
    const std::uint16_t status = cp_status();
    const bool cause = ((status & kStBreakpoint) && (cp_ctrl_ & kBpInt)) ||
                       ((status & kStOverflow) && (cp_ctrl_ & kOverflowIntEnable)) ||
                       ((status & kStUnderflow) && (cp_ctrl_ & kUnderflowIntEnable));
    pi_.set_cause(Pi::kCp, cause && (cp_ctrl_ & kGpReadEnable));
}

void GxFifo::run_cp() {
    while ((cp_ctrl_ & kGpReadEnable) && distance_ != 0 && !at_breakpoint()) {
        const std::uint32_t at = rptr_;
        if (std::uint64_t{at} + kBurst > mem1_.size()) {
            throw BusError("the CP's FIFO read at " + hex32(at) + " is outside MEM1", at);
        }
        pending_.insert(pending_.end(), mem1_.begin() + at, mem1_.begin() + at + kBurst);
        pending_blocks_.push_back(at);
        rptr_ = next_block(rptr_, base_, end_);
        distance_ -= kBurst;  // a multiple of 32: its LO half is 32-byte aligned

        const Source source{false, 0, pending_pos_};
        std::size_t ran = 0;
        try {
            run_commands(std::span<const std::uint8_t>(pending_).subspan(pending_pos_), source, ran);
        } catch (...) {
            pending_pos_ += ran;  // the commands before the offending one are done
            throw;
        }
        pending_pos_ += ran;
        // Drop the blocks that are run to their end.
        const std::size_t done = pending_pos_ / kBurst;
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(done * kBurst));
        pending_blocks_.erase(pending_blocks_.begin(), pending_blocks_.begin() + static_cast<std::ptrdiff_t>(done));
        pending_pos_ -= done * kBurst;
    }
    update_cp_irq();
}

std::uint32_t GxFifo::address_of(const Source& source, std::size_t offset) const {
    if (source.list) return source.base + static_cast<std::uint32_t>(offset);
    const std::size_t k = source.origin + offset;
    return pending_blocks_[k / kBurst] + static_cast<std::uint32_t>(k % kBurst);
}

void GxFifo::run_commands(std::span<const std::uint8_t> data, const Source& source, std::size_t& pos) {
    auto counted = [this](GxCommand kind) { ++counters_.commands[static_cast<std::size_t>(kind)]; };
    pos = 0;
    while (pos < data.size()) {
        const std::uint8_t* p = data.data() + pos;
        const std::size_t left = data.size() - pos;
        const std::uint8_t op = p[0];
        auto here = [&] { return address_of(source, pos); };
        std::size_t bytes = 0;
        switch (op) {
            case 0x00:  // NOP
                bytes = 1;
                counted(GxCommand::Nop);
                break;
            case 0x08:  // LOAD_CP_REG: register, value
                if (left < 6) return;
                load_cp_reg(p[1], be32(p + 2), here());
                bytes = 6;
                counted(GxCommand::LoadCpReg);
                break;
            case 0x10: {  // LOAD_XF_REG: count - 1 and address, then the words. Accepted, no effect.
                if (left < 5) return;
                const std::uint32_t word = be32(p + 1);
                if ((word >> 16) >= 16) {
                    throw BusError("LOAD_XF_REG at " + hex32(here()) + " with the count field " + hex32(word >> 16) +
                                       " (over 16 words) is not modeled",
                                   here());
                }
                bytes = 5 + 4 * std::size_t{(word >> 16) + 1};
                if (left < bytes) return;
                counted(GxCommand::LoadXfReg);
                break;
            }
            case 0x20:
            case 0x28:
            case 0x30:
            case 0x38:  // LOAD_INDX_A..D: index, size, address. Accepted, no effect (XF matrices).
                if (left < 5) return;
                bytes = 5;
                counted(GxCommand::LoadIndexed);
                break;
            case 0x40:  // CALL_DL: address, size
                if (left < 9) return;
                if (source.list) {
                    throw BusError("CALL_DL at " + hex32(here()) + " inside the display list at " + hex32(source.base) +
                                       " (the hardware does not nest lists)",
                                   here());
                }
                call_display_list(be32(p + 1), be32(p + 5));
                bytes = 9;
                counted(GxCommand::CallDisplayList);
                break;
            case 0x48:  // INVALIDATE_VTX_CACHE: accepted, no effect (no vertex cache)
                bytes = 1;
                counted(GxCommand::InvalidateVertexCache);
                break;
            case 0x61:  // LOAD_BP_REG: register, 24-bit value
                if (left < 5) return;
                load_bp_reg(p[1], be24(p + 2), here());
                bytes = 5;
                counted(GxCommand::LoadBpReg);
                break;
            default: {
                if ((op & 0xC0) != 0x80) {
                    throw BusError("unknown graphics command opcode " + hex8(op) + " at " + hex32(here()) +
                                       (source.list ? " in the display list at " + hex32(source.base) : " in the FIFO"),
                                   here());
                }
                // A primitive: type in bits 3-6, VAT group in bits 0-2, then the vertex count.
                if (left < 3) return;
                const auto vertex_bytes = vertex_.vertex_size(op & 7u);
                if (!vertex_bytes) {
                    throw BusError("primitive " + hex8(op) + " at " + hex32(here()) + ": VAT group " +
                                       std::to_string(op & 7u) + " has a color format the hardware does not define",
                                   here());
                }
                const std::uint32_t vertices = be16(p + 1);
                bytes = 3 + std::size_t{vertices} * *vertex_bytes;
                if (left < bytes) return;
                counted(GxCommand::Primitive);
                ++counters_.primitives;
                counters_.vertices += vertices;
                break;
            }
        }
        pos += bytes;
    }
}

void GxFifo::call_display_list(std::uint32_t pointer, std::uint32_t raw_size) {
    // OpcodeDecoding's OnDisplayList(address & ~31, size & ~31), then Memmap's
    // GetSpanForAddress (address &= 0x3FFFFFFF): cached and uncached pointers reach MEM1.
    const std::uint32_t address = (pointer & 0x3FFFFFFF) & ~31u, size = raw_size & ~31u;
    if (std::uint64_t{address} + size > mem1_.size()) {
        throw BusError("display list at " + hex32(pointer) + " (physical " + hex32(address) + "), " + hex32(size) +
                           " bytes, runs past MEM1",
                       address);
    }
    const Source source{true, address, 0};
    std::size_t done = 0;
    run_commands(std::span<const std::uint8_t>(mem1_).subspan(address, size), source, done);
    if (done != size) {
        const std::uint32_t at = address + static_cast<std::uint32_t>(done);
        throw BusError("display list at " + hex32(address) + " (" + hex32(size) + " bytes) ends inside the command at " +
                           hex32(at),
                       at);
    }
}

void GxFifo::load_cp_reg(std::uint8_t reg, std::uint32_t value, std::uint32_t at) {
    // By the high nibble, as Dolphin's CPState::LoadCPReg (which also takes 0x51-0x5F as VCD_LO
    // and 0x78-0x7F as VAT group reg & 7).
    switch (reg & 0xF0) {
        case 0x00:
        case 0x10:
        case 0x20: return;  // performance-metric commands (Dolphin's UNKNOWN_00/10/20): accepted, no effect
        case 0x30:
        case 0x40: return;  // MATINDEX_A/B: accepted, no effect (drawing only)
        case 0x50: vertex_.vcd_lo = value; return;
        case 0x60: vertex_.vcd_hi = value; return;
        case 0x70: vertex_.vat_a[reg & 7u] = value; return;
        case 0x80: vertex_.vat_b[reg & 7u] = value; return;
        case 0x90: vertex_.vat_c[reg & 7u] = value; return;
        case 0xA0:
        case 0xB0: return;  // ARRAY_BASE/ARRAY_STRIDE: accepted, no effect (drawing only)
        default: throw BusError("LOAD_CP_REG of the unknown CP register " + hex8(reg) + " at " + hex32(at), at);
    }
}

void GxFifo::load_bp_reg(std::uint8_t reg, std::uint32_t value, std::uint32_t at) {
    // Dolphin's LoadBPReg: the mask applies to this load, then goes back to all ones.
    const std::uint32_t mask = bp_[kBpMask];
    const std::uint32_t stored = (bp_[reg] & ~mask) | (value & mask);
    if (reg != kBpMask) bp_[kBpMask] = kBpValueMask;
    bp_[reg] = stored;
    switch (reg) {
        case kBpDrawDone:
            if ((stored & 0xFF) != 0x02) {
                throw BusError("BP draw done (0x45) with the value " + hex32(stored) + " at " + hex32(at) +
                                   " is not modeled",
                               at);
            }
            finish_signal_ = true;
            update_pe_irq();
            return;
        case kBpToken:
        case kBpTokenInt:
            pe_token_ = static_cast<std::uint16_t>(stored);
            if (reg == kBpTokenInt) {
                token_signal_ = true;
                update_pe_irq();
            }
            return;
        case kBpCopy: efb_copy(stored, at); return;
        default: return;  // drawing state: stored, no effect
    }
}

void GxFifo::efb_copy(std::uint32_t copy, std::uint32_t at) {
    // Only a clearing copy to the XFB writes anything: the EFB is not drawn, so a copy that does
    // not clear (or one to a texture) is accepted with no effect.
    if (!(copy & kCopyToXfb) || !(copy & kCopyClear)) return;
    const std::uint32_t top_left = bp_[kBpEfbTopLeft], size = bp_[kBpEfbSize];
    const std::uint32_t x = top_left & 0x3FF, y = (top_left >> 10) & 0x3FF;
    if (x >= kEfbWidth || y >= kEfbHeight) return;  // Dolphin copies nothing from outside the EFB
    const std::uint32_t width = std::min(x + (size & 0x3FF) + 1, kEfbWidth) - x;
    const std::uint32_t stride = bp_[kBpCopyStride] << 5;
    if (stride == 0) return;  // Dolphin copies only with a stride
    // Dolphin's XFB height: 1 + (height - 1) * yScale, in float, truncated.
    const std::uint32_t y_scale_reg = bp_[kBpCopyYScale];
    float y_scale;
    if (copy & kCopyScaleInvert) {
        if (y_scale_reg == 0) {
            throw BusError("EFB copy at " + hex32(at) + " with scale_invert and a y scale of 0 is not modeled", at);
        }
        y_scale = 256.0f / static_cast<float>(y_scale_reg);
    } else {
        y_scale = static_cast<float>(y_scale_reg) / 256.0f;
    }
    const auto rows = static_cast<std::uint32_t>(1.0f + static_cast<float>((size >> 10) & 0x3FF) * y_scale);
    const std::uint32_t dest = bp_[kBpCopyDest] << 5;
    const std::uint32_t row_bytes = 2 * width;
    if (std::uint64_t{dest} + std::uint64_t{rows - 1} * stride + row_bytes > mem1_.size()) {
        throw BusError("EFB copy at " + hex32(at) + " to the XFB at " + hex32(dest) + " (" + std::to_string(rows) +
                           " rows of " + std::to_string(row_bytes) + " bytes, stride " + std::to_string(stride) +
                           ") runs past MEM1",
                       dest);
    }
    const std::uint32_t ar = bp_[kBpClearAr], gb = bp_[kBpClearGb];
    const Yuv c = rgb_to_yuv(static_cast<int>(ar & 0xFF), static_cast<int>((gb >> 8) & 0xFF), static_cast<int>(gb & 0xFF));
    const std::uint8_t pattern[4] = {c.y, c.u, c.y, c.v};  // two pixels, 4:2:2
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::uint8_t* out = mem1_.data() + dest + std::size_t{row} * stride;
        for (std::uint32_t i = 0; i < row_bytes; ++i) out[i] = pattern[i & 3];
    }
}

// --- CP registers ---

std::uint32_t& GxFifo::fifo_register(std::uint32_t offset) {
    switch (offset & ~3u) {
        case 0x20: return base_;
        case 0x24: return end_;
        case 0x28: return hi_mark_;
        case 0x2C: return lo_mark_;
        case 0x30: return distance_;
        case 0x34: return wptr_;
        case 0x38: return rptr_;
        default: return breakpoint_;  // 0x3C
    }
}

std::uint16_t GxFifo::cp_read16(std::uint32_t offset) {
    if (offset == kCpStatus) return cp_status();
    if (offset == kCpCtrl) return cp_ctrl_;
    if (offset == kCpClear) return 0;  // Dolphin: MMIO::Constant<u16>(0)
    if (offset == kCpPerfSelect) return perf_select_;
    if (offset >= kCpFifoFirst && offset <= kCpFifoLast && !(offset & 1)) {
        const std::uint32_t value = fifo_register(offset);
        return static_cast<std::uint16_t>((offset & 2) ? value >> 16 : value);
    }
    unmodeled("command processor", kCpBlock, "read", offset);
}

void GxFifo::cp_write16(std::uint32_t offset, std::uint16_t value) {
    if (offset == kCpCtrl) {
        cp_ctrl_ = value & kCtrlBits;
        run_cp();  // turning reading on (or a breakpoint off) reads what waits
        return;
    }
    if (offset == kCpClear) {  // accepted, no effect (Dolphin: not emulated; it runs the GPU)
        run_cp();
        return;
    }
    if (offset == kCpPerfSelect) {  // stored, no effect (performance metrics)
        perf_select_ = value & 0x0007;
        return;
    }
    if (offset >= kCpFifoFirst && offset <= kCpFifoLast && !(offset & 1)) {
        std::uint32_t& reg = fifo_register(offset);
        if (offset & 2) reg = (reg & 0xFFFFu) | std::uint32_t{static_cast<std::uint16_t>(value & kHiMask)} << 16;
        else reg = (reg & 0xFFFF0000u) | (value & kLoMask);
        if (offset == kCpDistanceHi) run_cp();  // Dolphin runs the GPU here
        else update_cp_irq();
        return;
    }
    unmodeled("command processor", kCpBlock, "write", offset);  // STATUS is read-only
}

std::uint32_t GxFifo::CpRegs::read32(std::uint32_t offset) {
    unmodeled("command processor", kCpBlock, "32-bit read (its registers are 16 bits)", offset);
}
void GxFifo::CpRegs::write32(std::uint32_t offset, std::uint32_t) {
    unmodeled("command processor", kCpBlock, "32-bit write (its registers are 16 bits)", offset);
}

// --- Pixel engine ---

void GxFifo::update_pe_irq() {
    pi_.set_cause(Pi::kPeToken, token_signal_ && (pe_ctrl_ & kPeTokenEnable));
    pi_.set_cause(Pi::kPeFinish, finish_signal_ && (pe_ctrl_ & kPeFinishEnable));
}

std::uint16_t GxFifo::pe_read16(std::uint32_t offset) {
    if (offset <= kPeConfigLast && !(offset & 1)) return pe_config_[offset / 2];
    if (offset == kPeCtrl) return pe_ctrl_;
    if (offset == kPeToken) return pe_token_;
    unmodeled("pixel engine", kPeBlock, "read", offset);  // bounding box, performance counters
}

void GxFifo::pe_write16(std::uint32_t offset, std::uint16_t value) {
    if (offset <= kPeConfigLast && !(offset & 1)) {
        pe_config_[offset / 2] = value;  // Z/alpha configuration: stored, no effect (drawing only)
        return;
    }
    if (offset == kPeCtrl) {
        if (value & kPeTokenAck) token_signal_ = false;
        if (value & kPeFinishAck) finish_signal_ = false;
        pe_ctrl_ = value & (kPeTokenEnable | kPeFinishEnable);
        update_pe_irq();
        return;
    }
    unmodeled("pixel engine", kPeBlock, "write", offset);  // the token is read-only
}

std::uint32_t GxFifo::PeRegs::read32(std::uint32_t offset) {
    unmodeled("pixel engine", kPeBlock, "32-bit read (its registers are 16 bits)", offset);
}
void GxFifo::PeRegs::write32(std::uint32_t offset, std::uint32_t) {
    unmodeled("pixel engine", kPeBlock, "32-bit write (its registers are 16 bits)", offset);
}

}  // namespace ezy::gc
