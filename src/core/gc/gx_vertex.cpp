#include "gc/gx_vertex.hpp"

#include <bit>

namespace ezy::gc {

namespace {
enum : std::uint32_t { kNotPresent = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3 };

std::uint32_t field(std::uint32_t word, unsigned shift, unsigned bits) { return (word >> shift) & ((1u << bits) - 1); }

// ComponentFormat: u8/s8, u16/s16, f32; 5-7 are read as floats (Dolphin's GetElementSize).
std::uint32_t component_bytes(std::uint32_t format) { return format < 2 ? 1 : format < 4 ? 2 : 4; }

std::uint32_t index_bytes(std::uint32_t type) { return type == kIndex8 ? 1 : 2; }
}  // namespace

std::optional<std::uint32_t> VertexFormat::vertex_size(unsigned vat) const {
    const std::uint32_t a = vat_a[vat & 7], b = vat_b[vat & 7], c = vat_c[vat & 7];
    std::uint32_t size = static_cast<std::uint32_t>(std::popcount(vcd_lo & 0x1FFu));  // matrix indices

    if (const std::uint32_t t = field(vcd_lo, 9, 2); t != kNotPresent) {  // position
        size += t == kDirect ? (field(a, 0, 1) ? 3 : 2) * component_bytes(field(a, 1, 3)) : index_bytes(t);
    }
    if (const std::uint32_t t = field(vcd_lo, 11, 2); t != kNotPresent) {  // normal
        const bool nbt = field(a, 9, 1) != 0;
        if (t == kDirect) size += (nbt ? 9 : 3) * component_bytes(field(a, 10, 3));
        else size += (nbt && field(a, 31, 1) ? 3 : 1) * index_bytes(t);
    }
    for (unsigned i = 0; i < 2; ++i) {  // colors 0 and 1
        const std::uint32_t t = field(vcd_lo, 13 + 2 * i, 2);
        if (t == kNotPresent) continue;
        if (t != kDirect) {
            size += index_bytes(t);
            continue;
        }
        constexpr std::uint32_t kColorBytes[6] = {2, 3, 4, 2, 3, 4};  // 565, 888, 888x, 4444, 6666, 8888
        const std::uint32_t format = field(a, 14 + 4 * i, 3);
        if (format >= 6) return std::nullopt;
        size += kColorBytes[format];
    }
    // Each texture coordinate's elements bit and format field: (word, shift of the elements bit).
    const std::uint32_t tex_word[8] = {a, b, b, b, b, c, c, c};
    constexpr unsigned kTexShift[8] = {21, 0, 9, 18, 27, 5, 14, 23};
    for (unsigned i = 0; i < 8; ++i) {
        const std::uint32_t t = field(vcd_hi, 2 * i, 2);
        if (t == kNotPresent) continue;
        if (t != kDirect) {
            size += index_bytes(t);
            continue;
        }
        const std::uint32_t elements = field(tex_word[i], kTexShift[i], 1) ? 2 : 1;
        size += elements * component_bytes(field(tex_word[i], kTexShift[i] + 1, 3));
    }
    return size;
}

}  // namespace ezy::gc
