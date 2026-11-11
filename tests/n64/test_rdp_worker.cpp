#include <doctest/doctest.h>

#include <array>
#include <future>
#include <stdexcept>
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

TEST_CASE("threaded RDP captures XBUS words and completes writes before Sync Full") {
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
    CHECK(access.ranges[0].end == 0x4027);
    CHECK(access.ranges[1].begin == 0x6000);
    CHECK(access.ranges[1].end == 0x6013);
    CHECK(access.ranges[0].write);
    packet.words[0] = 0x3400000000000000ULL;
    access = state.access(packet);
    REQUIRE(access.count == 1);
    CHECK(access.ranges[0].begin == 0);
    CHECK(access.ranges[0].end == 8 * 1024 * 1024);
    CHECK_FALSE(access.ranges[0].write);
}
