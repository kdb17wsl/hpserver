#include "hpserver.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace {
constexpr std::uint32_t kClientEvents = EPOLLIN | EPOLLET | EPOLLRDHUP;
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
    if (client_fd >= 0 && static_cast<std::size_t>(client_fd) < connections_.size()) {
        connections_[client_fd].reset();
    }
}

void hpserver::init() {
    connections_.resize(MAX_FD);
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

    std::cout << "Server listening on port " << port << std::endl;

    while (true) {
        int nfds = poller_.wait(events, MAX_EVENTS, -1);
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
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("accept");
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
                }
            } else {
                const int client_fd = events[i].data.fd;
                const std::uint32_t ev = events[i].events;

                if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                    close_client(client_fd);
                    continue;
                }

                if (handle_client(client_fd) == -1) {
                    std::cerr << "Failed to handle client fd " << client_fd << std::endl;
                    close_client(client_fd);
                }
            }
        }
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

    http_conn& conn = *connections_[client_fd];
    if (conn.read_from_socket() < 0) {
        return -1;
    }

    if (!conn.parse_available_data()) {
        std::cerr << "HTTP parse error: " << conn.parse_error() << std::endl;
        return -1;
    }

    if (!conn.is_message_complete()) {
        return 0;
    }

    const auto& req = conn.request();
    std::cout << "HTTP request complete: method=" << req.method << " url=" << req.url
              << " host=" << req.host << " port=" << req.port << std::endl;

    // Keep parsing for the next keep-alive request. Forwarding will be added next.
    conn.reset_for_next_message();
    return 0;
}