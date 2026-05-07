#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "http_request_parser.h"
#include "memory_cache.h"

#include "persistent_cache.h"

class http_cache {
public:
    enum class lookup_status {
        kBypass,
        kMiss,
        kNotModified,
        kHit,
        kError,
    };

    struct lookup_result {
        lookup_status status =
            lookup_status::kBypass;  // Default to bypass for non-cacheable requests.
        std::string cache_key;
        std::string response;
        std::string detail;
    };

    bool open(std::string_view db_path);
    bool ready() const { return l2_ != nullptr && l2_->ready(); }

    lookup_result lookup(const http_request_parser::request_info& req) const;
    bool store(const http_request_parser::request_info& req, std::string_view upstream_response);

    /** @brief Configure L1 memory cache. Disabled by default (0, 0). */
    void configure_l1(std::size_t max_entries, std::size_t max_bytes);

private:
    static bool is_cacheable_request(const http_request_parser::request_info& req);
    static std::string build_cache_key(const http_request_parser::request_info& req);

    std::unique_ptr<persistent_cache> l2_;
    mutable memory_cache<std::string, std::string> l1_cache_;
};
