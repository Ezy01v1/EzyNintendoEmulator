#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "gc/gx_vertex.hpp"
#include "gc/mmio.hpp"

namespace ezy::gc {

class Pi;

// The kinds of graphics command the counters tell apart.
enum class GxCommand : std::uint8_t {
    Nop,                    // 0x00
    LoadCpReg,              // 0x08
    LoadXfReg,              // 0x10
    LoadIndexed,            // 0x20/0x28/0x30/0x38 (LOAD_INDX_A..D)
    CallDisplayList,        // 0x40
    InvalidateVertexCache,  // 0x48
    LoadBpReg,              // 0x61
    Primitive,              // 0x80-0xBF
    Count,
};
inline constexpr std::size_t kGxCommandCount = static_cast<std::size_t>(GxCommand::Count);

// What the graphics FIFO saw in one field (design: "Contadores por campo"; see
// GxFifo::end_field).
struct GxCounters {
    std::uint64_t fifo_bytes = 0;                           // written to the FIFO by the pipe
    std::array<std::uint64_t, kGxCommandCount> commands{};  // run, FIFO and display lists
    std::uint64_t primitives = 0;
    std::uint64_t vertices = 0;
};

// The graphics FIFO without drawing (design, "FIFO gráfico (sin dibujar)"): the write-gather
// pipe, the command processor (CP, 0x0C000000) reading the FIFO and display lists, the command
// parser, and the pixel engine (PE, 0x0C001000) events the game waits for. Commands are parsed
// only to know their size, answer what the game waits for and be counted. Sources: Dolphin's
// VideoCommon CommandProcessor.cpp/.h, Fifo.cpp, OpcodeDecoding.h, BPStructs.cpp, BPMemory.h,
// PixelEngine.cpp/.h, CPMemory.cpp/.h and HW/GPFifo.cpp (master, read on 2026-11-23).
//
// Pipe (WriteGatherPipe::write, the brief's write_pipe): the bytes gather in a 32-byte buffer;
// each full buffer is copied to MEM1 at the PI's FIFO write pointer, which then moves 32 bytes
// on, or back to the PI's base when it was at the PI's end (the end register is the address of
// the FIFO's last block: GPFifo::UpdateGatherPipe). A burst outside MEM1 throws BusError with
// its address; its 32 bytes are dropped and the pointers stay, so a later store starts a new
// burst. When the CP is linked (CTRL.GPLinkEnable) the CP's write pointer moves the same
// way within the CP's base/end and the read-write distance grows by 32 (GatherPipeBursted).
// Deviation: Dolphin also copies the CP's base/end/write pointer back to the PI when linked
// with reading on; here the PI keeps its own (games point both at the same FIFO).
//
// CP timing: there is no GPU timing model in this sub-project. The CP runs synchronously: after
// every burst and after writes to CTRL or FIFO_RW_DISTANCE_HI (where Dolphin runs the GPU), it
// reads 32-byte blocks while CTRL.GPReadEnable is set, the distance is not 0 and the read
// pointer is not at an enabled breakpoint (Fifo.cpp's loop); the read pointer wraps from end
// to base like the pipe, the distance drops by 32. Blocks queue as bytes, so a command split
// by a block or by the FIFO's wrap waits for the rest and runs once, whole. A command that throws
// stays queued (the ones before it are done): more data runs into the same error.
//
// CP registers, 16 bits each (a 32-bit access throws: Dolphin registers none), offsets from
// 0x0C000000:
//   00 STATUS   read-only: OverflowHiWatermark 0 (distance > high mark), UnderflowLoWatermark
//               1 (distance < low mark), ReadIdle 2 (distance 0 or read == write pointer),
//               CommandIdle 3 (distance 0, at the breakpoint or reading off), Breakpoint 4
//               (BPEnable and read pointer == breakpoint)
//   02 CTRL     GPReadEnable 0, BPEnable 1, FifoOverflowIntEnable 2, FifoUnderflowIntEnable 3,
//               GPLinkEnable 4, BPInt 5. The CP cause (PI bit 11) is
//               ((Breakpoint && BPInt) || (Overflow && its enable) || (Underflow && its
//               enable)) && GPReadEnable.
//   04 CLEAR    accepted, no effect (Dolphin: "intentionally don't emulate"; the watermark
//               bits are recomputed from the distance anyway); reads 0 (Dolphin's Constant)
//   06 PERF_SELECT  stored, bits 0-2 (Dolphin's DirectWrite mask 0x0007), no effect
//   20/22 FIFO base   24/26 end   28/2A high watermark   2C/2E low watermark
//   30/32 read-write distance   34/36 write pointer   38/3A read pointer   3C/3E breakpoint
//               each a LO (bits 0-15, masked 0xFFE0) / HI (bits 16-31, masked 0x03FF: the
//               GameCube's 26-bit physical mask) pair, physical.
// Any other offset (the metrics registers 0x40-0x64 included) throws BusError.
//
// Commands (OpcodeDecoding.h), in the FIFO and in display lists:
//   00 NOP (1 byte)   08 LOAD_CP_REG (1 + register + 32-bit value)
//   10 LOAD_XF_REG (1 + 32-bit word: count - 1 in bits 16-19, address in 0-15, + 4 * count;
//      bits 20-31 set throw: stricter than Dolphin, which asserts and then uses bits 16-19)   20/28/30/38 LOAD_INDX_A..D (1 + 4)
//   40 CALL_DL (1 + address + size, both & ~31): runs the list from MEM1 at once, then the
//      FIFO goes on. The address is masked & 0x3FFFFFFF first, as Dolphin's
//      Memmap::GetSpanForAddress does, so the SDK's cached (0x8...) and uncached (0xC...)
//      pointers reach their physical address. A list outside MEM1, a list inside a list (the hardware has no nesting;
//      Dolphin ignores it) or a list that ends inside a command throws BusError.
//   48 INVALIDATE_VTX_CACHE (1)   61 LOAD_BP_REG (1 + register 8 bits + value 24 bits)
//   80-BF primitives: opcode bits 3-6 the type, bits 0-2 the VAT group; 1 + 16-bit vertex
//      count + count * the vertex size of that group (VertexFormat).
// Anything else throws BusError naming the opcode and its address (FIFO or list), as does a
// vertex of a format the hardware does not define.
//
// CP registers loaded with LOAD_CP_REG (CPMemory.cpp, by the high nibble): 0x50 VCD_LO, 0x60
// VCD_HI, 0x70/0x80/0x90 VAT A/B/C (group = low 3 bits) are stored for the vertex size;
// 0x00/0x10/0x20 (performance metric commands), 0x30/0x40 (matrix indices), 0xA0/0xB0 (array
// bases and strides) are accepted, no effect: they matter only for drawing. 0xC0-0xFF throw.
// LOAD_XF_REG and LOAD_INDX_*: accepted, no effect (transform state, for drawing only).
//
// BP registers: every load is stored in a 256-entry array after the BP mask (register 0xFE)
// is applied: new = (old & ~mask) | (value & mask), and the mask goes back to 0xFFFFFF after
// any load but one of 0xFE itself (LoadBPReg). Only these have side effects (BPStructs.cpp):
//   45 draw done: low byte 0x02 raises the PE finish signal; any other value throws.
//   47 token: the PE token = value bits 0-15.   48 token with interrupt: also raises the PE
//      token signal.
//   52 EFB copy (UPE_Copy): with copy_to_xfb (bit 14) and clear (bit 11), the clear color
//      (0x4F: alpha 8-15, red 0-7; 0x50: green 8-15, blue 0-7) is written as YUV 4:2:2 (Y U Y
//      V, rgb_to_yuv) over the XFB area the copy targets: from 0x4B << 5, a row every 0x4D << 5
//      bytes, the width of the rectangle 0x49 (x 0-9, y 10-19) / 0x4A (width - 1, height - 1),
//      clamped to the 640x528 EFB, and Dolphin's height 1 + (height - 1) * yScale with yScale
//      = 256 / 0x4E when scale_invert (bit 10), else 0x4E / 256. Nothing is written when the
//      origin is outside the EFB or the stride is 0 (Dolphin copies nothing then). Every other
//      copy (to a texture, or to the XFB without clearing) is accepted, no effect: the EFB is
//      not drawn in this sub-project. The XFB area outside MEM1 throws BusError.
//   Every other BP register is store-only (drawing state).
//
// PE registers, 16 bits (a 32-bit access throws), offsets from 0x0C001000:
//   00 ZCONF  02 ALPHACONF  04 DSTALPHACONF  06 ALPHAMODE  08 ALPHAREAD: stored, no effect
//   0A CTRL   PETokenEnable 0, PEFinishEnable 1; writing 1 to bit 2 clears the token signal,
//             to bit 3 the finish signal; those two are write-only and read 0 (Dolphin).
//             PI cause 9 = token signal && PETokenEnable; cause 10 = finish signal &&
//             PEFinishEnable.
//   0E TOKEN  read-only, the last BP token.
// Any other offset (the bounding box and the performance counters) throws BusError.
class GxFifo final : public WriteGatherPipe {
public:
    static constexpr std::uint32_t kBurst = 32;

