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

TEST(HttpResponseParserTest, IncrementalParsing) {
    http_response_parser parser;

    const std::string part1 = "HTTP/1.1 200 OK\r\nContent-Len";
    const std::string part2 = "gth: 5\r\n\r\nhello";

    EXPECT_TRUE(parser.parse(part1));
    EXPECT_FALSE(parser.is_message_complete());

    EXPECT_TRUE(parser.parse(part2));
    EXPECT_TRUE(parser.is_message_complete());

    const auto& resp = parser.response();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.content_length, 5U);
    EXPECT_EQ(resp.body, "hello");
}

TEST(HttpResponseParserTest, FinishSignalsEofForResponseWithoutContentLength) {
    http_response_parser parser;

    const std::string response =
        "HTTP/1.0 200 OK\r\n"
        "\r\n"
        "some body data";

    EXPECT_TRUE(parser.parse(response));
    EXPECT_FALSE(parser.is_message_complete());

    EXPECT_TRUE(parser.finish());
    EXPECT_TRUE(parser.is_message_complete());

    const auto& resp = parser.response();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "some body data");
    EXPECT_FALSE(resp.keep_alive);
}

TEST(HttpResponseParserTest, FinishOnAlreadyCompleteIsHarmless) {
    http_response_parser parser;

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    EXPECT_TRUE(parser.parse(response));
    EXPECT_TRUE(parser.is_message_complete());

    EXPECT_TRUE(parser.finish());
    EXPECT_TRUE(parser.is_message_complete());
}

TEST(HttpResponseParserTest, ChunkedParsingIncremental) {
    http_response_parser parser;

    EXPECT_TRUE(parser.parse(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"));
    EXPECT_FALSE(parser.is_message_complete());

    EXPECT_TRUE(parser.parse("5\r\nhello\r\n"));
    EXPECT_FALSE(parser.is_message_complete());

    EXPECT_TRUE(parser.parse("0\r\n\r\n"));
    EXPECT_TRUE(parser.is_message_complete());

    const auto& resp = parser.response();
    EXPECT_EQ(resp.body, "hello");
    EXPECT_TRUE(resp.chunked);
}

TEST(HttpResponseParserTest, ResetForNextMessage) {
    http_response_parser parser;

    const std::string response1 =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "hi";

    EXPECT_TRUE(parser.parse(response1));
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_EQ(parser.response().body, "hi");

    parser.reset_for_next_message();
    EXPECT_FALSE(parser.is_message_complete());
    EXPECT_EQ(parser.response().status_code, 0);
    EXPECT_TRUE(parser.response().body.empty());

    const std::string response2 =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    EXPECT_TRUE(parser.parse(response2));
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_EQ(parser.response().status_code, 404);
}

TEST(HttpResponseParserTest, ParseAfterCompleteIsNoop) {
    http_response_parser parser;

    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    EXPECT_TRUE(parser.parse(response));
    EXPECT_TRUE(parser.is_message_complete());

    // Extra parse calls after completion should return true without error.
    EXPECT_TRUE(parser.parse("extra garbage"));
    EXPECT_TRUE(parser.is_message_complete());
    EXPECT_EQ(parser.response().body, "hello");
}
