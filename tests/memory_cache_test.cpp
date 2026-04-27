#include <gtest/gtest.h>

#include "src/util/memory_cache.h"

TEST(MemoryCacheTest, BasicPutAndGet) {
    memory_cache<std::string, std::string> cache(100, 1024 * 1024);

    cache.put("key1", "value1");
    const std::string* v = cache.get("key1");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "value1");
}

TEST(MemoryCacheTest, GetMissReturnsNull) {
    memory_cache<std::string, std::string> cache(100, 1024);
    EXPECT_EQ(cache.get("nonexistent"), nullptr);
}

TEST(MemoryCacheTest, UpdateExistingKey) {
    memory_cache<std::string, std::string> cache(100, 1024);

    cache.put("key1", "value1");
    cache.put("key1", "value2");

    EXPECT_EQ(cache.size(), 1u);
    const std::string* v = cache.get("key1");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, "value2");
}

TEST(MemoryCacheTest, EvictionByMaxEntries) {
    memory_cache<int, std::string> cache(
        2, 1024 * 1024, [](const std::string&) { return 1; });

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");  // Should evict 1

    EXPECT_EQ(cache.get(1), nullptr);
    EXPECT_NE(cache.get(2), nullptr);
    EXPECT_NE(cache.get(3), nullptr);
}

TEST(MemoryCacheTest, EvictionByMaxBytes) {
    memory_cache<std::string, std::string> cache(
        100, 10, [](const std::string& s) { return s.size(); });

    cache.put("a", "12345");   // weight 5
    cache.put("b", "12345");   // weight 5, total 10
    cache.put("c", "123");     // weight 3, total 13 > 10, evict a (LRU)

    EXPECT_EQ(cache.get("a"), nullptr);
    EXPECT_NE(cache.get("b"), nullptr);
    EXPECT_NE(cache.get("c"), nullptr);
}

TEST(MemoryCacheTest, LruOrder) {
    memory_cache<int, std::string> cache(
        2, 1024, [](const std::string&) { return 1; });

    cache.put(1, "one");
    cache.put(2, "two");
    [[maybe_unused]] auto* _ = cache.get(1);  // Touch 1, making 2 the LRU
    cache.put(3, "three");     // Evict 2

    EXPECT_NE(cache.get(1), nullptr);
    EXPECT_EQ(cache.get(2), nullptr);
    EXPECT_NE(cache.get(3), nullptr);
}

TEST(MemoryCacheTest, Erase) {
    memory_cache<std::string, std::string> cache(100, 1024);
    cache.put("key", "value");
    EXPECT_TRUE(cache.erase("key"));
    EXPECT_EQ(cache.get("key"), nullptr);
    EXPECT_FALSE(cache.erase("key"));
}

TEST(MemoryCacheTest, Clear) {
    memory_cache<std::string, std::string> cache(100, 1024);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.get("a"), nullptr);
    EXPECT_EQ(cache.get("b"), nullptr);
}

TEST(MemoryCacheTest, DisabledCache) {
    memory_cache<std::string, std::string> cache(0, 0);
    cache.put("key", "value");
    EXPECT_EQ(cache.get("key"), nullptr);
    EXPECT_EQ(cache.size(), 0u);
}

TEST(MemoryCacheTest, WeightUpdatesTotal) {
    memory_cache<std::string, std::string> cache(100, 1024);
    cache.put("k", "1234");
    EXPECT_EQ(cache.total_weight(), 4u);
    cache.put("k", "12");
    EXPECT_EQ(cache.total_weight(), 2u);
}
