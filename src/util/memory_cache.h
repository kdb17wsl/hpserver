#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <utility>

// -------------------------------------------------------------------
// Concepts
// -------------------------------------------------------------------

template <typename W, typename T>
concept WeightFunction = std::invocable<W, const T&> &&
                         std::convertible_to<std::invoke_result_t<W, const T&>,
                                             std::size_t>;

// -------------------------------------------------------------------
// Default weight function
// -------------------------------------------------------------------

struct default_weight_fn {
    template <typename T>
    [[nodiscard]] constexpr std::size_t operator()(const T& value) const noexcept {
        if constexpr (requires {
                          { value.size() } -> std::convertible_to<std::size_t>;
                      }) {
            return static_cast<std::size_t>(value.size());
        } else {
            return sizeof(T);
        }
    }
};

// -------------------------------------------------------------------
// memory_cache
// -------------------------------------------------------------------

/**
 * @brief Fixed-capacity LRU cache with dual eviction policy.
 *
 * Eviction happens when either max_entries or max_bytes is exceeded.
 * The least-recently-used entry is removed first.
 *
 * Not thread-safe; external synchronization is required for concurrent use.
 */
template <typename Key, typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEq = std::equal_to<Key>>
class memory_cache {
public:
    using key_type = Key;
    using value_type = Value;

    explicit memory_cache(std::size_t max_entries = 0,
                          std::size_t max_bytes = 0)
        : memory_cache(max_entries, max_bytes, default_weight_fn{}) {}

    template <typename WeightFn>
        requires WeightFunction<WeightFn, Value>
    memory_cache(std::size_t max_entries, std::size_t max_bytes,
                 WeightFn&& weight_fn)
        : max_entries_(max_entries), max_bytes_(max_bytes),
          weight_fn_(std::forward<WeightFn>(weight_fn)) {}

    memory_cache(const memory_cache&) = delete;
    memory_cache& operator=(const memory_cache&) = delete;
    memory_cache(memory_cache&&) = default;
    memory_cache& operator=(memory_cache&&) = default;
    ~memory_cache() = default;

    /**
     * @brief Look up a key and promote it to most-recently-used.
     * @return Pointer to the cached value, or nullptr if not found.
     *         The pointer remains valid until the entry is evicted or erased.
     *
     * Prefer with_value() for safer access without raw pointers.
     */
    [[nodiscard]] const Value* get(const Key& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return &it->second->value;
    }

    /**
     * @brief Look up a key and invoke a callback with the value.
     *
     * Safer alternative to get() -- no raw pointers, no lifetime
     * ambiguity for the caller. The value is guaranteed valid only
     * inside the callback.
     *
     * @return true if the key was found and the callback was invoked.
     */
    template <typename F>
        requires std::invocable<F, const Value&>
    bool with_value(const Key& key, F&& fn) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        std::forward<F>(fn)(it->second->value);
        return true;
    }

    /** @brief Insert or update a key-value pair (by value). */
    void put(const Key& key, Value value) {
        if (max_entries_ == 0 || max_bytes_ == 0) {
            return;
        }
        do_put(key, std::move(value));
    }

    /** @brief Insert or update a key-value pair (forwarding). */
    template <typename V>
        requires std::convertible_to<V, Value>
    void put(const Key& key, V&& value) {
        if (max_entries_ == 0 || max_bytes_ == 0) {
            return;
        }
        do_put(key, std::forward<V>(value));
    }

    /** @brief Remove a key. Returns true if the key existed. */
    [[nodiscard]] bool erase(const Key& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        total_weight_ -= it->second->weight;
        lru_list_.erase(it->second);
        map_.erase(it);
        return true;
    }

    /** @brief Remove all entries. */
    void clear() noexcept {
        lru_list_.clear();
        map_.clear();
        total_weight_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] std::size_t total_weight() const noexcept {
        return total_weight_;
    }
    [[nodiscard]] std::size_t max_entries() const noexcept {
        return max_entries_;
    }
    [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }

private:
    struct node {
        Key key;
        Value value;
        std::size_t weight;
    };

    std::size_t max_entries_;
    std::size_t max_bytes_;
    std::function<std::size_t(const Value&)> weight_fn_;

    std::list<node> lru_list_;
    std::unordered_map<Key, typename std::list<node>::iterator, Hash, KeyEq>
        map_;
    std::size_t total_weight_ = 0;

    template <typename V>
    void do_put(const Key& key, V&& value) {
        auto it = map_.find(key);
        const std::size_t new_weight = weight_fn_(value);

        if (it != map_.end()) {
            node& n = *it->second;
            total_weight_ -= n.weight;
            n.value = std::forward<V>(value);
            n.weight = new_weight;
            total_weight_ += new_weight;
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        } else {
            lru_list_.emplace_front(key, std::forward<V>(value), new_weight);
            map_.emplace(key, lru_list_.begin());
            total_weight_ += new_weight;
        }

        evict_if_needed();
    }

    void evict_if_needed() noexcept {
        while (!lru_list_.empty() &&
               (map_.size() > max_entries_ || total_weight_ > max_bytes_)) {
            const node& back = lru_list_.back();
            total_weight_ -= back.weight;
            map_.erase(back.key);
            lru_list_.pop_back();
        }
    }
};
