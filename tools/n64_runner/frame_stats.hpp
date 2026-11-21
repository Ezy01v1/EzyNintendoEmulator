#pragma once

// Runner helpers with no N64 dependency: per-field time summary and --hold parsing.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct FrameSummary { double max_ms, p99_ms, mean_ms; int over_budget; };

// p99 is exact: the value at sorted index ceil(0.99 * n) - 1. over_budget counts values > budget_ms.
inline FrameSummary summarize_frames(std::span<const double> ms, double budget_ms) {
    if (ms.empty()) return {0, 0, 0, 0};
    std::vector<double> sorted(ms.begin(), ms.end());
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(sorted.size())));
    FrameSummary s{};
    s.max_ms = sorted.back();
    s.p99_ms = sorted[std::max<std::size_t>(rank, 1) - 1];
    s.mean_ms = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    s.over_budget = static_cast<int>(std::count_if(sorted.begin(), sorted.end(),
                                                   [&](double v) { return v > budget_ms; }));
    return s;
}

struct HoldSpec { std::uint16_t buttons; int from_field; };

// NAME[+NAME...][@FIELD]; names are case-sensitive N64 button names.
inline std::optional<HoldSpec> parse_hold(std::string_view text) {
    struct Button { std::string_view name; std::uint16_t bit; };
    static constexpr Button kButtons[] = {
        {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"Start", 0x1000}, {"Up", 0x0800}, {"Down", 0x0400},
        {"Left", 0x0200}, {"Right", 0x0100}, {"L", 0x0020}, {"R", 0x0010}, {"CU", 0x0008}, {"CD", 0x0004},
        {"CL", 0x0002}, {"CR", 0x0001}};
    HoldSpec spec{0, 0};
    if (const auto at = text.find('@'); at != std::string_view::npos) {
        const auto field = text.substr(at + 1);
        const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), spec.from_field);
        if (field.empty() || error != std::errc{} || end != field.data() + field.size() || spec.from_field < 0)
            return std::nullopt;
        text = text.substr(0, at);
    }
    while (true) {
        const auto plus = text.find('+');
        const auto name = text.substr(0, plus);
        const auto found = std::find_if(std::begin(kButtons), std::end(kButtons),
                                        [&](const Button& b) { return b.name == name; });
        if (found == std::end(kButtons)) return std::nullopt;
        spec.buttons = static_cast<std::uint16_t>(spec.buttons | found->bit);
        if (plus == std::string_view::npos) break;
        text.remove_prefix(plus + 1);
    }
    return spec;
}
