#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

template <typename T>
class ring_buffer {
public:
    explicit ring_buffer(size_t capacity)
        : capacity_(capacity), data_(capacity + 1), head_(0), tail_(0) {}

    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(tail_mutex_);

        size_t next_tail = (tail_ + 1) % data_.size();

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        data_[tail_] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        std::lock_guard<std::mutex> lock(tail_mutex_);

        size_t next_tail = (tail_ + 1) % data_.size();

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        data_[tail_] = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(head_mutex_);

        if (head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        size_t current_head = head_.load(std::memory_order_relaxed);
        T item = std::move(data_[current_head]);
        head_.store((current_head + 1) % data_.size(), std::memory_order_release);
        return item;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        if (t >= h) {
            return t - h;
        }
        return data_.size() - (h - t);
    }

    size_t capacity() const { return capacity_; }

private:
    const size_t capacity_;
    std::vector<T> data_;

#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t cache_line_size = std::hardware_destructive_interference_size;
#else
    static constexpr size_t cache_line_size = 64;
#endif

    alignas(cache_line_size) std::atomic_size_t head_;
    alignas(cache_line_size) std::atomic_size_t tail_;

    std::mutex head_mutex_;
    std::mutex tail_mutex_;
};
