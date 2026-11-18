#include <doctest/doctest.h>

#include <array>
#include <future>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#include "n64/rdp_worker.hpp"
#include "n64/rdp.hpp"
#include "common/state.hpp"

using namespace ezy::n64;

TEST_CASE("RDP worker preserves FIFO and fences writes across page boundaries") {
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::vector<unsigned> order;
    std::array<unsigned, 2> memory{};
    RdpWorker worker([&](const RdpPacket& packet) {
        if (packet.words[0] == 1) { entered.set_value(); gate.wait(); }
        order.push_back(static_cast<unsigned>(packet.words[0]));
        memory[0] = memory[1] = static_cast<unsigned>(packet.words[0]);
    });
    RdpPacket first{};
    first.words[0] = 1;
    worker.submit(first, RdpAccess::range(4094, 4102, true));
    entered.get_future().wait();
    // Neither an unrelated read nor an unrelated write waits for the blocked job.
    worker.wait_memory(8192, 8, false);
    worker.wait_memory(12288, 8, true);
    CHECK(worker.pending());
    auto waiter = std::async(std::launch::async, [&] {
        worker.wait_memory(4096, 4, false);
        return memory[1];
    });
    release.set_value();
    CHECK(waiter.get() == 1);
    RdpPacket second{};
    second.words[0] = 2;
    worker.submit(second, RdpAccess::range(0, 8, true));
    worker.drain();
    CHECK(order == std::vector<unsigned>{1, 2});
    CHECK_FALSE(worker.pending());
}

TEST_CASE("RDP read reservations allow readers and fence main-thread writers") {
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    unsigned value = 17, observed = 0;
    RdpWorker worker([&](const RdpPacket&) {
        entered.set_value(); gate.wait(); observed = value;
    });
    worker.submit({}, RdpAccess::range(0, 4096, false));
    entered.get_future().wait();
    worker.wait_memory(0, 4, false);  // Concurrent read is safe.
    CHECK(value == 17);
    auto writer = std::async(std::launch::async, [&] {
        worker.wait_memory(0, 4, true);
        value = 23;
    });
    release.set_value();
    writer.get();
    worker.drain();
    CHECK(observed == 17);
    CHECK(value == 23);
}

TEST_CASE("RDP worker handles bounded queues and propagates execution failures") {
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    unsigned calls = 0;
    RdpWorker worker([&](const RdpPacket&) {
        if (++calls == 1) { entered.set_value(); gate.wait(); }
        else throw std::runtime_error("raster failure");
    }, 1);
    worker.submit({}, {});
    entered.get_future().wait();
    worker.submit({}, {}); // The single queue slot is now full.
    auto producer = std::async(std::launch::async, [&] {
        try { worker.submit({}, {}); worker.drain(); }
        catch (const std::runtime_error& e) { return std::string(e.what()); }
        return std::string{};
    });
    release.set_value();
    CHECK(producer.get() == "raster failure");
    CHECK_THROWS_AS(worker.drain(), std::runtime_error);
    CHECK_THROWS_AS(worker.wait_memory(0, 1, false), std::runtime_error);
}

TEST_CASE("threaded RDP captures XBUS words and exposes writes to observers after Sync Full") {
    std::vector<std::uint8_t> ram(8 * 1024 * 1024);
    std::array<std::uint8_t, 4096> dmem{};
    Rdp rdp;
    REQUIRE(rdp.enable_threaded(ram));
    Scheduler scheduler;
    Mi mi;
    const std::array<std::uint64_t, 6> commands{
        0x3F18000300004000ULL, // 32-bit color, stride 4
        0x2F30000000000000ULL, // fill
        0x2D00000000010004ULL, // scissor (0,0)..(4,1)
        0x3700000012345678ULL,
        0x3600C00000000000ULL, // inclusive rectangle (0,0)..(3,0)
        0x2900000000000000ULL,
    };
    for (std::size_t w = 0; w < commands.size(); ++w)
        for (unsigned byte = 0; byte < 8; ++byte)
            dmem[w * 8 + byte] = static_cast<std::uint8_t>(commands[w] >> (56 - byte * 8));
    rdp.write(12, 2, scheduler, 0); // XBUS
    rdp.write(0, 0, scheduler, 0);
    rdp.write(4, static_cast<std::uint32_t>(commands.size() * 8), scheduler, 0);
    for (unsigned w = 0; w < commands.size(); ++w) {
        scheduler.advance(3);
        const auto event = scheduler.pop_due();
        REQUIRE(event);
        rdp.step(mi, scheduler, ram, dmem, event->when);
        CHECK(((mi.read(8) & 0x20) != 0) == (w + 1 == commands.size()));
        // The source can be overwritten as soon as its word has been consumed.
        std::fill_n(dmem.begin() + w * 8, 8, std::uint8_t{0});
    }
    // Sync Full does not block on the worker; observers wait on their own ranges.
    rdp.prepare_memory(0x4000, 16, false);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(ram[0x4000 + i] == static_cast<std::uint8_t>(0x12345678U >> ((3 - (i & 3)) * 8)));
    CHECK(rdp.read(8, scheduler.now()) == 48);
    rdp.drain();
    ezy::StateWriter writer;
    rdp.serialize(writer, 11);
    const auto state = writer.take();
    Rdp synchronous;
    ezy::StateReader reader(state);
    synchronous.serialize(reader, 11);
    REQUIRE_FALSE(reader.failed());
    ezy::StateWriter again;
    synchronous.serialize(again, 11);
    CHECK(again.take() == state);
}

