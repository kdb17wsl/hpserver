#pragma once

#include <llhttp.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

class http_response_parser {
public:
    enum class parse_state {
        kReadingResponse,
        kMessageComplete,
        kUpgradedTunnel,
        kParseError,
    };

    struct response_info {
        int status_code = 0;
        std::string reason_phrase;
        std::string version;
        std::uint64_t content_length = 0;
        bool chunked = false;
        bool keep_alive = false;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    http_response_parser();

    bool parse(std::string_view data);

    bool is_message_complete() const {
        return parse_state_ == parse_state::kMessageComplete ||
               parse_state_ == parse_state::kUpgradedTunnel;
    }
    bool is_connect_tunnel() const { return parse_state_ == parse_state::kUpgradedTunnel; }
    bool has_parse_error() const { return parse_state_ == parse_state::kParseError; }

    const response_info& response() const { return response_; }
    parse_state state() const { return parse_state_; }
    const std::string& parse_error() const { return parse_error_; }

    // Signal end-of-stream. Needed for responses without Content-Length
    // (HTTP/1.0 or Connection: close) where the body runs to EOF.
    bool finish();

    void reset_for_next_message();

private:
    static int on_message_begin_cb(llhttp_t* parser);
    static int on_status_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_version_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete_cb(llhttp_t* parser);

    void init_parser();
    void clear_response_runtime();
    void finish_pending_header();
    void post_process_headers();
    static std::string to_lower_ascii(std::string_view value);

    llhttp_t parser_{};
    llhttp_settings_t settings_{};

    parse_state parse_state_ = parse_state::kReadingResponse;
    std::string parse_error_;

    response_info response_;
    std::string current_header_field_;
    std::string current_header_value_;
    bool parsing_last_was_value_ = false;
};
