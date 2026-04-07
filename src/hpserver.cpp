#include "hpserver.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "net/proxy/http_proxy.h"
#include "logger/logger.h"

namespace {
constexpr std::uint32_t kClientEvents = EPOLLIN | EPOLLET | EPOLLRDHUP;
constexpr int kClientIdleTimeoutMs = 15000;
}

hpserver::~hpserver() {
    if (proxy_event_fd_ != -1) {
        ::close(proxy_event_fd_);
        proxy_event_fd_ = -1;
    }
}

bool hpserver::set_nonblocking(int fd) const {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        LOG_ERROR("fcntl(F_GETFL) failed: {}", strerror(errno));
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERROR("fcntl(F_SETFL) failed: {}", strerror(errno));
        return false;
    }

    return true;
}

void hpserver::close_client(int client_fd) {
    connection_timer_.remove(client_fd);

    if (client_fd >= 0 && static_cast<std::size_t>(client_fd) < proxy_inflight_.size()) {
        proxy_inflight_[client_fd] = false;
    }

    poller_.remove(client_fd);
    if (client_fd >= 0 && static_cast<std::size_t>(client_fd) < connections_.size()) {
        connections_[client_fd].reset();
    }
}

void hpserver::refresh_client_timeout(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= connections_.size() || !connections_[client_fd]) {
        return;
    }

    if (connection_timer_.contains(client_fd)) {
        connection_timer_.adjust(client_fd, kClientIdleTimeoutMs);
        return;
    }

    connection_timer_.add(client_fd, kClientIdleTimeoutMs, [this, client_fd]() {
        if (client_fd < 0 || client_fd >= MAX_FD ||
            static_cast<std::size_t>(client_fd) >= connections_.size() || !connections_[client_fd]) {
            return;
        }
        close_client(client_fd);
    });
}

void hpserver::init() {
    logger::instance().init();

    connections_.resize(MAX_FD);
    proxy_inflight_.assign(MAX_FD, false);
    server_socket = socket_ops(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    server_socket.set_option(SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
}

bool hpserver::init_proxy_async() {
    proxy_event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (proxy_event_fd_ == -1) {
        LOG_ERROR("eventfd creation failed: {}", strerror(errno));
        return false;
    }

    if (poller_.add(proxy_event_fd_, EPOLLIN | EPOLLET) != 0) {
        ::close(proxy_event_fd_);
        proxy_event_fd_ = -1;
        return false;
    }

    return true;
}

void hpserver::submit_proxy_job(int client_fd, http_conn::request_info req) {
    proxy_pool_.push([this, client_fd, req = std::move(req)]() mutable {
        proxy_done_event event;
        event.client_fd = client_fd;
        event.ok = http_proxy::forward_request(req, event.response, &event.err);
        if (!event.ok && event.err == 0) {
            event.err = EIO;
        }

        proxy_done_queue_.push(std::move(event));

        if (proxy_event_fd_ != -1) {
            std::uint64_t one = 1;
            ssize_t ignored = ::write(proxy_event_fd_, &one, sizeof(one));
            (void)ignored;
        }
    });
}

void hpserver::drain_proxy_done_events() {
    if (proxy_event_fd_ != -1) {
        std::uint64_t counter = 0;
        while (::read(proxy_event_fd_, &counter, sizeof(counter)) > 0) {
        }
    }

    proxy_done_event event;
    while (proxy_done_queue_.try_pop(event)) {
        const int client_fd = event.client_fd;
        if (client_fd < 0 || client_fd >= MAX_FD ||
            static_cast<std::size_t>(client_fd) >= connections_.size() ||
            !connections_[client_fd]) {
            continue;
        }

        if (static_cast<std::size_t>(client_fd) < proxy_inflight_.size()) {
            proxy_inflight_[client_fd] = false;
        }

        if (!event.ok) {
            logger::instance().write_log(log_level::error, "Proxy task failed for fd {}", client_fd);
            close_client(client_fd);
            continue;
        }

        connections_[client_fd]->queue_write(event.response);
        if (flush_client_output(client_fd) == -1) {
            close_client(client_fd);
            continue;
        }

        refresh_client_timeout(client_fd);
    }
}

int hpserver::flush_client_output(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= connections_.size() ||
        !connections_[client_fd]) {
        errno = ENOENT;
        return -1;
    }

    http_conn& conn = *connections_[client_fd];
    while (conn.has_pending_write()) {
        ssize_t n = conn.flush_to_socket();
        if (n > 0) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (poller_.modify(client_fd, kClientEvents | EPOLLOUT) != 0) {
                return -1;
            }
            return 0;
        }

        return -1;
    }

    if (poller_.modify(client_fd, kClientEvents) != 0) {
        return -1;
    }

    return 0;
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

    if (!init_proxy_async()) {
        return -1;
    }

    LOG_INFO("Server listening on port {}", port);

    while (true) {
        const int timeout_ms = connection_timer_.get_next_timeout_ms();
        int nfds = poller_.wait(events, MAX_EVENTS, timeout_ms);
        if (nfds == -1) {
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_socket.get_fd()) {
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

                    if (!set_nonblocking(client_fd) || poller_.add(client_fd, kClientEvents)) {
                        ::close(client_fd);
                        continue;
                    }

                    if (client_fd < 0 || client_fd >= MAX_FD) {
                        ::close(client_fd);
                        continue;
                    }

                    connections_[client_fd] = std::make_unique<http_conn>(client_fd);
                    refresh_client_timeout(client_fd);
                }
            } else if (events[i].data.fd == proxy_event_fd_) {
                drain_proxy_done_events();
            } else {
                const int client_fd = events[i].data.fd;
                const std::uint32_t ev = events[i].events;

                if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                    close_client(client_fd);
                    continue;
                }

                if ((ev & (EPOLLIN | EPOLLOUT)) != 0) {
                    refresh_client_timeout(client_fd);
                }

                if ((ev & EPOLLOUT) != 0) {
                    if (flush_client_output(client_fd) == -1) {
                        LOG_ERROR("Failed to flush client fd {}", client_fd);
                        close_client(client_fd);
                        continue;
                    }
                }

                if (handle_client(client_fd) == -1) {
                    LOG_ERROR("Failed to handle client fd {}", client_fd);
                    close_client(client_fd);
                }
            }
        }

        connection_timer_.tick();
    }

    return 0;
}

int hpserver::handle_client(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_FD ||
        static_cast<std::size_t>(client_fd) >= connections_.size() ||
        !connections_[client_fd]) {
        errno = ENOENT;
        return -1;
    }

    if (static_cast<std::size_t>(client_fd) < proxy_inflight_.size() &&
        proxy_inflight_[client_fd]) {
        return 0;
    }

    http_conn& conn = *connections_[client_fd];
    if (conn.read_from_socket() < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }

    if (!conn.parse_available_data()) {
        LOG_ERROR("HTTP parse error: {}", conn.parse_error());
        return -1;
    }

    if (!conn.is_message_complete()) {
        return 0;
    }

    const auto req = conn.request();
    LOG_INFO("HTTP request complete: method={} url={} host={} port={}", req.method, req.url, req.host, req.port);

    conn.reset_for_next_message();
    proxy_inflight_[client_fd] = true;
    submit_proxy_job(client_fd, req);

    return 0;
}