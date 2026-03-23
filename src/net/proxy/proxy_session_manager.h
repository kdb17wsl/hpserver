#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class proxy_session_state : std::uint8_t {
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
    proxy_session_state state = proxy_session_state::kReadingRequest;

    std::string to_upstream;
    std::size_t to_upstream_offset = 0;
    std::string to_client;
    std::size_t to_client_offset = 0;
};

class proxy_session_manager {
public:
    static void mark_upstream_connected(proxy_session& session);
    static void maybe_shutdown_peer_after_read_close(proxy_session& session);
    static bool should_close_session(const proxy_session& session);
};