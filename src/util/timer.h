#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <queue>
#include <unordered_map>
#include <utility>

class timer {
public:
    using timeout_callback = std::function<void()>;
    using clock_type = std::chrono::steady_clock;
    using time_point = std::chrono::time_point<clock_type>;

    timer() = default;
    ~timer() = default;

    void add(int id, int timeout_ms, timeout_callback callback) {
        if (id < 0) {
            return;
        }

        const time_point expire = clock_type::now() + std::chrono::milliseconds(timeout_ms);
        entry& e = entries_[id];
        ++e.version;
        e.expires_at = expire;
        e.callback = std::move(callback);
        heap_.push(node{id, expire, e.version});
    }

    void adjust(int id, int timeout_ms) {
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            return;
        }

        entry& e = it->second;
        ++e.version;
        e.expires_at = clock_type::now() + std::chrono::milliseconds(timeout_ms);
        heap_.push(node{id, e.expires_at, e.version});
    }

    void remove(int id) { entries_.erase(id); }

    void tick() {
        while (true) {
            discard_stale_top();
            if (heap_.empty()) {
                break;
            }

            const node current = heap_.top();
            if (current.expires_at > clock_type::now()) {
                break;
            }

            auto it = entries_.find(current.id);
            if (it == entries_.end()) {
                heap_.pop();
                continue;
            }

            timeout_callback callback = it->second.callback;
            entries_.erase(it);
            heap_.pop();
            if (callback) {
                callback();
            }
        }
    }

    int get_next_timeout_ms() {
        tick();
        discard_stale_top();
        if (heap_.empty()) {
            return -1;
        }

        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            heap_.top().expires_at - clock_type::now());
        return diff.count() > 0 ? static_cast<int>(diff.count()) : 0;
    }

    bool contains(int id) const { return entries_.find(id) != entries_.end(); }

    bool empty() const { return entries_.empty(); }

    std::size_t size() const { return entries_.size(); }

private:
    struct node {
        int id;
        time_point expires_at;
        std::size_t version;

        bool operator>(const node& other) const { return expires_at > other.expires_at; }
    };

    struct entry {
        time_point expires_at{clock_type::now()};
        timeout_callback callback;
        std::size_t version = 0;
    };

    void discard_stale_top() {
        while (!heap_.empty()) {
            const node& top = heap_.top();
            auto it = entries_.find(top.id);
            if (it != entries_.end() && it->second.version == top.version &&
                it->second.expires_at == top.expires_at) {
                break;
            }
            heap_.pop();
        }
    }

    std::priority_queue<node, std::vector<node>, std::greater<node>> heap_;
    std::unordered_map<int, entry> entries_;
};