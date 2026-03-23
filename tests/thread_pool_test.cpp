#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "src/util/thread_pool.h"

TEST(ThreadPoolTest, ExecutesAllTasks) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    constexpr int kTasks = 100;
    std::atomic<int> count{0};
    std::vector<std::future<int>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.push([&count, i]() {
            count.fetch_add(1, std::memory_order_relaxed);
            return i;
        }));
    }

    int sum = 0;
    for (auto& fut : futures) {
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        sum += fut.get();
    }

    EXPECT_EQ(count.load(), kTasks);
    EXPECT_EQ(sum, (kTasks - 1) * kTasks / 2);

    pool.stop(true);
}

TEST(ThreadPoolTest, ReturnsValue) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    auto fut = pool.push([] { return 42; });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(fut.get(), 42);

    pool.stop(true);
}

TEST(ThreadPoolTest, StopWaitsForTasks) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    constexpr int kTasks = 20;
    std::atomic<int> count{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.push([&count]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            count.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    pool.stop(true);
    EXPECT_EQ(count.load(), kTasks);

    for (auto& fut : futures) {
        EXPECT_EQ(fut.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    }
}

TEST(ThreadPoolTest, ThreadIdsWithinRange) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    constexpr int kTasks = 200;
    std::atomic<bool> invalid_id{false};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.push([&pool, &invalid_id]() {
            unsigned int id = thread_pool::this_thread_id();
            if (id >= static_cast<unsigned int>(pool.size())) {
                invalid_id.store(true, std::memory_order_relaxed);
            }
        }));
    }

    for (auto& fut : futures) {
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    }

    EXPECT_FALSE(invalid_id.load());

    pool.stop(true);
}

TEST(ThreadPoolTest, TaskExceptionPropagatesToFuture) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    auto fut = pool.push([]() -> int { throw std::runtime_error("boom"); });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_THROW(fut.get(), std::runtime_error);

    pool.stop(true);
}

TEST(ThreadPoolTest, StopIsIdempotent) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    auto fut = pool.push([] { return 1; });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(fut.get(), 1);

    pool.stop(true);
    EXPECT_NO_THROW(pool.stop(true));
}

TEST(ThreadPoolTest, StopFalseDiscardsQueuedTasks) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    constexpr int kExtraTasks = 200;
    std::atomic<int> blockers_started{0};
    std::atomic<int> blockers_finished{0};
    std::atomic<int> queued_started{0};

    const int blocker_count = std::max(1, pool.size());
    for (int i = 0; i < blocker_count; ++i) {
        pool.push([&]() {
            blockers_started.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            blockers_finished.fetch_add(1, std::memory_order_relaxed);
        });
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (blockers_started.load(std::memory_order_relaxed) < blocker_count &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    if (blockers_started.load(std::memory_order_relaxed) == 0) {
        GTEST_SKIP() << "no tasks started";
    }

    for (int i = 0; i < kExtraTasks; ++i) {
        pool.push([&]() { queued_started.fetch_add(1, std::memory_order_relaxed); });
    }

    pool.stop(false);

    EXPECT_EQ(blockers_finished.load(), blockers_started.load());
    EXPECT_EQ(queued_started.load(), 0);
}

TEST(ThreadPoolTest, MultiProducerPushExecutesAll) {
    thread_pool pool;
    if (pool.size() == 0) {
        GTEST_SKIP() << "hardware_concurrency reported 0";
    }

    constexpr int kProducers = 4;
    constexpr int kTasksPerProducer = 50;
    constexpr int kTotalTasks = kProducers * kTasksPerProducer;

    std::mutex futures_mtx;
    std::vector<std::future<int>> futures;
    futures.reserve(kTotalTasks);

    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < kTasksPerProducer; ++i) {
                auto fut = pool.push([p, i, kTasksPerProducer]() {
                    return p * kTasksPerProducer + i;
                });
                std::lock_guard<std::mutex> lock(futures_mtx);
                futures.push_back(std::move(fut));
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    int sum = 0;
    for (auto& fut : futures) {
        ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        sum += fut.get();
    }

    EXPECT_EQ(static_cast<int>(futures.size()), kTotalTasks);
    EXPECT_EQ(sum, (kTotalTasks - 1) * kTotalTasks / 2);

    pool.stop(true);
}
