#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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

// One producer (the emulation thread), one consumer. Reservation tables belong
// exclusively to the producer; acquire/release completion fences publish RAM writes.
class RdpWorker {
public:
    using Execute = std::function<void(const RdpPacket&)>;
    explicit RdpWorker(Execute execute, std::size_t capacity = 1024);
    ~RdpWorker();
    RdpWorker(const RdpWorker&) = delete;
    RdpWorker& operator=(const RdpWorker&) = delete;
    void submit(const RdpPacket& packet, const RdpAccess& access);
    void wait_memory(std::uint32_t address, std::size_t bytes, bool write) const;
    void drain() const { wait(submitted_); }
    [[nodiscard]] bool pending() const {
        return failed_.load(std::memory_order_acquire) || submitted_ > completed_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::uint32_t kRamSize = 8 * 1024 * 1024;
    static constexpr unsigned kPageShift = 12;
    struct Work { RdpPacket packet; std::uint64_t sequence = 0; };
    void run();
    void wait(std::uint64_t sequence) const;
    Execute execute_;
    std::vector<Work> queue_;
    std::array<std::uint64_t, (kRamSize >> kPageShift)> readers_{}, writers_{};
    std::uint64_t submitted_ = 0;
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<bool> failed_{false};
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::size_t head_ = 0, tail_ = 0, size_ = 0;
    bool stopping_ = false;
    std::exception_ptr failure_;
    std::thread thread_;
};

} // namespace ezy::n64
