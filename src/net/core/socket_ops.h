#pragma once

#include <sys/socket.h>
#include <unistd.h>

class socket_ops {
private:
    int fd_;

public:
    socket_ops() : fd_(-1) {}
    socket_ops(int f_) : fd_(f_) {}
    socket_ops(int domain, int type, int protocol);
    socket_ops(const socket_ops&) = delete;
    socket_ops& operator=(const socket_ops&) = delete;
    socket_ops(socket_ops&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    socket_ops& operator=(socket_ops&& other) noexcept;
    ~socket_ops();

    int get_fd() const { return fd_; }
    int bind(const struct sockaddr* addr, socklen_t addrlen) const;
    int listen(int backlog) const;
    int accept(struct sockaddr* addr, socklen_t* addrlen) const;
    int connect(const struct sockaddr* addr, socklen_t addrlen) const;
    ssize_t read(void* buf, size_t count) const;
    ssize_t write(const void* buf, size_t count) const;
    int set_option(int level, int optname, const void* optval, socklen_t optlen) const;
};
