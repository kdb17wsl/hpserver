#pragma once

#include <vector>
#include <string_view>
#include <cstdint>
#include <array>
#include <algorithm>

namespace hpserver::util {

/**
 * @brief Simple Bloom Filter implementation for fast set membership testing.
 */
class bloom_filter {
public:
    explicit bloom_filter(size_t size) : bits_(size, false) {}

    void add(std::string_view data) {
        auto hashes = compute_hashes(data);
        for (auto hash : hashes) {
            bits_[hash % bits_.size()] = true;
        }
    }

    [[nodiscard]] bool contains(std::string_view data) const {
        auto hashes = compute_hashes(data);
        return std::all_of(hashes.begin(), hashes.end(), [this](uint64_t hash) {
            return bits_[hash % bits_.size()];
        });
    }

    void clear() {
        std::fill(bits_.begin(), bits_.end(), false);
    }

private:
    static constexpr uint64_t fnv1a_hash(std::string_view data, uint64_t seed) {
        uint64_t hash = seed;
        for (char c : data) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::array<uint64_t, 3> compute_hashes(std::string_view data) const {
        return {
            fnv1a_hash(data, 0xcbf29ce484222325ULL),
            fnv1a_hash(data, 0x811c9dc5ULL),
            fnv1a_hash(data, 0x100000001b3ULL)
        };
    }

    std::vector<bool> bits_;
};

} // namespace hpserver::util
