#include "proxy_event_dispatcher.h"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>

#include "relay_engine.h"

int proxy_event_dispatcher::finalize_connect_if_needed(proxy_session& session) {
    if (session.upstream_connected) {
        return 0;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(session.upstream_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
        return -1;
    }
    if (so_error != 0) {
        errno = so_error;
        return -1;
    }

    proxy_session_manager::mark_upstream_connected(session);
    return 0;
}

int proxy_event_dispatcher::handle_client_session_io(proxy_session& session,
                                                     std::uint32_t events_mask) {
    if ((events_mask & EPOLLRDHUP) != 0) {
        session.client_read_closed = true;
    }

    if ((events_mask & EPOLLIN) != 0 && session.state == proxy_session_state::kTunneling) {
        if (relay_engine::read_from_fd_into_buffer(session.client_fd, session.to_upstream,
                                                    session.client_read_closed) != 0) {
            return -1;
        }
    }

    if ((events_mask & EPOLLOUT) != 0 && !session.to_client.empty()) {
        if (relay_engine::flush_buffer_to_fd(session.client_fd, session.to_client,
                                             session.to_client_offset) != 0) {
            return -1;
        }
    }

    proxy_session_manager::maybe_shutdown_peer_after_read_close(session);
    return 0;
}

int proxy_event_dispatcher::handle_upstream_session_io(proxy_session& session,
                                                       std::uint32_t events_mask) {
    if ((events_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        session.upstream_read_closed = true;
    }

    if (!session.upstream_connected && (events_mask & EPOLLOUT) != 0) {
        if (finalize_connect_if_needed(session) != 0) {
            return -1;
        }
    }

    if (session.upstream_connected && (events_mask & EPOLLOUT) != 0 &&
        !session.to_upstream.empty()) {
        if (relay_engine::flush_buffer_to_fd(session.upstream_fd, session.to_upstream,
                                             session.to_upstream_offset) != 0) {
            return -1;
        }
    }

    if (session.upstream_connected && (events_mask & EPOLLIN) != 0) {
        if (relay_engine::read_from_fd_into_buffer(session.upstream_fd, session.to_client,
                                                    session.upstream_read_closed) != 0) {
            return -1;
        }
    }

    proxy_session_manager::maybe_shutdown_peer_after_read_close(session);
    return 0;
}