#include "http_cache.h"

#include <rocksdb/db.h>

#include <utility>

bool http_cache::open(std::string_view db_path) {
    rocksdb::Options options;
    options.create_if_missing = true;

    std::unique_ptr<rocksdb::DB> db;
    const rocksdb::Status status =
        rocksdb::DB::Open(options, std::string(db_path), &db);
    if (!status.ok()) {
        return false;
    }

    db_ = std::move(db);
    return true;
}

http_cache::lookup_result http_cache::lookup(
    const http_request_parser::request_info& req) const {
    lookup_result result;

    if (!is_cacheable_request(req)) {
        result.status = lookup_status::kBypass;
        return result;
    }

    result.cache_key = build_cache_key(req);

    if (!ready()) {
        result.status = lookup_status::kError;
        result.detail = "cache module is not initialized";
        return result;
    }

    db_->Get(rocksdb::ReadOptions(), result.cache_key, &result.response);

    // Framework placeholder:
    // 1) Read entry from RocksDB by cache_key.
    // 2) Evaluate If-Modified-Since against cached Last-Modified.
    // 3) Return kNotModified/kHit/kMiss accordingly.
    result.status = lookup_status::kMiss;
    return result;
}

bool http_cache::store(const http_request_parser::request_info& req,
                       std::string_view upstream_response) {
    (void)upstream_response;

    if (!is_cacheable_request(req) || !ready()) {
        return false;
    }

    const std::string cache_key = build_cache_key(req);
    (void)cache_key;

    // Framework placeholder:
    // 1) Parse upstream response headers.
    // 2) Persist payload + metadata (e.g. Last-Modified) to RocksDB.
    return true;
}

bool http_cache::is_cacheable_request(const http_request_parser::request_info& req) {
    return !req.is_connect && req.method == "GET";
}

std::string http_cache::build_cache_key(const http_request_parser::request_info& req) {
    std::string key;
    key.reserve(req.host.size() + req.url.size() + req.method.size() + 32);
    key.append(req.host);
    key.push_back(':');
    key.append(std::to_string(req.port));
    key.push_back('|');
    key.append(req.method);
    key.push_back('|');
    key.append(req.url);

    const auto it = req.headers.find("accept-encoding");
    if (it != req.headers.end()) {
        key.push_back('|');
        key.append(it->second);
    }

    return key;
}