    // `mem1` is MEM1 in guest order (Memory::mem1()); the PE and CP causes go to `pi`, the
    // pipe writes at its FIFO registers.
    GxFifo(Pi& pi, std::span<std::uint8_t> mem1);
    GxFifo(const GxFifo&) = delete;
    GxFifo& operator=(const GxFifo&) = delete;

    void write(std::span<const std::uint8_t> bytes) override;
    // The PI's FIFO reset (PI 0x18 bit 0, GXAbortFrame): the gathered bytes and the CP's
    // partial command are dropped (Dolphin's ResetGatherPipe and ResetVideoBuffer).
    void reset_pipe();

    // The register blocks, for Memory::map_mmio.
    MmioHandler& cp() { return cp_regs_; }
    MmioHandler& pe() { return pe_regs_; }

    // The field in progress, and the last finished one. end_field() (the machine calls it at
    // each field start) moves the first into the second and starts again from zero.
    [[nodiscard]] const GxCounters& counters() const { return counters_; }
    [[nodiscard]] const GxCounters& last_field() const { return last_field_; }
    void end_field() {
        last_field_ = counters_;
        counters_ = {};
    }
    [[nodiscard]] const VertexFormat& vertex_format() const { return vertex_; }
    [[nodiscard]] std::uint32_t bp(std::uint8_t reg) const { return bp_[reg]; }

private:
    struct CpRegs final : MmioHandler {
        explicit CpRegs(GxFifo& f) : fifo(f) {}
        std::uint16_t read16(std::uint32_t offset) override { return fifo.cp_read16(offset); }
        std::uint32_t read32(std::uint32_t offset) override;
        void write16(std::uint32_t offset, std::uint16_t value) override { fifo.cp_write16(offset, value); }
        void write32(std::uint32_t offset, std::uint32_t value) override;
        GxFifo& fifo;
    };
    struct PeRegs final : MmioHandler {
        explicit PeRegs(GxFifo& f) : fifo(f) {}
        std::uint16_t read16(std::uint32_t offset) override { return fifo.pe_read16(offset); }
        std::uint32_t read32(std::uint32_t offset) override;
        void write16(std::uint32_t offset, std::uint16_t value) override { fifo.pe_write16(offset, value); }
        void write32(std::uint32_t offset, std::uint32_t value) override;
        GxFifo& fifo;
    };
    // Where a run of commands comes from, for the addresses in errors.
    struct Source {
        bool list;            // a display list (else the FIFO's queued blocks)
        std::uint32_t base;   // the list's address
        std::size_t origin;   // the FIFO: the run's first byte's index in pending_
    };

