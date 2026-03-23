#include "src/util/threadsafe_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

TEST(ThreadsafeQueueTest, EmptyOnNewQueue) {
    threadsafe_queue<int> queue;
    EXPECT_TRUE(queue.empty());
}

TEST(ThreadsafeQueueTest, TryPopOnEmptyReturnsFalse) {
    threadsafe_queue<int> queue;
    int value = 0;
    EXPECT_FALSE(queue.try_pop(value));
}

TEST(ThreadsafeQueueTest, PushThenTryPopReturnsValue) {
    threadsafe_queue<int> queue;
    queue.push(42);

    int value = 0;
    EXPECT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(queue.empty());
}

TEST(ThreadsafeQueueTest, WaitPopBlocksUntilPush) {
    threadsafe_queue<int> queue;

    std::atomic<bool> started{false};
    int value = -1;

    std::thread consumer([&] {
        started.store(true, std::memory_order_release);
        queue.wait_pop(value);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    queue.push(7);

    consumer.join();
    EXPECT_EQ(value, 7);
}

TEST(ThreadsafeQueueTest, ConcurrentProducersConsumers) {
    threadsafe_queue<int> queue;

    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kItemsPerProducer = 1000;
    constexpr int kTotalItems = kProducers * kItemsPerProducer;
    constexpr int kSentinel = -1;

    std::atomic<long long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(kProducers);
    consumers.reserve(kConsumers);

    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            while (true) {
                int value = 0;
                queue.wait_pop(value);
                if (value == kSentinel) {
                    break;
                }
                sum.fetch_add(value, std::memory_order_relaxed);
                count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            int base = p * kItemsPerProducer;
            for (int i = 0; i < kItemsPerProducer; ++i) {
                queue.push(base + i);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    for (int i = 0; i < kConsumers; ++i) {
        queue.push(kSentinel);
    }

    for (auto& consumer : consumers) {
        consumer.join();
    }

    const long long expected_sum =
        static_cast<long long>(kTotalItems - 1) * kTotalItems / 2;

    EXPECT_EQ(count.load(), kTotalItems);
    EXPECT_EQ(sum.load(), expected_sum);
}

TEST(ThreadsafeQueueTest, NoDeadlockUnderContention) {
    threadsafe_queue<int> queue;

    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kItemsPerProducer = 5000;
    constexpr int kTotalItems = kProducers * kItemsPerProducer;
    constexpr int kSentinel = -1;

    auto producer_task = [&queue](int base) {
        for (int i = 0; i < kItemsPerProducer; ++i) {
            queue.push(base + i);
        }
    };

    auto consumer_task = [&queue]() {
        while (true) {
            int value = 0;
            queue.wait_pop(value);
            if (value == kSentinel) {
                return;
            }
        }
    };

    auto run = std::async(std::launch::async, [&]() {
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;
        producers.reserve(kProducers);
        consumers.reserve(kConsumers);

        for (int i = 0; i < kConsumers; ++i) {
            consumers.emplace_back(consumer_task);
        }

        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back(producer_task, p * kItemsPerProducer);
        }

        for (auto& producer : producers) {
            producer.join();
        }

        for (int i = 0; i < kConsumers; ++i) {
            queue.push(kSentinel);
        }

        for (auto& consumer : consumers) {
            consumer.join();
        }
    });

    EXPECT_EQ(run.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}
