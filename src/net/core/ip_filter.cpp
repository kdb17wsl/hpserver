#include "net/core/ip_filter.h"

#include <arpa/inet.h>
#include <fstream>
#include <iostream>
#include <string>

/**
 * @brief IP Filter supporting both single IPs and CIDR ranges (e.g., 192.168.1.0/24).
 * Uses optimized bitwise_trie and bloom_filter for fast lookups.
 */

ip_filter::ip_filter(size_t bloom_size) 
    : trie_(), bloom_filter_(bloom_size) {}

ip_filter::ip_filter(std::string ip_list_path, size_t bloom_size)
    : trie_(), bloom_filter_(bloom_size), ip_list_path_(std::move(ip_list_path)) {
    init();
}

void ip_filter::add_ip(uint32_t ip) {
    trie_.insert(ip, true);
    std::string_view data(reinterpret_cast<const char*>(&ip), sizeof(ip));
    bloom_filter_.add(data);
}

void ip_filter::add_cidr(std::string_view cidr) {
    size_t slash_pos = cidr.find('/');
    if (slash_pos == std::string_view::npos) {
        uint32_t ip = 0;
        if (inet_pton(AF_INET, cidr.data(), &ip) == 1) {
            add_ip(ntohl(ip));
        }
        return;
    }

    std::string ip_part(cidr.substr(0, slash_pos));
    int mask = std::stoi(std::string(cidr.substr(slash_pos + 1)));
    
    uint32_t ip_addr = 0;
    if (inet_pton(AF_INET, ip_part.c_str(), &ip_addr) != 1) return;
    ip_addr = ntohl(ip_addr);

    if (mask <= 0 || mask > 32) return;

    if (mask >= 24) { 
        uint32_t num_ips = 1 << (32 - mask);
        uint32_t base_ip = ip_addr & (0xFFFFFFFF << (32 - mask));
        for (uint32_t i = 0; i < num_ips; ++i) {
            add_ip(base_ip + i);
        }
    } else {
        add_ip(ip_addr); 
    }
}

bool ip_filter::is_blocked(uint32_t ip) const noexcept {
    std::string_view data(reinterpret_cast<const char*>(&ip), sizeof(ip));
    if (!bloom_filter_.contains(data)) {
        return false;
    }
    return trie_.search(ip).has_value();
}

void ip_filter::init() {
    if (ip_list_path_.empty()) return;

    std::ifstream file(ip_list_path_);
    if (!file.is_open()) {
        std::cerr << "Failed to open IP blacklist file: " << ip_list_path_ << std::endl;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        add_cidr(line);
    }
}
