#include <gtest/gtest.h>
#include "src/util/bitwise_trie.h"
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

/**
 * @brief 基础功能测试：验证插入、检索与未找到的情况。
 */
TEST(BitwiseTrieTest, BasicOperations) {
    bitwise_trie<std::string> trie;
    
    trie.insert(0, "zero");
    trie.insert(0xFFFFFFFF, "max");
    trie.insert(12345, "normal");

    EXPECT_EQ(trie.search(0).value_or(""), "zero");
    EXPECT_EQ(trie.search(0xFFFFFFFF).value_or(""), "max");
    EXPECT_EQ(trie.search(12345).value_or(""), "normal");
    EXPECT_FALSE(trie.search(1).has_value());
}

/**
 * @brief 覆盖更新测试：验证对同一 Key 重复插入会更新其值。
 */
TEST(BitwiseTrieTest, UpdateSemantics) {
    bitwise_trie<int> trie;
    trie.insert(42, 100);
    EXPECT_EQ(trie.search(42).value(), 100);
    
    trie.insert(42, 200);
    EXPECT_EQ(trie.search(42).value(), 200);
}

/**
 * @brief 删除功能测试：验证 remove 方法的返回值及删除后的检索行为。
 */
TEST(BitwiseTrieTest, RemoveSemantics) {
    bitwise_trie<std::string> trie;
    trie.insert(10, "ten");
    
    EXPECT_TRUE(trie.remove(10));
    EXPECT_FALSE(trie.search(10).has_value());
    
    EXPECT_FALSE(trie.remove(10)); // 重复删除
    EXPECT_FALSE(trie.remove(99)); // 删除不存在的 Key
}

/**
 * @brief 边界与重叠位测试：验证具有公共前缀的位路径不会冲突。
 */
TEST(BitwiseTrieTest, BitPathOverlap) {
    bitwise_trie<int> trie;
    // 010... (2) 和 011... (3) 在 Trie 深度中共享前缀
    trie.insert(2, 200);
    trie.insert(3, 300);

    EXPECT_EQ(trie.search(2).value(), 200);
    EXPECT_EQ(trie.search(3).value(), 300);
    
    trie.remove(2);
    EXPECT_FALSE(trie.search(2).has_value());
    EXPECT_EQ(trie.search(3).value(), 300);
}

/**
 * @brief 压测/批量测试：插入大量 Key 验证稳定性。
 */
TEST(BitwiseTrieTest, LargeBatch) {
    bitwise_trie<int> trie;
    constexpr int kCount = 1000;
    
    for (int i = 0; i < kCount; ++i) {
        trie.insert(i, i * 10);
    }
    
    for (int i = 0; i < kCount; ++i) {
        auto val = trie.search(i);
        ASSERT_TRUE(val.has_value()) << "Failed to find key: " << i;
        EXPECT_EQ(*val, i * 10);
    }
}

/**
 * @brief 移动语义测试：验证 bitwise_trie 本身的移动行为（std::unique_ptr 管理）。
 */
TEST(BitwiseTrieTest, MoveSemantics) {
    bitwise_trie<std::string> trie1;
    trie1.insert(1, "one");

    bitwise_trie<std::string> trie2 = std::move(trie1);
    
    EXPECT_EQ(trie2.search(1).value(), "one");
    // trie1 此时处于移出状态（有效但内容未定义，通常 root 为空或重置）
}
