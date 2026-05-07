#include "persistent_cache.h"

#include <rocksdb/db.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "http_cache.pb.h"
#include "http_response_parser.h"

namespace {

int64_t now_unix_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_hop_by_hop_header(std::string_view key) {
    return key == "connection" || key == "proxy-connection" || key == "keep-alive" ||
           key == "te" || key == "trailer" || key == "transfer-encoding" ||
           key == "upgrade";
}

std::string to_lower_ascii(std::string_view value) {
    std::string out(value);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

int64_t parse_max_age(std::string_view cc) {
    constexpr std::string_view kMaxAge = "max-age=";
    std::size_t pos = 0;
    while (pos < cc.size()) {
        std::size_t start = cc.find(kMaxAge, pos);
        if (start == std::string_view::npos) {
            return -1;
        }
        bool valid_start =
            start == 0 || cc[start - 1] == ',' || cc[start - 1] == ' ' || cc[start - 1] == ';';
        if (!valid_start) {
            pos = start + kMaxAge.size();
            continue;
        }
        std::size_t val_begin = start + kMaxAge.size();
        std::size_t val_end = val_begin;
        while (val_end < cc.size() && cc[val_end] >= '0' && cc[val_end] <= '9') {
            ++val_end;
        }
        if (val_end == val_begin) {
            return -1;
        }
        try {
            return std::stoll(std::string(cc.substr(val_begin, val_end - val_begin)));
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

int64_t compute_ttl(const http_response_parser::response_info& resp) {
    auto cc_it = resp.headers.find("cache-control");
    if (cc_it != resp.headers.end()) {
        int64_t max_age = parse_max_age(cc_it->second);
        if (max_age >= 0) {
            return max_age;
        }
    }
    auto expires_it = resp.headers.find("expires");
    if (expires_it != resp.headers.end()) {
        return 3600;
    }
    return 3600;
}

bool is_fresh(const hpserver::cache::HttpCacheEntry& entry) {
    return entry.stored_at_unix_sec() + entry.ttl_sec() >= now_unix_sec();
}

bool check_not_modified(const http_request_parser::request_info& req,
                        const hpserver::cache::HttpCacheEntry& entry) {
    auto inm_it = req.headers.find("if-none-match");
    if (inm_it != req.headers.end() && !entry.etag().empty()) {
        std::string client_etag = to_lower_ascii(inm_it->second);
        std::string cached_etag = to_lower_ascii(entry.etag());
        auto trim_quotes = [](std::string& s) {
            if (!s.empty() && s.front() == '"') {
                s.erase(0, 1);
            }
            if (!s.empty() && s.back() == '"') {
                s.pop_back();
            }
        };
        trim_quotes(client_etag);
        trim_quotes(cached_etag);
        return client_etag == cached_etag;
    }

    auto ims_it = req.headers.find("if-modified-since");
    if (ims_it != req.headers.end() && !entry.last_modified().empty()) {
        return ims_it->second == entry.last_modified();
    }

    return false;
}

std::string rebuild_response(const hpserver::cache::HttpCacheEntry& entry) {
    std::string response;
    std::size_t headers_size = 0;
    for (int i = 0; i < entry.headers_size(); ++i) {
        headers_size += entry.headers(i).key().size();
        headers_size += entry.headers(i).value().size();
        headers_size += 4;
    }
    response.reserve(64 + headers_size + 4 + entry.body().size());

    response.append("HTTP/1.1 ");
    response.append(std::to_string(entry.status_code()));
    response.push_back(' ');
    response.append(entry.reason_phrase());
    response.append("\r\n");

    bool has_content_length = false;
    for (int i = 0; i < entry.headers_size(); ++i) {
        const auto& h = entry.headers(i);
        std::string key_lower = to_lower_ascii(h.key());
        if (is_hop_by_hop_header(key_lower)) {
            continue;
        }
        if (key_lower == "content-length") {
            has_content_length = true;
        }
        response.append(h.key());
        response.append(": ");
        response.append(h.value());
        response.append("\r\n");
    }

    if (!has_content_length) {
        response.append("Content-Length: ");
        response.append(std::to_string(entry.body().size()));
        response.append("\r\n");
    }

    response.append("\r\n");
    response.append(entry.body());
    return response;
}

std::string build_not_modified_response(
    const hpserver::cache::HttpCacheEntry& entry) {
    std::string response;
    response.reserve(256);
    response.append("HTTP/1.1 304 Not Modified\r\n");
    for (int i = 0; i < entry.headers_size(); ++i) {
        const auto& h = entry.headers(i);
        std::string key_lower = to_lower_ascii(h.key());
        if (key_lower == "etag" || key_lower == "last-modified" ||
            key_lower == "cache-control" || key_lower == "expires" ||
            key_lower == "vary") {
            response.append(h.key());
            response.append(": ");
            response.append(h.value());
            response.append("\r\n");
        }
    }
    response.append("\r\n");
    return response;
}

std::vector<std::string> parse_vary_headers(std::string_view vary_value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < vary_value.size()) {
        while (start < vary_value.size() && vary_value[start] == ' ') {
            ++start;
        }
        std::size_t end = vary_value.find(',', start);
        if (end == std::string_view::npos) {
            end = vary_value.size();
        }
        std::size_t name_end = end;
        while (name_end > start && vary_value[name_end - 1] == ' ') {
            --name_end;
        }
        if (name_end > start) {
            result.emplace_back(to_lower_ascii(vary_value.substr(start, name_end - start)));
        }
        start = end + 1;
    }
    return result;
}

}  // namespace

class persistent_cache::impl {
public:
    bool open(std::string_view db_path) {
        rocksdb::Options options;
        options.create_if_missing = true;
        std::unique_ptr<rocksdb::DB> db;
        if (!rocksdb::DB::Open(options, std::string(db_path), &db).ok()) {
            return false;
        }
        db_ = std::move(db);
        return true;
    }

    bool ready() const { return db_ != nullptr; }

    lookup_result lookup(const std::string& cache_key,
                         const http_request_parser::request_info& req) const {
        lookup_result result;

        std::string raw_value;
        if (!db_->Get(rocksdb::ReadOptions(), cache_key, &raw_value).ok()) {
            return result;
        }

        hpserver::cache::HttpCacheEntry entry;
        if (!entry.ParseFromString(raw_value)) {
            return result;
        }

        if (!is_fresh(entry)) {
            return result;
        }

        if (check_not_modified(req, entry)) {
            result.status = lookup_status::kNotModified;
            result.response = build_not_modified_response(entry);
            return result;
        }

        result.status = lookup_status::kHit;
        result.response = rebuild_response(entry);
        return result;
    }

    bool store(const std::string& cache_key,
               std::string_view upstream_response) {
        http_response_parser parser;
        if (!parser.parse(upstream_response) || parser.has_parse_error()) {
            return false;
        }
        if (!parser.is_message_complete()) {
            parser.finish();
        }
        if (!parser.is_message_complete() || parser.has_parse_error()) {
            return false;
        }

        const auto& resp = parser.response();
        if (resp.status_code < 200 || resp.status_code >= 300) {
            return false;
        }

        hpserver::cache::HttpCacheEntry entry;
        entry.set_status_code(resp.status_code);
        entry.set_reason_phrase(resp.reason_phrase);
        entry.set_stored_at_unix_sec(now_unix_sec());
        entry.set_ttl_sec(compute_ttl(resp));

        for (const auto& kv : resp.headers) {
            auto* h = entry.add_headers();
            h->set_key(kv.first);
            h->set_value(kv.second);

            if (kv.first == "last-modified") {
                entry.set_last_modified(kv.second);
            } else if (kv.first == "etag") {
                entry.set_etag(kv.second);
            } else if (kv.first == "vary") {
                for (const auto& vh : parse_vary_headers(kv.second)) {
                    entry.add_vary_headers(vh);
                }
            }
        }

        entry.set_body(resp.body);

        std::string serialized;
        if (!entry.SerializeToString(&serialized)) {
            return false;
        }

        return db_->Put(rocksdb::WriteOptions(), cache_key, serialized).ok();
    }

    bool erase(const std::string& cache_key) {
        return db_->Delete(rocksdb::WriteOptions(), cache_key).ok();
    }

private:
    std::unique_ptr<rocksdb::DB> db_;
};

persistent_cache::persistent_cache() : impl_(std::make_unique<impl>()) {}
persistent_cache::~persistent_cache() = default;
persistent_cache::persistent_cache(persistent_cache&&) noexcept = default;
persistent_cache& persistent_cache::operator=(persistent_cache&&) noexcept = default;

bool persistent_cache::open(std::string_view db_path) {
    return impl_->open(db_path);
}

bool persistent_cache::ready() const {
    return impl_ && impl_->ready();
}

persistent_cache::lookup_result persistent_cache::lookup(
    const std::string& cache_key,
    const http_request_parser::request_info& req) const {
    return impl_->lookup(cache_key, req);
}

bool persistent_cache::store(const std::string& cache_key,
                             std::string_view upstream_response) {
    return impl_->store(cache_key, upstream_response);
}

bool persistent_cache::erase(const std::string& cache_key) {
    return impl_->erase(cache_key);
}
