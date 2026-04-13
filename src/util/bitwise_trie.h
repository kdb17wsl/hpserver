#pragma once

#include <memory>
#include <concepts>
#include <optional>
#include <array>


/**
 * @brief Bitwise Trie for mapping 32-bit integer keys to values.
 * Optimized with C++20 features and std::optional for search results.
 */
template <typename T>
class bitwise_trie {
    struct node {
        std::array<std::unique_ptr<node>, 2> children{};
        std::optional<T> value;
    };

public:
    bitwise_trie() : root_(std::make_unique<node>()) {}
    ~bitwise_trie() = default;

    // Use default move operations, disable copies
    bitwise_trie(const bitwise_trie&) = delete;
    bitwise_trie& operator=(const bitwise_trie&) = delete;
    bitwise_trie(bitwise_trie&&) noexcept = default;
    bitwise_trie& operator=(bitwise_trie&&) noexcept = default;

    /**
     * @brief Search for a key. Returns std::optional<T> containing the value if found.
     * Note: This returns a copy of the value. For complex types, consider using a pointer-based search.
     */
    [[nodiscard]] std::optional<T> search(uint32_t key) const noexcept(std::is_nothrow_copy_constructible_v<T>) {
        const node* current = root_.get();
        for (int i = 31; i >= 0; --i) {
            uint32_t bit = (key >> i) & 1;
            if (!current->children[bit]) {
                return std::nullopt;
            }
            current = current->children[bit].get();
        }
        return current->value;
    }

    /**
     * @brief Insert or update a key-value pair.
     */
    void insert(uint32_t key, T value) {
        node* current = root_.get();
        for (int i = 31; i >= 0; --i) {
            uint32_t bit = (key >> i) & 1;
            if (!current->children[bit]) {
                current->children[bit] = std::make_unique<node>();
            }
            current = current->children[bit].get();
        }
        current->value = std::move(value);
    }

    /**
     * @brief Remove a key. Returns true if key was found and removed.
     */
    bool remove(uint32_t key) noexcept {
        node* current = root_.get();
        for (int i = 31; i >= 0; --i) {
            uint32_t bit = (key >> i) & 1;
            if (!current->children[bit]) {
                return false;
            }
            current = current->children[bit].get();
        }
        bool existed = current->value.has_value();
        current->value.reset();
        return existed;
    }

private:
    std::unique_ptr<node> root_;
};