    std::uint16_t cp_read16(std::uint32_t offset);
    void cp_write16(std::uint32_t offset, std::uint16_t value);
    std::uint16_t pe_read16(std::uint32_t offset);
    void pe_write16(std::uint32_t offset, std::uint16_t value);

    void burst();
    // Reads and runs the FIFO while the CP may (see the class comment), then updates the CP
    // cause.
    void run_cp();
    // Runs the complete commands at the start of `data`. `pos` ends as the bytes they took (a
    // command cut short by the end of `data` is left); it moves past each command as it
    // completes, so after a throw it is the offending command's offset.
    void run_commands(std::span<const std::uint8_t> data, const Source& source, std::size_t& pos);
    // CALL_DL's two words, as written.
    void call_display_list(std::uint32_t pointer, std::uint32_t raw_size);
    void load_cp_reg(std::uint8_t reg, std::uint32_t value, std::uint32_t at);
    void load_bp_reg(std::uint8_t reg, std::uint32_t value, std::uint32_t at);
    void efb_copy(std::uint32_t copy, std::uint32_t at);
    [[nodiscard]] std::uint32_t address_of(const Source& source, std::size_t offset) const;

    // The CP register a FIFO offset (0x20-0x3E) is a half of.
    std::uint32_t& fifo_register(std::uint32_t offset);
    [[nodiscard]] bool at_breakpoint() const;
    [[nodiscard]] std::uint16_t cp_status() const;
    void update_cp_irq();
    void update_pe_irq();

    Pi& pi_;
    std::span<std::uint8_t> mem1_;
    CpRegs cp_regs_{*this};
    PeRegs pe_regs_{*this};

    std::array<std::uint8_t, kBurst> gather_{};
    std::size_t gathered_ = 0;

    // CP
    std::uint16_t cp_ctrl_ = 0, perf_select_ = 0;
    std::uint32_t base_ = 0, end_ = 0, hi_mark_ = 0, lo_mark_ = 0, distance_ = 0, wptr_ = 0, rptr_ = 0,
                  breakpoint_ = 0;
    std::vector<std::uint8_t> pending_;          // FIFO blocks read, whole, oldest first
    std::vector<std::uint32_t> pending_blocks_;  // each block's address
    std::size_t pending_pos_ = 0;                // first byte of pending_ not yet run
    VertexFormat vertex_;

    // BP and PE
    std::array<std::uint32_t, 256> bp_{};
    std::uint16_t pe_ctrl_ = 0;  // the two enables
    std::array<std::uint16_t, 5> pe_config_{};
    std::uint16_t pe_token_ = 0;
    bool token_signal_ = false, finish_signal_ = false;

    GxCounters counters_, last_field_;
};

}  // namespace ezy::gc
