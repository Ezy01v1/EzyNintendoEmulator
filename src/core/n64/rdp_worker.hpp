#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ezy::n64 {

// A single background thread that runs jobs in the order they were pushed. Jobs are
// gathered in a local batch and handed over in bulk, so the common push is lock-free;
// drain() hands over what is left and waits until everything has run.
template <class Job>
class RdpWorker {
public:
    explicit RdpWorker(std::function<void(const Job&)> run) : run_(std::move(run)), thread_([this] { loop(); }) {}
    RdpWorker(const RdpWorker&) = delete;
    RdpWorker& operator=(const RdpWorker&) = delete;
    ~RdpWorker() {
        drain();
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    void push(const Job& job) {
        batch_.push_back(job);
        if (batch_.size() >= kBatch) hand_over();
    }

    void drain() {
        hand_over();
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }

private:
    static constexpr std::size_t kBatch = 64;

    void hand_over() {
        if (batch_.empty()) return;
        {
            std::lock_guard lock(mutex_);
            queue_.insert(queue_.end(), batch_.begin(), batch_.end());
        }
        batch_.clear();
        wake_.notify_one();
    }

    void loop() {
        std::vector<Job> work;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                busy_ = false;
                if (queue_.empty()) done_.notify_all();
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stop_ with nothing left
                work.swap(queue_);
                busy_ = true;
            }
            for (const Job& job : work) run_(job);
            work.clear();
        }
    }

    std::function<void(const Job&)> run_;
    std::vector<Job> batch_;  // only touched by the pushing thread
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::vector<Job> queue_;
    bool busy_ = false, stop_ = false;
    std::thread thread_;  // last: starts after everything above exists
};

}  // namespace ezy::n64
