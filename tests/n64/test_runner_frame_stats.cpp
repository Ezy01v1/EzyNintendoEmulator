#include <doctest/doctest.h>

#include "frame_stats.hpp"

#include <vector>

TEST_CASE("summarize_frames gives max, exact p99, mean and the count over budget") {
    std::vector<double> ms;
    for (int i = 1; i <= 100; ++i) ms.push_back(i);
    const FrameSummary s = summarize_frames(ms, 16.7);
    CHECK(s.max_ms == doctest::Approx(100));
    CHECK(s.p99_ms == doctest::Approx(99));
    CHECK(s.mean_ms == doctest::Approx(50.5));
    CHECK(s.over_budget == 84);
}

TEST_CASE("parse_hold reads names, combinations and a start field") {
    auto hold = parse_hold("A");
    REQUIRE(hold);
    CHECK(hold->buttons == 0x8000);
    CHECK(hold->from_field == 0);
    hold = parse_hold("A+B@1500");
    REQUIRE(hold);
    CHECK(hold->buttons == 0xC000);
    CHECK(hold->from_field == 1500);
    hold = parse_hold("CU+R");
    REQUIRE(hold);
    CHECK(hold->buttons == 0x0018);
    for (const char* bad : {"a", "", "A@", "A@x", "A+"}) CHECK_FALSE(parse_hold(bad));
}
