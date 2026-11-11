#include <doctest/doctest.h>

#include <array>
#include <future>
#include <stdexcept>
#include <vector>

#include "n64/rdp_worker.hpp"

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