namespace {
constexpr std::uint64_t rect(std::uint64_t op, unsigned hi_x, unsigned hi_y, unsigned lo_x, unsigned lo_y) {
    return (op << 56) | (std::uint64_t{hi_x * 4} << 44) | (std::uint64_t{hi_y * 4} << 32) |
           (std::uint64_t{lo_x * 4} << 12) | (lo_y * 4);
}

// Feeds commands through XBUS (DMEM) one word per RDP step, then waits for the raster threads.
void run_xbus(Rdp& rdp, std::vector<std::uint8_t>& ram, const std::vector<std::uint64_t>& commands) {
    std::array<std::uint8_t, 4096> dmem{};
    REQUIRE(commands.size() * 8 <= dmem.size());
    for (std::size_t w = 0; w < commands.size(); ++w)
        for (unsigned byte = 0; byte < 8; ++byte)
            dmem[w * 8 + byte] = static_cast<std::uint8_t>(commands[w] >> (56 - byte * 8));
    Scheduler scheduler;
    Mi mi;
    rdp.write(12, 2, scheduler, 0);  // XBUS
    rdp.write(0, 0, scheduler, 0);
    rdp.write(4, static_cast<std::uint32_t>(commands.size() * 8), scheduler, 0);
    for (std::size_t w = 0; w < commands.size(); ++w) {
        scheduler.advance(3);
        const auto event = scheduler.pop_due();
        REQUIRE(event);
        rdp.step(mi, scheduler, ram, dmem, event->when);
    }
    rdp.drain();
}
}  // namespace

TEST_CASE("banded threaded RDP draws the same pixels, including render to texture") {
    // Three bands split the 16 rows in groups of 4. Image A is filled in three colors, then
    // loaded as a texture and copied 1:1 into image B: the load must see rows every band drew.
    constexpr std::uint64_t image_a = 0x3F10000F00004000ULL, image_b = 0x3F10000F00008000ULL;  // RGBA16, 16 wide
    const std::uint64_t one_cycle = (0x2FULL << 56) | (6ULL << 41) | (3ULL << 38) | (1U << 9);
    std::uint64_t texel0 = 0x3CULL << 56;  // both cycles: (0 - 0) * 0 + TEXEL0, alpha too
    for (const auto [shift, value] : std::initializer_list<std::pair<unsigned, unsigned>>{
             {52, 15}, {47, 31}, {44, 7}, {41, 7}, {28, 15}, {15, 1}, {12, 7}, {9, 1},
             {37, 15}, {32, 31}, {21, 7}, {18, 7}, {24, 15}, {6, 1}, {3, 7}, {0, 1}}) {
        texel0 |= std::uint64_t{value} << shift;
    }
    const std::vector<std::uint64_t> commands{
        image_a,
        0x2F30000000000000ULL,  // fill mode
        (0x2DULL << 56) | (64ULL << 12) | 64,  // scissor (0,0)..(16,16)
        0x37000000F801F801ULL, rect(0x36, 15, 4, 0, 0),
        0x3700000007C107C1ULL, rect(0x36, 15, 9, 0, 5),
        0x37000000003F003FULL, rect(0x36, 15, 15, 0, 10),
        image_b,
        one_cycle,
        texel0,
        0x3D10000F00004000ULL,  // texture image: A
        (0x35ULL << 56) | (2ULL << 51) | (4ULL << 41),  // tile 0: RGBA16, 4 words per row
        rect(0x34, 0, 0, 15, 15),  // load tile (sl, tl, sh, th)
        rect(0x32, 0, 0, 15, 15),  // tile size
        rect(0x24, 16, 16, 0, 0), (1024ULL << 16) | 1024,  // texrect, one texel per pixel
    };
    std::vector<std::uint8_t> sync_ram(8 * 1024 * 1024), banded_ram(8 * 1024 * 1024);
    Rdp synchronous, banded;
    REQUIRE(banded.enable_threaded(banded_ram, 3));
    REQUIRE(banded.bands() == 3);
    run_xbus(synchronous, sync_ram, commands);
    run_xbus(banded, banded_ram, commands);
    for (std::uint32_t at = 0x4000; at < 0x4200; ++at) REQUIRE(banded_ram[at] == sync_ram[at]);
    for (std::uint32_t at = 0x8000; at < 0x8200; ++at) REQUIRE(banded_ram[at] == sync_ram[at]);
    CHECK(sync_ram[0x4000] == 0xF8);  // row 0: first fill color
    CHECK(sync_ram[0x4000 + 15 * 32] == 0x00);  // row 15: third color, 0x003F
    CHECK(sync_ram[0x4000 + 15 * 32 + 1] == 0x3F);
    for (std::uint32_t i = 0; i < 0x200; i += 2) {  // B is A copied (opaque texels)
        CAPTURE(i);
        CHECK(sync_ram[0x8000 + i] == sync_ram[0x4000 + i]);
    }
}

