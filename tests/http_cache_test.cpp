#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "net/http/http_cache.h"
#include "net/http/http_request_parser.h"

namespace {

http_request_parser::request_info make_get_request(
    std::string host, std::uint16_t port, std::string url,
    std::unordered_map<std::string, std::string> headers = {}) {
    http_request_parser::request_info req;
    req.method = "GET";
    req.host = std::move(host);
    req.port = port;
    req.url = std::move(url);
    req.headers = std::move(headers);
    return req;
}

std::string make_upstream_response(int status_code, std::string_view reason,
                                   std::string_view body,
                                   std::string_view extra_headers = "") {
    std::string resp;
    resp.reserve(256 + body.size());
    resp.append("HTTP/1.1 ");
    resp.append(std::to_string(status_code));
    resp.push_back(' ');
    resp.append(reason);
    resp.append("\r\n");
    resp.append("Content-Length: ");
    resp.append(std::to_string(body.size()));
    resp.append("\r\n");
    if (!extra_headers.empty()) {
        resp.append(extra_headers);
    }
    resp.append("\r\n");
    resp.append(body);
    return resp;
}

class HttpCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "hpserver_cache_test_XXXXXX";
        // Create a temporary directory.
        std::string tmpl = db_path_.string();
        char* dir = mkdtemp(tmpl.data());
        ASSERT_NE(dir, nullptr);
        db_path_ = dir;
    }

    void TearDown() override {
        std::filesystem::remove_all(db_path_);
    }

    std::filesystem::path db_path_;
};

TEST_F(HttpCacheTest, NotReadyReturnsError) {
    http_cache cache;
    auto req = make_get_request("example.com", 80, "/");
    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kError);
}

TEST_F(HttpCacheTest, NonGetRequestBypasses) {
    http_cache cache;
    ASSERT_TRUE(cache.open(db_path_.string()));

    http_request_parser::request_info req;
    req.method = "POST";
    req.host = "example.com";
    req.port = 80;
    req.url = "/";

    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kBypass);
}

TEST_F(HttpCacheTest, ConnectRequestBypasses) {
    http_cache cache;
    ASSERT_TRUE(cache.open(db_path_.string()));

    http_request_parser::request_info req;
    req.method = "GET";
    req.host = "example.com";
    req.port = 80;
    req.url = "/";
    req.is_connect = true;

    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kBypass);
}

TEST_F(HttpCacheTest, StoreAndL2Hit) {
    http_cache cache;
    cache.configure_l1(0, 0);  // Disable L1 to force L2 path.
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/index.html");
    std::string upstream = make_upstream_response(200, "OK", "hello world");
    ASSERT_TRUE(cache.store(req, upstream));

    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kHit);
    // L2 rebuilds from parsed headers (keys lowercased), so exact string match
    // against the original upstream text may differ in header casing.
    EXPECT_TRUE(result.response.find("HTTP/1.1 200 OK") != std::string::npos);
    EXPECT_TRUE(result.response.find("hello world") != std::string::npos);
}

TEST_F(HttpCacheTest, L1Hit) {
    http_cache cache;
    cache.configure_l1(100, 1024 * 1024);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/index.html");
    std::string upstream = make_upstream_response(200, "OK", "hello l1");
    ASSERT_TRUE(cache.store(req, upstream));

    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kHit);
    EXPECT_EQ(result.response, upstream);

    // Second lookup should also hit (L1 path).
    auto result2 = cache.lookup(req);
    EXPECT_EQ(result2.status, http_cache::lookup_status::kHit);
    EXPECT_EQ(result2.response, upstream);
}

TEST_F(HttpCacheTest, MissWhenNotStored) {
    http_cache cache;
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/missing.html");
    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kMiss);
}

TEST_F(HttpCacheTest, NotModifiedByEtag) {
    http_cache cache;
    cache.configure_l1(0, 0);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/resource");
    std::string upstream = make_upstream_response(
        200, "OK", "body",
        "ETag: \"abc123\"\r\n"
        "Cache-Control: max-age=3600\r\n");
    ASSERT_TRUE(cache.store(req, upstream));

    auto conditional_req = make_get_request("example.com", 80, "/resource",
                                            {{"if-none-match", "\"abc123\""}});
    auto result = cache.lookup(conditional_req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kNotModified);
    EXPECT_TRUE(result.response.find("304 Not Modified") != std::string::npos);
}

TEST_F(HttpCacheTest, NotModifiedByLastModified) {
    http_cache cache;
    cache.configure_l1(0, 0);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/resource");
    std::string upstream = make_upstream_response(
        200, "OK", "body",
        "Last-Modified: Wed, 21 Oct 2025 07:28:00 GMT\r\n"
        "Cache-Control: max-age=3600\r\n");
    ASSERT_TRUE(cache.store(req, upstream));

    auto conditional_req = make_get_request(
        "example.com", 80, "/resource",
        {{"if-modified-since", "Wed, 21 Oct 2025 07:28:00 GMT"}});
    auto result = cache.lookup(conditional_req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kNotModified);
}

TEST_F(HttpCacheTest, ConditionalMissWhenEtagMismatch) {
    http_cache cache;
    cache.configure_l1(0, 0);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/resource");
    std::string upstream = make_upstream_response(
        200, "OK", "body",
        "ETag: \"abc123\"\r\n"
        "Cache-Control: max-age=3600\r\n");
    ASSERT_TRUE(cache.store(req, upstream));

    auto conditional_req = make_get_request(
        "example.com", 80, "/resource",
        {{"if-none-match", "\"different\""}});
    auto result = cache.lookup(conditional_req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kHit);
}

TEST_F(HttpCacheTest, Non2xxResponseNotCached) {
    http_cache cache;
    cache.configure_l1(0, 0);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/error");
    std::string upstream = make_upstream_response(500, "Internal Error", "oops");
    EXPECT_FALSE(cache.store(req, upstream));
}

TEST_F(HttpCacheTest, LargeResponseSkipsL1) {
    http_cache cache;
    cache.configure_l1(100, 1024 * 1024);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/big");
    std::string large_body(100 * 1024, 'x');
    std::string upstream = make_upstream_response(200, "OK", large_body);
    ASSERT_TRUE(cache.store(req, upstream));

    // L1 should miss because body is large.
    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kHit);
    EXPECT_TRUE(result.response.find("HTTP/1.1 200 OK") != std::string::npos);
    EXPECT_TRUE(result.response.find(large_body) != std::string::npos);
}

TEST_F(HttpCacheTest, VaryHeadersAreStored) {
    http_cache cache;
    cache.configure_l1(0, 0);
    ASSERT_TRUE(cache.open(db_path_.string()));

    auto req = make_get_request("example.com", 80, "/resource");
    std::string upstream = make_upstream_response(
        200, "OK", "body",
        "Vary: Accept-Encoding\r\n"
        "Cache-Control: max-age=3600\r\n");
    ASSERT_TRUE(cache.store(req, upstream));

    auto result = cache.lookup(req);
    EXPECT_EQ(result.status, http_cache::lookup_status::kHit);
}

}  // namespace
