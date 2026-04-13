#pragma once

#include <string>
#include <string_view>
#include <cstdint>

#include "util/bitwise_trie.h"
#include "util/bloom_filter.h"

/**
 * @brief IP Filter supporting both single IPs and CIDR ranges (e.g., 192.168.1.0/24).
 * Uses optimized bitwise_trie and bloom_filter for fast lookups.
 */
class ip_filter {
public:
    explicit ip_filter(size_t bloom_size = 1024);

    /**
     * @brief Add a single IP address to the blacklist.
     */
    void add_ip(uint32_t ip);

    /**
     * @brief Add a CIDR range to the blacklist (e.g., 10.0.0.0/8).
     */
    void add_cidr(std::string_view cidr);

    /**
     * @brief Check if an IP (host byte order) is blocked.
     */
    [[nodiscard]] bool is_blocked(uint32_t ip) const noexcept;

    void init(const std::string &ip_list_path = "/config/blocked_ips.txt");

private:
    bitwise_trie<bool> trie_;
    bloom_filter bloom_filter_;
    std::string ip_list_path_;

    
};
