#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ezy::n64 {

struct RdpPacket {
    std::array<std::uint64_t, 22> words{};
    unsigned count = 0;
};

struct RdpAccess {
    struct Range { std::uint32_t begin = 0, end = 0; bool write = false; };
    std::array<Range, 2> ranges{};
    unsigned count = 0;
    static RdpAccess range(std::uint32_t begin, std::uint32_t end, bool write) {
        RdpAccess access;
        access.ranges[0] = {begin, end, write};
        access.count = 1;
        return access;
    }
};

// One producer (the emulation thread) and one or more consumers. Every consumer executes
// every packet, in order (band workers each draw their own rows). Reservation tables
// belong exclusively to the producer; acquire/release completion fences publish RAM
// writes. A packet is complete once every consumer has executed it.
class RdpWorker {
public:
    using Execute = std::function<void(const RdpPacket&)>;
    explicit RdpWorker(Execute execute, std::size_t capacity = 16384);
    explicit RdpWorker(std::vector<Execute> executes, std::size_t capacity = 16384);
    ~RdpWorker();
    RdpWorker(const RdpWorker&) = delete;
    RdpWorker& operator=(const RdpWorker&) = delete;
    // A barrier packet starts only after every consumer has finished the packets before it
    // (for state that crosses bands: a new framebuffer, a texture read from one just drawn).
    void submit(const RdpPacket& packet, const RdpAccess& access, bool barrier = false);
    void wait_memory(std::uint32_t address, std::size_t bytes, bool write) const;
    void drain() const { wait(submitted_); }
    [[nodiscard]] bool pending() const {
        return failed_.load(std::memory_order_acquire) || submitted_ > completed();
    }
    [[nodiscard]] std::size_t consumers() const { return lanes_.size(); }
    struct Stats { std::uint64_t submitted = 0, waits = 0, queue_waits = 0; };
    [[nodiscard]] Stats stats() const { return {submitted_, waits_, queue_waits_}; }

private:
    static constexpr std::uint32_t kRamSize = 8 * 1024 * 1024;
    static constexpr unsigned kPageShift = 12;
    struct Work { RdpPacket packet; std::uint64_t sequence = 0; bool barrier = false; };
    // Per consumer (each allocated apart, padded off its neighbours' cache lines): `taken`
    // once it copied a packet out of its slot, `completed` after executing it.
    struct Lane {
        std::atomic<std::uint64_t> taken{0};
        std::atomic<std::uint64_t> completed{0};
        std::array<char, 48> padding{};
        Execute execute;
        std::thread thread;
    };
    void run(std::size_t lane);
    void wait(std::uint64_t sequence) const;
    [[nodiscard]] std::uint64_t completed() const;
    [[nodiscard]] std::uint64_t taken() const;
    std::vector<Work> queue_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    // Each page names the reservation slot that last covered it; the slot holds the newest
    // packet sequence using that range. Consecutive draws into the same framebuffer then
    // only bump one slot instead of rewriting every page. Slots are recycled round-robin:
    // a stale page may see a newer sequence, which only makes a barrier more conservative.
    static constexpr unsigned kSlots = 4096, kCached = 4;
    struct Cached { std::uint32_t begin = 0, end = 0; bool write = false; std::uint16_t slot = 0; };
    std::array<std::uint16_t, (kRamSize >> kPageShift)> readers_{}, writers_{};
    std::array<std::uint64_t, kSlots> slot_sequence_{};
    std::array<Cached, kCached> cached_{};
    unsigned next_slot_ = 1, next_cached_ = 0;
    std::uint64_t submitted_ = 0;
    mutable std::uint64_t waits_ = 0;
    std::uint64_t queue_waits_ = 0;
    // The producer publishes `pushed_`. `submitted_ - min(taken)` is the number of queued
    // packets. The mutex only guards sleeping consumers and the failure pointer.
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<bool> failed_{false};
    std::atomic<int> sleepers_{0};
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::exception_ptr failure_;
};

} // namespace ezy::n64
