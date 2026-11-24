#include <doctest/doctest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "disc_builder.hpp"
#include "gc/gamecube.hpp"
#include "gc/pi.hpp"
#include "gc_run.hpp"

using namespace ezy::gc;

namespace {
constexpr std::uint64_t kField = 8'108'100;  // 486 MHz / 59.94 Hz
constexpr std::uint32_t kLoop = DiscBuilder::kTextAddress + 6 * 4;  // the synthetic program's b 0
}  // namespace

TEST_CASE("the runner boots the synthetic disc and reports its stats") {
    DiscBuilder builder;
    const auto image = builder.write("runner");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    gc->set_pc_trace(4);

    std::ostringstream out;
    const ezy::gc_runner::RunReport report = ezy::gc_runner::run_fields(*gc, 3, out, 2);
    CHECK(report.exit_code == 0);
    CHECK(report.fields_run == 3);
    CHECK(gc->stats().fields == 3);
    CHECK(gc->stats().instructions == 3 * kField);  // the program spins at 1 cycle per instruction

    // A stats line every `print_every` fields (2 here; the runner's is 60) and one at the end,
    // each naming its field.
    const std::string text = out.str();
    CAPTURE(text);
    CHECK(text.find("field 2:") != std::string::npos);
    CHECK(text.find("field 3:") != std::string::npos);
    CHECK(text.find("field 1:") == std::string::npos);
    CHECK(text.find("instructions 24324300") != std::string::npos);  // 3 fields
    CHECK(text.find("fields with FIFO data: 0 of 3") != std::string::npos);
    CHECK(text.find("DSP mails 0") != std::string::npos);
    // The criterion's per-field counts: the synthetic program writes no FIFO data and enables
    // no VI interrupt.
    CHECK(report.fields_with_fifo_data == 0);
    CHECK(report.fields_with_vi_interrupt == 0);

    // The PC trace keeps the last 4 PCs, oldest first: the loop, over and over.
    const std::vector<std::uint32_t> pcs = gc->recent_pcs();
    CHECK(pcs == std::vector<std::uint32_t>{kLoop, kLoop, kLoop, kLoop});
}

TEST_CASE("the runner stops on an unimplemented instruction with the pc, the word and the last pcs") {
    DiscBuilder builder;
    builder.put32(DiscBuilder::kDolOffset + 0x100 + 6 * 4, 0x04000000);  // primary opcode 1: not an instruction
    const auto image = builder.write("runner_stop");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    gc->set_pc_trace(3);

    std::ostringstream out;
    const ezy::gc_runner::RunReport report = ezy::gc_runner::run_fields(*gc, 10, out);
    CHECK(report.exit_code == 1);
    CHECK(report.fields_run == 0);
    const std::string text = out.str();
    CAPTURE(text);
    CHECK(text.find("pc 0x80003118") != std::string::npos);
    CHECK(text.find("word/address 0x04000000") != std::string::npos);
    // The last 3 PCs, oldest first, ending at the one that failed.
    const auto trace = text.find("last 3 pcs, oldest first:\n");
    REQUIRE(trace != std::string::npos);
    const auto at_14 = text.find("    0x80003110\n", trace);
    const auto at_18 = text.find("    0x80003118\n", trace);
    CHECK(text.find("    0x80003114\n", trace) != std::string::npos);
    REQUIRE(at_14 != std::string::npos);
    REQUIRE(at_18 != std::string::npos);
    CHECK(at_14 < at_18);
    CHECK(text.find("0x8000310C") == std::string::npos);  // older than the trace's depth
    CHECK(gc->recent_pcs() == std::vector<std::uint32_t>{0x80003110, 0x80003114, 0x80003118});
}

TEST_CASE("the PC trace is empty until set and holds only what ran when shorter than its depth") {
    DiscBuilder builder;
    builder.put32(DiscBuilder::kDolOffset + 0x100 + 6 * 4, 0x04000000);
    const auto image = builder.write("runner_trace");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    CHECK(gc->recent_pcs().empty());
    gc->set_pc_trace(64);
    CHECK_THROWS_AS(gc->run_field(), GekkoError);
    // The 7 instructions of the program, the last one the one that failed.
    std::vector<std::uint32_t> expected;
    for (std::uint32_t i = 0; i < 7; ++i) expected.push_back(DiscBuilder::kTextAddress + 4 * i);
    CHECK(gc->recent_pcs() == expected);
}

TEST_CASE("the runner counts the fields in which the VI raised its interrupt") {
    DiscBuilder builder;
    const auto image = builder.write("runner_vi");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    // DI0 at line 1 of every field (DI1 starts pending, as Dolphin's Preset: cleared first).
    // Nobody acknowledges it, so the cause rises once.
    gc->memory().write32(0x0C002034, 0);
    gc->memory().write32(0x0C002030, 0x10000000 | (1u << 16));
    std::ostringstream out;
    const ezy::gc_runner::RunReport report = ezy::gc_runner::run_fields(*gc, 3, out);
    CHECK(report.exit_code == 0);
    CHECK(report.fields_with_vi_interrupt == 1);
    CHECK(gc->stats().interrupts[Pi::kVi] == 1);
}

TEST_CASE("the criterion streak counts the longest run of fields with FIFO data and a VI interrupt") {
    ezy::gc_runner::CriterionStreak streak;
    const bool fifo[] = {false, true, true, true, false, true, true};
    const bool vi[] = {true, true, true, true, true, true, false};
    for (int i = 0; i < 7; ++i) streak.add(i + 1, fifo[i], vi[i]);
    CHECK(streak.longest == 3);
    CHECK(streak.longest_last_field == 4);  // fields 2-4
    CHECK(streak.current == 0);             // field 7 had no VI interrupt
    streak.add(8, true, true);
    CHECK(streak.current == 1);
    CHECK(streak.longest == 3);
}

TEST_CASE("the runner prints only the final stats when print_every is 0") {
    DiscBuilder builder;
    const auto image = builder.write("runner_print0");
    auto gc = GameCube::create(image);
    REQUIRE(gc != nullptr);
    std::ostringstream out;
    const ezy::gc_runner::RunReport report = ezy::gc_runner::run_fields(*gc, 2, out, 0);
    CHECK(report.exit_code == 0);
    const std::string text = out.str();
    CHECK(text.find("field 1:") == std::string::npos);
    CHECK(text.find("field 2:") != std::string::npos);
}
