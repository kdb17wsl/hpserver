#include "socket_ops.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

socket_ops::socket_ops(int domain, int type, int protocol) {
    fd_ = ::socket(domain, type, protocol);
    if (fd_ == -1) {
        perror("socket");
    }
}

socket_ops& socket_ops::operator=(socket_ops&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) {
            close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

socket_ops::~socket_ops() {
    if (fd_ != -1) {
        close(fd_);
    }
}

int socket_ops::bind(const struct sockaddr* addr, socklen_t addrlen) const {
    int ret = ::bind(fd_, addr, addrlen);
    if (ret == -1) {
        perror("bind");
    }
    return ret;
}

int socket_ops::listen(int backlog) const {
    int ret = ::listen(fd_, backlog);
    if (ret == -1) {
        perror("listen");
    }
    return ret;
}

int socket_ops::accept(struct sockaddr* addr, socklen_t* addrlen) const {
    int ret = ::accept(fd_, addr, addrlen);
    if (ret == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        perror("accept");
    }
    return ret;
}

int socket_ops::connect(const struct sockaddr* addr, socklen_t addrlen) const {
    int ret = ::connect(fd_, addr, addrlen);
    if (ret == -1) {
        perror("connect");
    }
    return ret;
}

ssize_t socket_ops::read(void* buf, size_t count) const {
    ssize_t ret = ::read(fd_, buf, count);
    if (ret == -1) {
        perror("read");
    }
    return ret;
}

ssize_t socket_ops::write(const void* buf, size_t count) const {
    ssize_t ret = ::write(fd_, buf, count);
    if (ret == -1) {
        perror("write");
    }
    return ret;
}

int socket_ops::set_option(int level, int optname, const void* optval, socklen_t optlen) const {
    int ret = ::setsockopt(fd_, level, optname, optval, optlen);
    if (ret == -1) {
        perror("setsockopt");
    }
    return ret;
}
