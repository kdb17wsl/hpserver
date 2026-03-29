#pragma once

#include <cstdint>
#include <string>

#include "http_conn.h"

class http_proxy {
private:
    static constexpr int kIoTimeoutMs = 10000;

    static bool wait_fd(int fd, std::uint32_t events);
    static bool connect_upstream(const std::string& host, std::uint16_t port,
                                 int& upstream_fd);
    static bool send_all_nonblocking(int fd, const std::string& data);
    static std::string build_forward_request(const http_conn::request_info& req);
    static std::string build_origin_form(const std::string& url);
    static bool is_hop_by_hop_header(const std::string& key);

public:
    static bool forward_request(const http_conn::request_info& req,
                                std::string& out_response, int* out_errno = nullptr);
};