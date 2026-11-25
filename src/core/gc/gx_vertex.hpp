#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace ezy::gc {

// The vertex layout the CP's VCD and VAT registers describe, and how many bytes one vertex
// takes in a primitive command (design, "FIFO gráfico": a primitive's size comes from
// VCD/VAT). No vertex is decoded: this sub-project only needs the size.
//
// Bit fields from Dolphin's VideoCommon/CPMemory.h (TVtxDesc, UVAT_group0-2); sizes from
// VertexLoaderBase::GetVertexSize and the VertexLoader_* readers (master, read 2026-11-23):
//   VCD_LO  PosMatIdx bit 0 and Tex0-7MatIdx bits 1-8 (one byte each when set), Position
//           9-10, Normal 11-12, Color0 13-14, Color1 15-16
//   VCD_HI  TexNCoord bits 2N..2N+1
//           every 2-bit field: 0 not present, 1 direct, 2 index8 (1 byte), 3 index16 (2 bytes)
//   VAT_A   PosElements 0 (xy / xyz), PosFormat 1-3, NormalElements 9 (N / NBT), NormalFormat
//           10-12, Color0Comp 14-16, Color1Comp 18-20, Tex0 elements 21 (s / st) and format
//           22-24, NormalIndex3 31
//   VAT_B   Tex1 elements 0 / format 1-3, Tex2 9 / 10-12, Tex3 18 / 19-21, Tex4 27 / 28-30
//   VAT_C   Tex5 5 / 6-8, Tex6 14 / 15-17, Tex7 23 / 24-26
// Direct data: components of u8/s8 take 1 byte, u16/s16 2, f32 4 (formats 5-7 are read as
// floats, Dolphin's GetElementSize); a position has 2 or 3 components, a normal 3 (N) or 9
// (NBT), a texture coordinate 1 or 2. A direct color takes RGB565 2, RGB888 3, RGB888x 4,
// RGBA4444 2, RGBA6666 3, RGBA8888 4 bytes (formats 6-7 do not exist). An indexed normal takes
// 3 indices when it is NBT with NormalIndex3, else 1.
struct VertexFormat {
    std::uint32_t vcd_lo = 0, vcd_hi = 0;
    std::array<std::uint32_t, 8> vat_a{}, vat_b{}, vat_c{};  // one per VAT group

    // The bytes of one vertex of VAT group `vat` (0-7). nullopt when a present attribute has
    // a format the hardware does not define (a direct color of format 6 or 7).
    [[nodiscard]] std::optional<std::uint32_t> vertex_size(unsigned vat) const;
};

}  // namespace ezy::gc