TEST_CASE("RDP reservations include unaligned targets and X beyond the row stride") {
    RdpMemoryState state;
    state.color_address = 0x4003;
    state.depth_address = 0x6001;
    state.width = 1;
    state.pixel_size = 3;
    state.scissor = (32ULL << 12) | 8; // eight pixels, two rows
    state.modes = 1U << 4;
    RdpPacket packet;
    packet.words[0] = 0x0800000000000000ULL;
    auto access = state.access(packet);
    REQUIRE(access.count == 2);
    CHECK(access.ranges[0].begin == 0x4000);
    CHECK(access.ranges[0].end >= 0x4027);
    CHECK(access.ranges[1].begin == 0x6000);
    CHECK(access.ranges[1].end >= 0x6013);
    CHECK(access.ranges[0].write);
}

TEST_CASE("RDP texture load reservations cover the source texels they read") {
    RdpMemoryState state;
    // 16-bit texture image at 0x10000, 64 texels wide.
    state.apply((0x3DULL << 56) | (2ULL << 51) | (63ULL << 32) | 0x10000);
    RdpPacket packet;
    // Load Tile of texels (0,0)-(15,3) in 10.2 coordinates.
    packet.words[0] = (0x34ULL << 56) | ((15ULL << 2) << 12) | (3ULL << 2);
    auto access = state.access(packet);
    REQUIRE(access.count == 1);
    CHECK_FALSE(access.ranges[0].write);
    CHECK(access.ranges[0].begin == 0x10000);
    // The last row's final 64-bit word ends at texel 3 * 64 + 19.
    CHECK(access.ranges[0].end >= 0x10000 + (3 * 64 + 20) * 2);
    CHECK(access.ranges[0].end < 0x10000 + 4096);

    // Load Block of 256 texels from (8, 2) reads a run starting at 2 * 64 + 8.
    packet.words[0] = (0x33ULL << 56) | (8ULL << 44) | (2ULL << 32) | (263ULL << 12);
    access = state.access(packet);
    REQUIRE(access.count == 1);
    CHECK(access.ranges[0].begin <= 0x10000 + (2 * 64 + 8) * 2);
    CHECK(access.ranges[0].end >= 0x10000 + (2 * 64 + 8 + 256 + 4) * 2);

    // Load TLUT of entries 0-15 on row 0.
    packet.words[0] = (0x30ULL << 56) | ((15ULL << 2) << 12);
    access = state.access(packet);
    REQUIRE(access.count == 1);
    CHECK(access.ranges[0].begin == 0x10000);
    CHECK(access.ranges[0].end >= 0x10000 + 16 * 2);

    // A source that could wrap the 24-bit address reserves all of RDRAM.
    state.apply((0x3DULL << 56) | (2ULL << 51) | (1023ULL << 32) | 0xFFF000);
    packet.words[0] = (0x34ULL << 56) | ((15ULL << 2) << 12) | (3ULL << 2);
    access = state.access(packet);
    REQUIRE(access.count == 1);
    CHECK(access.ranges[0].begin == 0);
    CHECK(access.ranges[0].end == 8 * 1024 * 1024);
}

TEST_CASE("RDP reservations cover inclusive COPY and fill edges on a page boundary") {
    RdpMemoryState state;
    state.color_address = 0x0FF8;
    state.width = 1;
    state.pixel_size = 2;
    state.scissor = (16ULL << 12) | 4;
    for (const auto op : {0x24ULL, 0x08ULL}) {
        RdpPacket packet;
        packet.words[0] = op << 56;
        const auto access = state.access(packet);
        REQUIRE(access.count == 1);
        // These paths may write the pixel at x = floor(scissor_right / 4).
        CHECK(access.ranges[0].begin <= 0x1000);
        CHECK(access.ranges[0].end >= 0x1002);
    }
}

TEST_CASE("RDP worker destruction completes all published packets") {
    unsigned completed = 0;
    {
        RdpWorker worker([&](const RdpPacket&) { ++completed; }, 2);
        for (unsigned i = 0; i < 100; ++i) worker.submit({}, {});
    }
    CHECK(completed == 100);
}
