#include "n64/rdp_worker.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ezy::n64 {

RdpWorker::RdpWorker(Execute execute, std::size_t capacity)
    : execute_(std::move(execute)), queue_(std::max(std::size_t{1}, capacity)),
      thread_([this] { run(); }) {}

RdpWorker::~RdpWorker() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void RdpWorker::submit(const RdpPacket& packet, const RdpAccess& access) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return size_ < queue_.size() || stopping_; });
    if (failure_) std::rethrow_exception(failure_);
    if (stopping_) throw std::runtime_error("RDP worker stopped");
    const auto sequence = ++submitted_;
    for (unsigned i = 0; i < access.count && i < access.ranges.size(); ++i) {
        const auto& range = access.ranges[i];
        const auto end = std::min(range.end, kRamSize);
        if (range.begin >= end) continue;
        for (auto page = range.begin >> kPageShift; page <= ((end - 1) >> kPageShift); ++page) {
            (range.write ? writers_ : readers_)[page] = sequence;
        }
    }
    queue_[tail_] = {packet, sequence};
    tail_ = (tail_ + 1) % queue_.size();
    ++size_;
    lock.unlock();
    changed_.notify_all();
}

void RdpWorker::wait(std::uint64_t sequence) const {
    if (!failed_.load(std::memory_order_acquire) && completed_.load(std::memory_order_acquire) >= sequence) return;
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this, sequence] {
        return failure_ || completed_.load(std::memory_order_acquire) >= sequence;
    });
    if (failure_) std::rethrow_exception(failure_);
}

void RdpWorker::wait_memory(std::uint32_t address, std::size_t bytes, bool write) const {
    std::uint64_t sequence = 0;
    if (address < kRamSize && bytes != 0) {
        const auto end = address + std::min(bytes, std::size_t{kRamSize - address});
        for (auto page = address >> kPageShift; page <= ((end - 1) >> kPageShift); ++page) {
            sequence = std::max(sequence, writers_[page]);
            if (write) sequence = std::max(sequence, readers_[page]);
        }
    }
    wait(sequence);
}

void RdpWorker::run() {
    try {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this] { return size_ != 0 || stopping_; });
                if (size_ == 0) return;
                work = queue_[head_];
                head_ = (head_ + 1) % queue_.size();
                --size_;
            }
            changed_.notify_all();
            execute_(work.packet);
            // Store while holding the condition variable's mutex to prevent a waiter
            // from missing the notification between its predicate check and sleep.
            { std::lock_guard lock(mutex_); completed_.store(work.sequence, std::memory_order_release); }
            changed_.notify_all();
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
            stopping_ = true;
            failed_.store(true, std::memory_order_release);
        }
        changed_.notify_all();
    }
}

} // namespace ezy::n64
