#pragma once

#include <string>
#include <string_view>

#include "http_connection_io.h"
#include "http_request_parser.h"

class http_conn {
public:
    using parse_state = http_request_parser::parse_state;
    using request_info = http_request_parser::request_info;

    explicit http_conn(int fd = -1);
    ~http_conn() = default;

    http_conn(const http_conn&) = delete;
    http_conn& operator=(const http_conn&) = delete;
    http_conn(http_conn&&) = delete;
    http_conn& operator=(http_conn&&) = delete;

    int fd() const { return io_.fd(); }
    bool valid() const { return io_.valid(); }

    ssize_t read_from_socket();
    ssize_t flush_to_socket();
    void queue_write(std::string_view data);
    bool has_pending_write() const;

    bool parse_available_data();
    bool is_message_complete() const { return parser_.is_message_complete(); }
    bool is_connect_tunnel() const { return parser_.is_connect_tunnel(); }
    bool has_parse_error() const { return parser_.has_parse_error(); }

    const request_info& request() const { return parser_.request(); }
    parse_state state() const { return parser_.state(); }
    const std::string& parse_error() const { return parser_.parse_error(); }

    const std::string& read_buffer() const { return io_.read_buffer(); }
    std::string take_write_buffer();

    void reset_for_next_message();

private:
    http_connection_io io_;
    http_request_parser parser_;
    std::size_t parse_offset_ = 0;
};