#include "hpserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace {
constexpr std::uint32_t kBaseClientEvents = EPOLLIN | EPOLLET | EPOLLRDHUP;
constexpr std::uint32_t kBaseUpstreamEvents = EPOLLIN | EPOLLET | EPOLLRDHUP;
constexpr std::size_t kRelayChunk = 8192;
}

bool hpserver::set_nonblocking(int fd) const {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return false;
    }

    return true;
}

void hpserver::close_client(int client_fd) {
    poller_.remove(client_fd);
    if (client_fd >= 0 && client_fd < MAX_FD) {
        roles_[client_fd] = fd_role::kNone;
    }
    if (client_fd >= 0 && static_cast<std::size_t>(client_fd) < connections_.size()) {
        connections_[client_fd].reset();
    }
    if (client_fd >= 0 && static_cast<std::size_t>(client_fd) < sessions_.size()) {
        sessions_[client_fd].reset();
    }
}

void hpserver::close_session(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= sessions_.size()) {
        close_client(client_fd);
        return;
    }

    std::unique_ptr<proxy_session> session = std::move(sessions_[client_fd]);
    if (session && session->upstream_fd >= 0 && session->upstream_fd < MAX_FD) {
        poller_.remove(session->upstream_fd);
        ::close(session->upstream_fd);
        roles_[session->upstream_fd] = fd_role::kNone;
        upstream_to_client_[session->upstream_fd] = -1;
    }

    close_client(client_fd);
}

void hpserver::init() {
    connections_.resize(MAX_FD);
    sessions_.resize(MAX_FD);
    roles_.assign(MAX_FD, fd_role::kNone);
    upstream_to_client_.assign(MAX_FD, -1);

    server_socket = socket_ops(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    server_socket.set_option(SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
}

int hpserver::listen() {
    init();

    if (!set_nonblocking(server_socket.get_fd())) {
        return -1;
    }

    if (server_socket.bind((struct sockaddr*)&server_addr, sizeof(server_addr))) {
        return -1;
    }
    if (server_socket.listen(SOMAXCONN)) {
        return -1;
    }

    if (poller_.add(server_socket.get_fd(), EPOLLIN | EPOLLET)) {
        return -1;
    }
    roles_[server_socket.get_fd()] = fd_role::kListener;

    std::cout << "Server listening on port " << port << std::endl;

    while (true) {
        int nfds = poller_.wait(events, MAX_EVENTS, -1);
        if (nfds == -1) {
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            const int fd = events[i].data.fd;
            const std::uint32_t ev = events[i].events;

            if (fd == server_socket.get_fd()) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd =
                        server_socket.accept((struct sockaddr*)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        break;
                    }

                    if (!set_nonblocking(client_fd) ||
                        poller_.add(client_fd, kBaseClientEvents)) {
                        ::close(client_fd);
                        continue;
                    }

                    if (client_fd < 0 || client_fd >= MAX_FD) {
                        ::close(client_fd);
                        continue;
                    }

                    connections_[client_fd] = std::make_unique<http_conn>(client_fd);
                    roles_[client_fd] = fd_role::kClient;
                }
            } else {
                if (fd < 0 || fd >= MAX_FD) {
                    continue;
                }

                int ret = 0;
                if (roles_[fd] == fd_role::kClient) {
                    ret = handle_client(fd, ev);
                } else if (roles_[fd] == fd_role::kUpstream) {
                    ret = handle_upstream(fd, ev);
                } else {
                    continue;
                }

                if (ret == -1) {
                    if (roles_[fd] == fd_role::kUpstream) {
                        const int owner = upstream_to_client_[fd];
                        if (owner >= 0) {
                            close_session(owner);
                        } else {
                            poller_.remove(fd);
                            ::close(fd);
                            roles_[fd] = fd_role::kNone;
                        }
                    } else {
                        close_session(fd);
                    }
                }
            }
        }
    }

    return 0;
}

int hpserver::open_upstream_nonblocking(const std::string& host, std::uint16_t port,
                                        int& upstream_fd,
                                        bool& connected_immediately) const {
    upstream_fd = -1;
    connected_immediately = false;

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return -1;
    }

    int ret = -1;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd == -1) {
            continue;
        }

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            upstream_fd = fd;
            connected_immediately = true;
            ret = 0;
            break;
        }

        if (errno == EINPROGRESS) {
            upstream_fd = fd;
            ret = 0;
            break;
        }

        ::close(fd);
    }

    ::freeaddrinfo(result);
    return ret;
}

int hpserver::finalize_connect_if_needed(proxy_session& session) {
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

    session.upstream_connected = true;
    if (session.is_connect) {
        session.state = session_state::kTunneling;
        session.to_client.append("HTTP/1.1 200 Connection Established\r\n"
                                 "Proxy-Agent: hpserver\r\n"
                                 "\r\n");
    } else {
        session.state = session_state::kForwardingHttp;
    }
    return 0;
}

int hpserver::flush_buffer_to_fd(int fd, std::string& buffer, std::size_t& offset) {
    while (offset < buffer.size()) {
        const ssize_t n = ::send(fd, buffer.data() + offset, buffer.size() - offset, 0);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        return -1;
    }

    buffer.clear();
    offset = 0;
    return 0;
}

