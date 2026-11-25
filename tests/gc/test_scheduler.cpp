#include <doctest/doctest.h>

#include "gc/scheduler.hpp"

using ezy::gc::EventType;
using ezy::gc::Scheduler;

TEST_CASE("the GameCube scheduler pops events in time order at absolute cycles") {
    Scheduler s;
    CHECK_FALSE(s.next_event_time().has_value());
    CHECK_FALSE(s.pop_due().has_value());

    s.schedule(EventType::DiTransfer, 100);
    s.schedule_in(EventType::ViLine, 50);
    CHECK(s.pending(EventType::ViLine));
    CHECK(s.next_event_time() == 50u);

    s.advance(49);
    CHECK_FALSE(s.pop_due().has_value());
    s.advance(1);
    auto due = s.pop_due();
    REQUIRE(due.has_value());
    CHECK(due->type == EventType::ViLine);
    CHECK(due->when == 50);
    CHECK_FALSE(s.pending(EventType::ViLine));
    CHECK_FALSE(s.pop_due().has_value());

    // Late: both due, the earlier first; ties go to the lower type.
    s.schedule(EventType::ViLine, 100);
    s.advance_to(150);
    due = s.pop_due();
    REQUIRE(due.has_value());
    CHECK(due->type == EventType::ViLine);
    due = s.pop_due();
    REQUIRE(due.has_value());
    CHECK(due->type == EventType::DiTransfer);
    CHECK(due->when == 100);
    CHECK_FALSE(s.pop_due().has_value());

    // Rescheduling replaces; cancel removes.
    s.schedule(EventType::ViLine, 200);
    s.schedule(EventType::ViLine, 300);
    s.schedule(EventType::DiTransfer, 250);
    s.cancel(EventType::DiTransfer);
    s.advance_to(299);
    CHECK_FALSE(s.pop_due().has_value());
    CHECK(s.next_event_time() == 300u);
    s.advance_to(300);
    due = s.pop_due();
    REQUIRE(due.has_value());
    CHECK(due->when == 300);
    CHECK(s.now() == 300);
}
