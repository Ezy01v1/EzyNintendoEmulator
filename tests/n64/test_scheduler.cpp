#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "n64/scheduler.hpp"

using namespace ezy::n64;

// --- Scheduler ---

namespace {

constexpr EventType kA = EventType::ViLine;
constexpr EventType kB = EventType::PiDma;

}  // namespace

TEST_CASE("scheduler pops nothing until the event is due") {
    Scheduler s;
    s.schedule_in(kA, 100);
    CHECK(s.pending(kA));
    CHECK(s.next_event_time() == 100);
    s.advance(99);
    CHECK_FALSE(s.pop_due());
    s.advance(1);
    const auto due = s.pop_due();
    REQUIRE(due);
    CHECK(due->type == kA);
    CHECK(due->when == 100);
    CHECK_FALSE(s.pending(kA));
    CHECK_FALSE(s.pop_due());
    CHECK_FALSE(s.next_event_time());
}

TEST_CASE("scheduler: rescheduling replaces, cancel removes") {
    Scheduler s;
    s.schedule(kA, 50);
    s.schedule(kA, 80);  // replaces the 50
    s.advance(60);
    CHECK_FALSE(s.pop_due());
    s.cancel(kA);
    s.advance(100);
    CHECK_FALSE(s.pop_due());
}

TEST_CASE("scheduler pops in time order, ties by type") {
    Scheduler s;
    s.schedule(kB, 30);
    s.schedule(kA, 40);
    s.advance(50);
    auto first = s.pop_due();
    REQUIRE(first);
    CHECK(first->type == kB);
    CHECK(s.pop_due()->type == kA);

    s.schedule(kB, 60);
    s.schedule(kA, 60);
    s.advance(20);
    CHECK(s.pop_due()->type == kA);  // same time: lower type first
    CHECK(s.pop_due()->type == kB);
}

TEST_CASE("scheduler: a late event keeps its original time") {
    Scheduler s;
    s.schedule(kA, 10);
    s.advance(25);  // an instruction overran the event
    const auto due = s.pop_due();
    REQUIRE(due);
    CHECK(due->when == 10);
    CHECK(s.now() == 25);
}

