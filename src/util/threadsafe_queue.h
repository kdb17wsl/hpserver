#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

template <typename T>
class threadsafe_queue {
public:
    threadsafe_queue() : head(new node), tail(head.get()) {}
    threadsafe_queue(const threadsafe_queue& other) = delete;
    threadsafe_queue& operator=(const threadsafe_queue& other) = delete;

    bool try_pop(T& res) {
        auto old_head = try_pop_head(res);
        return old_head != nullptr;
    }

    void push(T val) {
        auto new_data = std::make_shared<T>(std::move(val));
        auto p = std::make_unique<node>();

        std::unique_lock<std::mutex> tail_lock(tail_mtx);
        tail->data = new_data;
        auto new_tail = p.get();
        tail->next = std::move(p);
        tail = new_tail;
        tail_lock.unlock();

        data_cond.notify_one();
    }

    bool empty() const {
        std::lock_guard<std::mutex> head_lock(head_mtx);
        return (head.get() == get_tail());
    }

    void wait_pop(T& res) { std::unique_ptr<node> old_head = wait_pop_head(res); }

private:
    mutable std::mutex head_mtx;
    mutable std::mutex tail_mtx;

    struct node {
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
    };

    std::unique_ptr<node> head;
    node* tail;
    std::condition_variable data_cond;

    node* get_tail() const {
        std::lock_guard<std::mutex> tail_lock(tail_mtx);
        return tail;
    }

    std::unique_ptr<node> pop_head() {
        std::unique_ptr<node> old_head = std::move(head);
        head = std::move(old_head->next);
        return old_head;
    }

    std::unique_ptr<node> try_pop_head(T& res) {
        std::lock_guard<std::mutex> head_lock(head_mtx);

        if (head.get() == get_tail()) {
            return std::unique_ptr<node>();
        }
        res = std::move(*head->data);
        return pop_head();
    }

    std::unique_lock<std::mutex> wait_for_data() {
        std::unique_lock<std::mutex> head_lock(head_mtx);
        data_cond.wait(head_lock, [&] { return head.get() != get_tail(); });
        return std::move(head_lock);
    }

    std::unique_ptr<node> wait_pop_head(T& res) {
        std::unique_lock<std::mutex> head_lock(wait_for_data());
        res = std::move(*head->data);
        return pop_head();
    }
};