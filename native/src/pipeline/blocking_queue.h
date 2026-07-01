#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace yamanoha {

template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    bool Push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return false;
            }
            queue_.push_back(std::move(value));
        }
        cv_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool WaitPopFor(T& value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] {
            return stopped_ || !queue_.empty();
        });

        if (queue_.empty()) {
            return false;
        }

        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        stopped_ = false;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    [[nodiscard]] size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    bool stopped_ = false;
};

} // namespace yamanoha
