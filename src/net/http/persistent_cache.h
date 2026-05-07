#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "http_request_parser.h"

class persistent_cache {
public:
    persistent_cache();
    ~persistent_cache();

    persistent_cache(const persistent_cache&) = delete;
    persistent_cache& operator=(const persistent_cache&) = delete;
    persistent_cache(persistent_cache&&) noexcept;
    persistent_cache& operator=(persistent_cache&&) noexcept;

    enum class lookup_status {
        kMiss,
        kNotModified,
        kHit,
    };

    struct lookup_result {
        lookup_status status = lookup_status::kMiss;
        std::string response;
    };

    bool open(std::string_view db_path);
    bool ready() const;

    lookup_result lookup(const std::string& cache_key,
                         const http_request_parser::request_info& req) const;
    bool store(const std::string& cache_key,
               std::string_view upstream_response);
    bool erase(const std::string& cache_key);

private:
    class impl;
    std::unique_ptr<impl> impl_;
};
