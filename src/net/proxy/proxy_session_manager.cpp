#include "proxy_session_manager.h"

#include <sys/socket.h>

void proxy_session_manager::mark_upstream_connected(proxy_session& session) {
    session.upstream_connected = true;
    if (session.is_connect) {
        session.state = proxy_session_state::kTunneling;
        session.to_client.append("HTTP/1.1 200 Connection Established\r\n"
                                 "Proxy-Agent: hpserver\r\n"
                                 "\r\n");
        return;
    }

    session.state = proxy_session_state::kForwardingHttp;
}

void proxy_session_manager::maybe_shutdown_peer_after_read_close(proxy_session& session) {
    if (session.client_read_closed && session.to_upstream.empty() && session.upstream_fd >= 0) {
        ::shutdown(session.upstream_fd, SHUT_WR);
    }
    if (session.upstream_read_closed && session.to_client.empty() && session.client_fd >= 0) {
        ::shutdown(session.client_fd, SHUT_WR);
    }
}

bool proxy_session_manager::should_close_session(const proxy_session& session) {
    if (session.state == proxy_session_state::kForwardingHttp) {
        return session.upstream_read_closed && session.to_client.empty();
    }

    if (session.state == proxy_session_state::kTunneling) {
        return (session.client_read_closed || session.upstream_read_closed) &&
               session.to_client.empty() && session.to_upstream.empty();
    }

    return false;
}