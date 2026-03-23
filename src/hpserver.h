#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "http_conn.h"
#include "net/proxy/proxy_session_manager.h"
#include "net/proxy/relay_engine.h"
#include "net/proxy/upstream_connector.h"
#include "poller.h"
#include "socket_ops.h"

const int MAX_FD = 65536;
const int MAX_EVENTS = 1024;
const int DEFAULT_PORT = 8080;

class hpserver {
public:
    hpserver(int port = DEFAULT_PORT) : port(port) {};
    ~hpserver() = default;

    int listen();

private:
    enum class fd_role : std::uint8_t {
        kNone,
        kListener,
        kClient,
        kUpstream,
    };

    struct sockaddr_in server_addr;
    socket_ops server_socket;
    int port;

    poller poller_;
    struct epoll_event events[MAX_EVENTS];

    std::vector<std::unique_ptr<http_conn>> connections_;
    std::vector<std::unique_ptr<proxy_session>> sessions_;
    std::vector<fd_role> roles_;
    std::vector<int> upstream_to_client_;

    void init();
    bool set_nonblocking(int fd) const;
    void close_client(int client_fd);
    void close_session(int client_fd);

    int handle_client(int client_fd, std::uint32_t events_mask);
    int handle_upstream(int upstream_fd, std::uint32_t events_mask);
    int start_proxy_session(int client_fd);

    int finalize_connect_if_needed(proxy_session& session);

    int refresh_client_interest(int client_fd);
    int refresh_upstream_interest(int upstream_fd);

    static std::string build_origin_form(const std::string& url);
    static bool is_hop_by_hop_header(const std::string& key);
    static std::string build_forward_request(const http_conn::request_info& req);
};