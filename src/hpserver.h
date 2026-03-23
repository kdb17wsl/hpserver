#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "http_conn.h"
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

    enum class session_state : std::uint8_t {
        kReadingRequest,
        kConnectingUpstream,
        kForwardingHttp,
        kTunneling,
        kClosing,
    };

    struct proxy_session {
        int client_fd = -1;
        int upstream_fd = -1;
        bool is_connect = false;
        bool upstream_connected = false;
        bool client_read_closed = false;
        bool upstream_read_closed = false;
        session_state state = session_state::kReadingRequest;

        std::string to_upstream;
        std::size_t to_upstream_offset = 0;
        std::string to_client;
        std::size_t to_client_offset = 0;
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

    int open_upstream_nonblocking(const std::string& host, std::uint16_t port,
                                  int& upstream_fd, bool& connected_immediately) const;
    int finalize_connect_if_needed(proxy_session& session);

    int read_client_into_upstream_buffer(proxy_session& session);
    int read_upstream_into_client_buffer(proxy_session& session);
    int flush_buffer_to_fd(int fd, std::string& buffer, std::size_t& offset);

    int refresh_client_interest(int client_fd);
    int refresh_upstream_interest(int upstream_fd);

    void maybe_shutdown_peer_after_read_close(proxy_session& session);
    bool should_close_session(const proxy_session& session) const;

    static std::string build_origin_form(const std::string& url);
    static bool is_hop_by_hop_header(const std::string& key);
    static std::string build_forward_request(const http_conn::request_info& req);
};