#pragma once

// Diagnostic build only. Thread-local storage keeps worker-mode use race-free;
// the runner collects only synchronous RDP work on its own thread.
#if EZY_N64_RDP_COUNTERS
#include <array>
#include <cstdint>
#include <ostream>
#include <map>
#include <tuple>

namespace ezy::n64::rdp_counters {
struct JointCounts { std::uint64_t primitives = 0, attempts = 0, survived = 0, full_coverage = 0; };
using JointKey = std::tuple<unsigned, unsigned, unsigned, std::uint64_t>;
struct Counts {
    std::map<JointKey, JointCounts> joint;
    std::map<std::tuple<unsigned, unsigned, unsigned, unsigned>, std::uint64_t> rectangle_bounds;
    std::map<std::tuple<unsigned, unsigned, bool, std::uint64_t, std::uint64_t>, std::uint64_t> triangle_modes;
    std::array<std::uint64_t, 2> constant_rectangle{}; // eligible primitives, stamped pixel events
    std::array<std::uint64_t, 4> pixel_cycle{}, pixel_size{}, depth_mode{}, texture_size{}, final_blend_source{};
    std::array<std::uint64_t, 2> image_read{}, force_blend{}, blend_bypass{}, texture_filter{}, texture_palette{};
    // pixel_exit: attempts, alpha rejection, zero-coverage rejection.
    // texture_palette: bit 46 of other modes (palette type, not TLUT enable).
    std::array<std::uint64_t, 8> pixel_exit{}, texture_format{};
};
inline thread_local Counts counts;
inline thread_local unsigned primitive_kind = 0;
struct PrimitiveScope {
    unsigned previous;
    PrimitiveScope(unsigned kind, unsigned cycle, unsigned size, std::uint64_t modes)
        : previous(primitive_kind) {
        primitive_kind = kind;
        ++counts.joint[{kind, cycle, size, modes}].primitives;
    }
    ~PrimitiveScope() { primitive_kind = previous; }
};
inline void observe(unsigned cycle, unsigned size, std::uint64_t modes, bool survived, unsigned coverage) {
    auto& row = counts.joint[{primitive_kind, cycle, size, modes}];
    if (!survived) ++row.attempts;
    else {
        ++row.survived;
        row.full_coverage += coverage == 8;
    }
}
inline void reset() { counts = {}; }
inline void write(std::ostream& out) {
    out << "RDP counters (measured fields only; instrumented timing is not a benchmark):\n";
    const auto row = [&](const char* name, const auto& values) {
        for (std::size_t i = 0; i < values.size(); ++i)
            out << "RdpCount," << name << ',' << i << ',' << values[i] << '\n';
    };
#define EZY_RDP_ROW(name) row(#name, counts.name)
    EZY_RDP_ROW(pixel_cycle); EZY_RDP_ROW(pixel_size); EZY_RDP_ROW(depth_mode);
    EZY_RDP_ROW(texture_size); EZY_RDP_ROW(final_blend_source);
    EZY_RDP_ROW(image_read); EZY_RDP_ROW(force_blend); EZY_RDP_ROW(blend_bypass);
    EZY_RDP_ROW(texture_filter); EZY_RDP_ROW(texture_palette);
    EZY_RDP_ROW(pixel_exit); EZY_RDP_ROW(texture_format);
    EZY_RDP_ROW(constant_rectangle);
#undef EZY_RDP_ROW
    out << "RdpJoint,Kind,Cycle,Size,Modes,Primitives,Attempts,Survived,FullCoverage\n";
    for (const auto& [key, value] : counts.joint) {
        const auto& [kind, cycle, size, modes] = key;
        out << "RdpJoint," << kind << ',' << cycle << ',' << size << ',' << modes << ','
            << value.primitives << ',' << value.attempts << ',' << value.survived << ',' << value.full_coverage << '\n';
    }
    out << "RdpBounds,Left,Top,Right,Bottom,Primitives\n";
    for (const auto& [bounds, count] : counts.rectangle_bounds) {
        const auto& [left, top, right, bottom] = bounds;
        out << "RdpBounds," << left << ',' << top << ',' << right << ',' << bottom << ',' << count << '\n';
    }
    out << "RdpTriangle,Opcode,Texels,UniformShade,Combine,Modes,Primitives\n";
    for (const auto& [key, count] : counts.triangle_modes) {
        const auto& [op, texels, uniform, combine, modes] = key;
        out << "RdpTriangle," << op << ',' << texels << ',' << uniform << ',' << combine << ',' << modes << ',' << count << '\n';
    }
}
}
#define EZY_RDP_COUNT(name, index) (++::ezy::n64::rdp_counters::counts.name[index])
#define EZY_RDP_ADD(name, index, value) (::ezy::n64::rdp_counters::counts.name[index] += (value))
#define EZY_RDP_PRIMITIVE(kind) ::ezy::n64::rdp_counters::PrimitiveScope primitive_scope(kind, cycle_type_, pixel_size_, solid_.modes)
#define EZY_RDP_OBSERVE(survived, coverage) ::ezy::n64::rdp_counters::observe(cycle_type_, pixel_size_, solid_.modes, survived, coverage)
#define EZY_RDP_RECT_BOUNDS(left, top, right, bottom) (++::ezy::n64::rdp_counters::counts.rectangle_bounds[{left, top, right, bottom}])
#define EZY_RDP_TRIANGLE(op, texels, uniform) (++::ezy::n64::rdp_counters::counts.triangle_modes[{op, texels, uniform, solid_.combine, solid_.modes}])
#else
#define EZY_RDP_COUNT(name, index) ((void)0)
#define EZY_RDP_ADD(name, index, value) ((void)0)
#define EZY_RDP_PRIMITIVE(kind) ((void)0)
#define EZY_RDP_OBSERVE(survived, coverage) ((void)0)
#define EZY_RDP_RECT_BOUNDS(left, top, right, bottom) ((void)0)
#define EZY_RDP_TRIANGLE(op, texels, uniform) ((void)0)
#endif
