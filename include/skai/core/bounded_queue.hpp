#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace skai {

struct QueueStats {
    std::size_t pushed = 0;
    std::size_t popped = 0;
    std::size_t dropped = 0;
    std::size_t high_water_mark = 0;
};

// Shutdown rejects new values and wakes blocked consumers. Buffered values
// remain available; pop returns an empty optional once the queue is drained.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) return false;
        queue_.push_back(std::move(value));
        ++stats_.pushed;
        if (queue_.size() > capacity_) {
            queue_.pop_front();
            ++stats_.dropped;
        }
        stats_.high_water_mark = std::max(stats_.high_water_mark, queue_.size());
        lock.unlock();
        available_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
        return take_front();
    }

    template <typename Rep, typename Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!available_.wait_for(lock, timeout, [this] { return shutdown_ || !queue_.empty(); })) {
            return std::nullopt;
        }
        return take_front();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        available_.notify_all();
    }

    // Begin a fresh lifecycle after all producers and consumers have stopped.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        stats_ = {};
        shutdown_ = false;
    }

    bool is_shutdown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const { return capacity_; }

    QueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    // Call with mutex_ held.
    std::optional<T> take_front() {
        if (queue_.empty()) return std::nullopt;
        std::optional<T> item(std::in_place, std::move(queue_.front()));
        queue_.pop_front();
        ++stats_.popped;
        return item;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<T> queue_;
    QueueStats stats_;
    bool shutdown_ = false;
};

} // namespace skai
