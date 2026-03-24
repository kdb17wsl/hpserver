#pragma once

#include <cstdint>
#include <string>

#include "http_conn.h"

class http_proxy {
private:
    static constexpr int kIoTimeoutMs = 10000;

    http_conn& conn;

    bool wait_fd(int fd, short events) const;
    bool connect_upstream(const std::string& host, std::uint16_t port, int& upstream_fd) const;
    bool send_all_nonblocking(int fd, const std::string& data) const;
    bool relay_response_to_client(int upstream_fd);
    std::string build_forward_request() const;
    static std::string build_origin_form(const std::string& url);
    static bool is_hop_by_hop_header(const std::string& key);
    void queue_error_response(int status, const char* reason, const char* body);

public:
    explicit http_proxy(http_conn& conn) : conn(conn) {};
    static bool forward_request(const http_conn::request_info& req,
                                std::string& out_response, int* out_errno = nullptr);
    bool handle_request();
};