#include <gtest/gtest.h>

#include "src/net/http/http_response_parser.h"

TEST(HttpResponseParserTest, ParsesSimpleResponse) {
    http_response_parser parser;

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "hello";

    EXPECT_TRUE(parser.parse(response));
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_FALSE(parser.has_parse_error());

    const auto& resp = parser.response();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.reason_phrase, "OK");
    EXPECT_EQ(resp.version, "1.1");
    EXPECT_TRUE(resp.keep_alive);
    EXPECT_FALSE(resp.chunked);
    EXPECT_EQ(resp.content_length, 5U);
    EXPECT_EQ(resp.body, "hello");
    ASSERT_EQ(resp.headers.count("content-length"), 1U);
    EXPECT_EQ(resp.headers.at("content-length"), "5");
    ASSERT_EQ(resp.headers.count("connection"), 1U);
    EXPECT_EQ(resp.headers.at("connection"), "keep-alive");
}

TEST(HttpResponseParserTest, HandlesChunkedResponseHeaders) {
    http_response_parser parser;

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    EXPECT_TRUE(parser.parse(response));
    EXPECT_TRUE(parser.is_message_complete());

    const auto& resp = parser.response();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.reason_phrase, "OK");
    EXPECT_TRUE(resp.chunked);
    EXPECT_EQ(resp.headers.at("transfer-encoding"), "chunked");
    EXPECT_EQ(resp.body, "hello");
}

TEST(HttpResponseParserTest, ReportsParseErrorForInvalidResponse) {
    http_response_parser parser;

    EXPECT_FALSE(parser.parse("BAD RESPONSE\r\n\r\n"));
    EXPECT_TRUE(parser.has_parse_error());
    EXPECT_FALSE(parser.parse_error().empty());
}