int hpserver::read_client_into_upstream_buffer(proxy_session& session) {
    char buf[kRelayChunk];
    while (true) {
        const ssize_t n = ::recv(session.client_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            session.to_upstream.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            session.client_read_closed = true;
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

int hpserver::read_upstream_into_client_buffer(proxy_session& session) {
    char buf[kRelayChunk];
    while (true) {
        const ssize_t n = ::recv(session.upstream_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            session.to_client.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            session.upstream_read_closed = true;
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

void hpserver::maybe_shutdown_peer_after_read_close(proxy_session& session) {
    if (session.client_read_closed && session.to_upstream.empty() && session.upstream_fd >= 0) {
        ::shutdown(session.upstream_fd, SHUT_WR);
    }
    if (session.upstream_read_closed && session.to_client.empty() && session.client_fd >= 0) {
        ::shutdown(session.client_fd, SHUT_WR);
    }
}

bool hpserver::should_close_session(const proxy_session& session) const {
    if (session.state == session_state::kForwardingHttp) {
        return session.upstream_read_closed && session.to_client.empty();
    }
    if (session.state == session_state::kTunneling) {
        return (session.client_read_closed || session.upstream_read_closed) &&
               session.to_client.empty() && session.to_upstream.empty();
    }
    return false;
}

int hpserver::refresh_client_interest(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= connections_.size() || !connections_[client_fd]) {
        return -1;
    }

    std::uint32_t events_mask = kBaseClientEvents;
    if (static_cast<std::size_t>(client_fd) < sessions_.size() && sessions_[client_fd] &&
        !sessions_[client_fd]->to_client.empty()) {
        events_mask |= EPOLLOUT;
    }
    return poller_.modify(client_fd, events_mask);
}

int hpserver::refresh_upstream_interest(int upstream_fd) {
    if (upstream_fd < 0 || upstream_fd >= MAX_FD ||
        static_cast<std::size_t>(upstream_fd) >= upstream_to_client_.size()) {
        return -1;
    }
    const int client_fd = upstream_to_client_[upstream_fd];
    if (client_fd < 0 || client_fd >= MAX_FD || static_cast<std::size_t>(client_fd) >= sessions_.size() ||
        !sessions_[client_fd]) {
        return -1;
    }

    const proxy_session& session = *sessions_[client_fd];
    std::uint32_t events_mask = kBaseUpstreamEvents;
    if (!session.upstream_connected || !session.to_upstream.empty()) {
        events_mask |= EPOLLOUT;
    }
    return poller_.modify(upstream_fd, events_mask);
}

std::string hpserver::build_origin_form(const std::string& url) {
    if (url.rfind("http://", 0) == 0) {
        const std::size_t host_begin = std::strlen("http://");
        const std::size_t path_pos = url.find('/', host_begin);
        return path_pos == std::string::npos ? "/" : url.substr(path_pos);
    }
    if (url.rfind("https://", 0) == 0) {
        const std::size_t host_begin = std::strlen("https://");
        const std::size_t path_pos = url.find('/', host_begin);
        return path_pos == std::string::npos ? "/" : url.substr(path_pos);
    }
    if (url.empty()) {
        return "/";
    }
    if (url == "*") {
        return url;
    }
    if (!url.empty() && url.front() != '/') {
        return std::string("/") + url;
    }
    return url;
}

bool hpserver::is_hop_by_hop_header(const std::string& key) {
    return key == "connection" || key == "proxy-connection" || key == "keep-alive" ||
           key == "te" || key == "trailer" || key == "transfer-encoding" ||
           key == "upgrade";
}

std::string hpserver::build_forward_request(const http_conn::request_info& req) {
    const std::string target = build_origin_form(req.url);
    const std::string version = req.version.empty() ? "1.1" : req.version;

    std::string out;
    out.reserve(1024 + req.body.size());
    out.append(req.method);
    out.push_back(' ');
    out.append(target);
    out.append(" HTTP/");
    out.append(version);
    out.append("\r\n");

    bool has_host = false;
    bool has_content_length = false;
    for (const auto& kv : req.headers) {
        std::string key = kv.first;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (is_hop_by_hop_header(key)) {
            continue;
        }

        if (key == "host") {
            has_host = true;
        }
        if (key == "content-length") {
            has_content_length = true;
        }

        out.append(key);
        out.append(": ");
        out.append(kv.second);
        out.append("\r\n");
    }

    if (!has_host) {
        out.append("host: ");
        out.append(req.host);
        if (req.port != 0 && req.port != 80) {
            out.push_back(':');
            out.append(std::to_string(req.port));
        }
        out.append("\r\n");
    }

    if (!has_content_length && !req.body.empty()) {
        out.append("content-length: ");
        out.append(std::to_string(req.body.size()));
        out.append("\r\n");
    }

    out.append("connection: close\r\n");
    out.append("\r\n");
    out.append(req.body);
    return out;
}

int hpserver::start_proxy_session(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= connections_.size() ||
        !connections_[client_fd]) {
        errno = ENOENT;
        return -1;
    }

    http_conn& conn = *connections_[client_fd];
    const auto& req = conn.request();
    if (req.host.empty() || req.port == 0) {
        return -1;
    }

    auto session = std::make_unique<proxy_session>();
    session->client_fd = client_fd;
    session->state = session_state::kConnectingUpstream;
    session->is_connect = req.is_connect;

    bool connected_immediately = false;
    if (open_upstream_nonblocking(req.host, req.port, session->upstream_fd,
                                  connected_immediately) != 0) {
        return -1;
    }

    if (poller_.add(session->upstream_fd, kBaseUpstreamEvents | EPOLLOUT) != 0) {
        ::close(session->upstream_fd);
        return -1;
    }

    roles_[session->upstream_fd] = fd_role::kUpstream;
    upstream_to_client_[session->upstream_fd] = client_fd;

    if (req.is_connect) {
        if (connected_immediately) {
            session->upstream_connected = true;
            session->state = session_state::kTunneling;
            session->to_client.append("HTTP/1.1 200 Connection Established\r\n"
                                      "Proxy-Agent: hpserver\r\n"
                                      "\r\n");
        }
    } else {
        session->to_upstream = build_forward_request(req);
        if (session->to_upstream.empty()) {
            poller_.remove(session->upstream_fd);
            ::close(session->upstream_fd);
            roles_[session->upstream_fd] = fd_role::kNone;
            upstream_to_client_[session->upstream_fd] = -1;
            return -1;
        }
        if (connected_immediately) {
            session->upstream_connected = true;
            session->state = session_state::kForwardingHttp;
        }
    }

    sessions_[client_fd] = std::move(session);
    conn.reset_for_next_message();

    if (refresh_client_interest(client_fd) != 0) {
        return -1;
    }
    if (refresh_upstream_interest(sessions_[client_fd]->upstream_fd) != 0) {
        return -1;
    }

    return 0;
}

int hpserver::handle_client(int client_fd, std::uint32_t events_mask) {
    if ((events_mask & (EPOLLERR | EPOLLHUP)) != 0) {
        return -1;
    }

    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= connections_.size() ||
        !connections_[client_fd]) {
        errno = ENOENT;
        return -1;
    }

    if (!sessions_[client_fd]) {
        http_conn& conn = *connections_[client_fd];
        if ((events_mask & EPOLLIN) != 0) {
            if (conn.read_from_socket() < 0) {
                return -1;
            }

            if (!conn.parse_available_data()) {
                std::cerr << "HTTP parse error: " << conn.parse_error() << std::endl;
                return -1;
            }

            if (conn.is_message_complete() && start_proxy_session(client_fd) != 0) {
                return -1;
            }
        }
        return 0;
    }

    proxy_session& session = *sessions_[client_fd];

    if ((events_mask & EPOLLRDHUP) != 0) {
        session.client_read_closed = true;
    }

    if ((events_mask & EPOLLIN) != 0 && session.state == session_state::kTunneling) {
        if (read_client_into_upstream_buffer(session) != 0) {
            return -1;
        }
    }

    if ((events_mask & EPOLLOUT) != 0 && !session.to_client.empty()) {
        if (flush_buffer_to_fd(client_fd, session.to_client, session.to_client_offset) != 0) {
            return -1;
        }
    }

    maybe_shutdown_peer_after_read_close(session);

    if (refresh_client_interest(client_fd) != 0 ||
        refresh_upstream_interest(session.upstream_fd) != 0) {
        return -1;
    }

    if (should_close_session(session)) {
        return -1;
    }

    return 0;
}

int hpserver::handle_upstream(int upstream_fd, std::uint32_t events_mask) {
    if (upstream_fd < 0 || upstream_fd >= MAX_FD ||
        static_cast<std::size_t>(upstream_fd) >= upstream_to_client_.size()) {
        errno = ENOENT;
        return -1;
    }

    const int client_fd = upstream_to_client_[upstream_fd];
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= sessions_.size() || !sessions_[client_fd]) {
        errno = ENOENT;
        return -1;
    }

    proxy_session& session = *sessions_[client_fd];

    if ((events_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        session.upstream_read_closed = true;
    }

    if (!session.upstream_connected && (events_mask & EPOLLOUT) != 0) {
        if (finalize_connect_if_needed(session) != 0) {
            return -1;
        }
    }

    if (session.upstream_connected && (events_mask & EPOLLOUT) != 0 && !session.to_upstream.empty()) {
        if (flush_buffer_to_fd(upstream_fd, session.to_upstream, session.to_upstream_offset) != 0) {
            return -1;
        }
    }

    if (session.upstream_connected && (events_mask & EPOLLIN) != 0) {
        if (read_upstream_into_client_buffer(session) != 0) {
            return -1;
        }
    }

    maybe_shutdown_peer_after_read_close(session);

    if (refresh_client_interest(client_fd) != 0 || refresh_upstream_interest(upstream_fd) != 0) {
        return -1;
    }

    if (should_close_session(session)) {
        return -1;
    }

    return 0;
}