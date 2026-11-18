#include "n64/rdp_worker.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ezy::n64 {

RdpWorker::RdpWorker(Execute execute, std::size_t capacity)
    : execute_(std::move(execute)), queue_(std::max(std::size_t{1}, capacity)),
      thread_([this] { run(); }) {}

namespace {
// Waits are short (the other thread is actively working), so spin before yielding.
template <class Done>
void spin_until(Done done) {
    for (unsigned spins = 0; !done(); ++spins) {
        if (spins >= 256) std::this_thread::yield();
    }
}
}  // namespace

RdpWorker::~RdpWorker() {
    { std::lock_guard lock(mutex_); stopping_.store(true, std::memory_order_release); }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void RdpWorker::submit(const RdpPacket& packet, const RdpAccess& access) {
    const auto full = [this] {
        return submitted_ - taken_.load(std::memory_order_acquire) >= queue_.size();
    };
    if (full()) {
        ++queue_waits_;
        spin_until([&] { return !full() || failed_.load(std::memory_order_acquire); });
    }
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
        throw std::runtime_error("RDP worker stopped");
    }
    const auto sequence = ++submitted_;
    for (unsigned i = 0; i < access.count && i < access.ranges.size(); ++i) {
        const auto& range = access.ranges[i];
        const auto end = std::min(range.end, kRamSize);
        if (range.begin >= end) continue;
        const auto known = std::find_if(cached_.begin(), cached_.end(), [&](const Cached& c) {
            return c.slot != 0 && c.begin == range.begin && c.end == end && c.write == range.write;
        });
        if (known != cached_.end()) {
            slot_sequence_[known->slot] = sequence;
            continue;
        }
        const auto slot = static_cast<std::uint16_t>(next_slot_);
        next_slot_ = next_slot_ + 1 == kSlots ? 1 : next_slot_ + 1;
        // A recycled slot must not stay reachable through an older cache entry.
        for (auto& c : cached_) if (c.slot == slot) c.slot = 0;
        slot_sequence_[slot] = sequence;
        for (auto page = range.begin >> kPageShift; page <= ((end - 1) >> kPageShift); ++page) {
            (range.write ? writers_ : readers_)[page] = slot;
        }
        cached_[next_cached_] = {range.begin, end, range.write, slot};
        next_cached_ = (next_cached_ + 1) % kCached;
    }
    queue_[(sequence - 1) % queue_.size()] = {packet, sequence};
    pushed_.store(sequence, std::memory_order_seq_cst);
    // seq_cst pairs with the worker's sleeping_/pushed_ check: one side always sees the other.
    if (sleeping_.load(std::memory_order_seq_cst)) {
        { std::lock_guard lock(mutex_); }
        changed_.notify_one();
    }
}

void RdpWorker::wait(std::uint64_t sequence) const {
    if (!failed_.load(std::memory_order_acquire) && completed_.load(std::memory_order_acquire) >= sequence) return;
    ++waits_;
    spin_until([&] {
        return failed_.load(std::memory_order_acquire) || completed_.load(std::memory_order_acquire) >= sequence;
    });
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard lock(mutex_);
        if (failure_) std::rethrow_exception(failure_);
    }
}

void RdpWorker::wait_memory(std::uint32_t address, std::size_t bytes, bool write) const {
    std::uint64_t sequence = 0;
    if (address < kRamSize && bytes != 0) {
        const auto end = address + std::min(bytes, std::size_t{kRamSize - address});
        for (auto page = address >> kPageShift; page <= ((end - 1) >> kPageShift); ++page) {
            sequence = std::max(sequence, slot_sequence_[writers_[page]]);
            if (write) sequence = std::max(sequence, slot_sequence_[readers_[page]]);
        }
    }
    wait(sequence);
}

void RdpWorker::run() {
    try {
        std::uint64_t next = 1;
        const auto ready = [&] { return pushed_.load(std::memory_order_seq_cst) >= next; };
        for (;;) {
            // Packets arrive in bursts a few microseconds apart: spin briefly, then sleep.
            for (unsigned spins = 0; !ready() && spins < 4096; ++spins) {}
            if (!ready()) {
                std::unique_lock lock(mutex_);
                sleeping_.store(true, std::memory_order_seq_cst);
                changed_.wait(lock, [&] { return ready() || stopping_.load(std::memory_order_acquire); });
                sleeping_.store(false, std::memory_order_seq_cst);
                if (!ready()) return;  // stopping with an empty queue
            }
            // Copy out, then release the slot so the producer can reuse it during execution.
            const RdpPacket packet = queue_[(next - 1) % queue_.size()].packet;
            taken_.store(next, std::memory_order_release);
            execute_(packet);
            completed_.store(next, std::memory_order_release);
            ++next;
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
            stopping_.store(true, std::memory_order_release);
            failed_.store(true, std::memory_order_release);
        }
        changed_.notify_all();
    }
}

} // namespace ezy::n64
