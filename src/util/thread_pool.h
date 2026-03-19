#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "threadsafe_queue.h"

using FunctionalWrapper = std::function<void()>;

class thread_pool {
public:
    thread_pool() : thread_count(std::thread::hardware_concurrency()) {
        waiting_thread_count.store(0);
        is_stopped.store(false);
        is_done.store(false);

        stop_flags.reserve(thread_count);
        threads.reserve(thread_count);
        for (unsigned int i = 0; i < thread_count; ++i) {
            stop_flags.push_back(std::make_shared<std::atomic_bool>(false));
            set_thread(i);
        }
    }

    ~thread_pool() { stop(false); }

    static unsigned int this_thread_id() { return tls_thread_id; }

    int size() const { return threads.size(); }

    std::thread& get_thread(int id) { return *threads[id]; }

    void clear_queue() {
        std::unique_ptr<FunctionalWrapper> task;
        while (task_queue.try_pop(task)) {
        }
    }

    std::unique_ptr<FunctionalWrapper> pop_task() {
        std::unique_ptr<FunctionalWrapper> task;
        if (task_queue.try_pop(task)) {
            return task;
        }
        return nullptr;
    }

    /**
     * @brief Stops the thread pool, optionally waiting for all queued tasks to finish.
     *
     * This function stops the thread pool in one of two modes:
     *
     * - If `wait_for_tasks` is true (default), the pool will finish processing all queued tasks
     * before stopping. If the pool is already marked as done or stopped, the function returns
     * immediately.
     *
     * - If `wait_for_tasks` is false, the pool stops accepting work, discards any remaining tasks
     * in the queue, and joins worker threads. Tasks that are already running are not interrupted
     * and must finish on their own. If the pool is already stopped, the function returns
     * immediately.
     *
     * In both cases, all worker threads are notified and joined before the function returns.
     * The task queue and internal thread/flag containers are cleared.
     *
     * @param wait_for_tasks If true, waits for all tasks to finish before stopping. If false, stops
     * immediately.
     */
    void stop(bool wait_for_tasks = true) {
        if (wait_for_tasks) {
            if (is_done.load() || is_stopped.load()) {
                return;
            }
            is_done.store(true);
        } else {
            if (is_stopped.load()) {
                return;
            }
            is_stopped.store(true);

            for (auto& flag : stop_flags) {
                flag->store(true);
            }
            this->clear_queue();
        }

        std::unique_lock<std::mutex> lock(mtx);
        cv.notify_all();
        lock.unlock();

        for (auto& thread : threads) {
            if (thread->joinable()) {
                thread->join();
            }
        }

        clear_queue();
        threads.clear();
        stop_flags.clear();
    }

    template <typename F, typename... Args>
    auto push(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using Result = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto _f = std::make_unique<FunctionalWrapper>([task]() { (*task)(); });
        task_queue.push(std::move(_f));
        std::lock_guard<std::mutex> lock(mtx);
        cv.notify_one();
        return task->get_future();
    }

private:
    static void set_thread_id(unsigned int id) { tls_thread_id = id; }

    void set_thread(unsigned int id) {
        auto stop_flag(this->stop_flags[id]);
        auto func = [this, id, stop_flag]() {
            set_thread_id(id);
            std::atomic_bool& flag = *stop_flag;
            std::unique_ptr<FunctionalWrapper> task;
            bool is_pop = this->task_queue.try_pop(task);
            while (true) {
                while (is_pop) {
                    (*task)();
                    task.reset();
                    if (flag.load()) {
                        return;
                    } else {
                        is_pop = this->task_queue.try_pop(task);
                    }
                }

                std::unique_lock<std::mutex> lock(this->mtx);
                this->waiting_thread_count.fetch_add(1);
                this->cv.wait(lock, [this, &task, &flag, &is_pop]() {
                    is_pop = this->task_queue.try_pop(task);
                    return is_pop || is_done.load() || flag.load();
                });
                this->waiting_thread_count.fetch_sub(1);

                if (!is_pop) {
                    return;
                }
            }
        };
        threads.emplace_back(std::make_unique<std::thread>(func));
    }

    static inline thread_local unsigned int tls_thread_id = 0;

    std::vector<std::unique_ptr<std::thread>> threads;
    std::vector<std::shared_ptr<std::atomic_bool>> stop_flags;
    threadsafe_queue<std::unique_ptr<FunctionalWrapper>> task_queue;

    const unsigned int thread_count;

    std::atomic_int waiting_thread_count;
    std::atomic_bool is_stopped;
    std::atomic_bool is_done;

    std::mutex mtx;
    std::condition_variable cv;
};
