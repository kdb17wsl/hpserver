#include <gtest/gtest.h>
#include "src/util/bloom_filter.h"
#include <string>

TEST(BloomFilterTest, BasicInsertAndSearch) {
    bloom_filter filter(1024);
    
    std::string s1 = "hello";
    std::string s2 = "world";
    std::string s3 = "hpserver";
    
    filter.add(s1);
    filter.add(s2);

    EXPECT_TRUE(filter.contains(s1));
    EXPECT_TRUE(filter.contains(s2));
    // Bloom filter can have false positives, but for a large enough filter and small number of items, it should be likely false.
    // However, it must never have false negatives.
}

TEST(BloomFilterTest, NoFalseNegatives) {
    bloom_filter filter(2048);
    std::vector<std::string> words = {"apple", "banana", "orange", "grape", "melon"};
    
    for (const auto& word : words) {
        filter.add(word);
    }
    
    for (const auto& word : words) {
        EXPECT_TRUE(filter.contains(word));
    }
}

TEST(BloomFilterTest, Clear) {
    bloom_filter filter(1024);
    filter.add("test");
    EXPECT_TRUE(filter.contains("test"));
    
    filter.clear();
    EXPECT_FALSE(filter.contains("test"));
}

TEST(BloomFilterTest, FalsePositiveRateIsReasonable) {
    // With 10000 bits and 3 hash functions, and 100 items, FP rate should be very low.
    bloom_filter filter(10000);
    
    for (int i = 0; i < 100; ++i) {
        filter.add("item_" + std::to_string(i));
    }
    
    int false_positives = 0;
    for (int i = 100; i < 1100; ++i) {
        if (filter.contains("item_" + std::to_string(i))) {
            false_positives++;
        }
    }
    
    // FP rate expected: (1 - (1 - 1/m)^(kn))^k approx (1 - e^(-kn/m))^k
    // (1 - e^(-3*100/10000))^3 = (1 - e^-0.03)^3 approx (0.0295)^3 approx 0.000025
    // In 1000 tests, we expect almost 0 false positives.
    EXPECT_LT(false_positives, 10);
}
