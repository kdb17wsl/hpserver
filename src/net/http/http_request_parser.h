#pragma once

#include <llhttp.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

class http_request_parser {
public:
    enum class parse_state {
        kReadingRequest,
        kMessageComplete,
        kUpgradedTunnel,
        kParseError,
    };

    struct request_info {
        std::string method;
        std::string url;
        std::string version;
        std::string host;
        std::uint16_t port = 0;
        std::uint64_t content_length = 0;
        bool chunked = false;
        bool keep_alive = false;
        bool is_connect = false;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    http_request_parser();

    bool parse(std::string_view data);

    bool is_message_complete() const {
        return parse_state_ == parse_state::kMessageComplete ||
               parse_state_ == parse_state::kUpgradedTunnel;
    }
    bool is_connect_tunnel() const { return parse_state_ == parse_state::kUpgradedTunnel; }
    bool has_parse_error() const { return parse_state_ == parse_state::kParseError; }

    const request_info& request() const { return request_; }
    parse_state state() const { return parse_state_; }
    const std::string& parse_error() const { return parse_error_; }

    void reset_for_next_message();

private:
    static int on_message_begin_cb(llhttp_t* parser);
    static int on_url_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_version_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_headers_complete_cb(llhttp_t* parser);
    static int on_body_cb(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete_cb(llhttp_t* parser);

    void init_parser();
    void clear_request_runtime();
    void finish_pending_header();
    void post_process_headers();
    static std::string to_lower_ascii(std::string_view value);
    static void parse_host_port(std::string_view host_port, std::string& host,
                                std::uint16_t& port);

    llhttp_t parser_{};
    llhttp_settings_t settings_{};

    parse_state parse_state_ = parse_state::kReadingRequest;
    std::string parse_error_;

    request_info request_;
    std::string current_header_field_;
    std::string current_header_value_;
    bool parsing_last_was_value_ = false;
};
